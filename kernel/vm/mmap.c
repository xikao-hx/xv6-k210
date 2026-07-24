#include "fcntl.h"
#include "file.h"
#include "kalloc.h"
#include "kbuf.h"
#include "mmap.h"
#include "mmap_file.h"
#include "printf.h"
#include "proc.h"
#include "string.h"
#include "vm.h"

#define MAP_FAILED ((uint64)-1)

struct anon_page {
  uint64 index;
  uint64 pa;
  struct anon_page *next;
};

struct anon_object {
  struct spinlock lock;
  struct anon_page *pages;
};

static struct vma_area *
vma_find(struct proc *p, uint64 va)
{
  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];
    if(vma->used && vma->start <= va && va < vma->end)
      return vma;
  }
  return 0;
}

static struct vma_area *
vma_find_free(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(!p->vmas[i].used)
      return &p->vmas[i];
  }
  return 0;
}

static int
vma_overlaps(struct vma_area *vma, uint64 start, uint64 end)
{
  return vma->used && start < vma->end && vma->start < end;
}

static uint64
vma_find_address(struct proc *p, uint64 length)
{
  uint64 top = MMAP_TOP;

  while(top >= length){
    uint64 start = top - length;
    struct vma_area *overlap = 0;

    if(start < PGROUNDUP(p->sz))
      return 0;
    for(int i = 0; i < NVMA; i++){
      if(vma_overlaps(&p->vmas[i], start, top)){
        overlap = &p->vmas[i];
        break;
      }
    }
    if(overlap == 0)
      return start;
    top = overlap->start;
  }
  return 0;
}

static struct anon_object *
anon_object_create(void)
{
  struct anon_object *anon = kmalloc(sizeof(*anon));

  if(anon == 0)
    return 0;
  memset(anon, 0, sizeof(*anon));
  initlock(&anon->lock, "anon_object");
  return anon;
}

static void
anon_object_destroy(struct anon_object *anon)
{
  struct anon_page *page;

  acquire(&anon->lock);
  page = anon->pages;
  anon->pages = 0;
  release(&anon->lock);

  while(page){
    struct anon_page *next = page->next;
    kfree_page((void *)page->pa);
    kfree(page);
    page = next;
  }
  kfree(anon);
}

// Return a shared anonymous page with one reference owned by the caller's PTE.
static void *
anon_page_get(struct anon_object *anon, uint64 index)
{
  struct anon_page *page;
  struct anon_page *candidate;
  void *mem;

  acquire(&anon->lock);
  for(page = anon->pages; page; page = page->next){
    if(page->index == index){
      kaddquota((void *)page->pa);
      release(&anon->lock);
      return (void *)page->pa;
    }
  }
  release(&anon->lock);

  mem = kalloc_page();
  if(mem == 0)
    return 0;
  memset(mem, 0, PGSIZE);
  candidate = kmalloc(sizeof(*candidate));
  if(candidate == 0){
    kfree_page(mem);
    return 0;
  }
  candidate->index = index;
  candidate->pa = (uint64)mem;

  acquire(&anon->lock);
  for(page = anon->pages; page; page = page->next){
    if(page->index == index){
      kaddquota((void *)page->pa);
      release(&anon->lock);
      kfree(candidate);
      kfree_page(mem);
      return (void *)page->pa;
    }
  }
  candidate->next = anon->pages;
  anon->pages = candidate;
  // kalloc's initial reference belongs to the object; add the PTE reference.
  kaddquota(mem);
  release(&anon->lock);
  return mem;
}

static struct mmap_object *
mmap_object_create(enum vma_type type, struct file *file, int flags,
                   struct kbuf *kbuf)
{
  struct mmap_object *object = kmalloc(sizeof(*object));

  if(object == 0)
    return 0;
  memset(object, 0, sizeof(*object));
  initlock(&object->lock, "mmap_object");
  object->refcnt = 1;
  object->type = type;
  if(type == VMA_FILE)
    object->file = filedup(file);
  if(type == VMA_ANON && (flags & MAP_SHARED)){
    object->anon = anon_object_create();
    if(object->anon == 0){
      kfree(object);
      return 0;
    }
  }
  if(type == VMA_KBUF){
    object->kbuf = kbuf;
    kbuf_get(kbuf);
  }
  return object;
}

static void
mmap_object_get(struct mmap_object *object)
{
  acquire(&object->lock);
  if(object->refcnt < 1)
    panic("mmap_object_get");
  object->refcnt++;
  release(&object->lock);
}

static void
mmap_object_put(struct mmap_object *object)
{
  int destroy = 0;
  struct file *file = 0;

  acquire(&object->lock);
  if(object->refcnt < 1)
    panic("mmap_object_put");
  object->refcnt--;
  if(object->refcnt == 0){
    destroy = 1;
    file = object->file;
  }
  release(&object->lock);

  if(destroy){
    if(file)
      fileclose(file);
    if(object->anon)
      anon_object_destroy(object->anon);
    if(object->kbuf)
      kbuf_put(object->kbuf);
    kfree(object);
  }
}

uint64
vma_heap_limit(struct proc *p)
{
  uint64 limit = MMAP_TOP;

  for(int i = 0; i < NVMA; i++){
    if(p->vmas[i].used && p->vmas[i].start < limit)
      limit = p->vmas[i].start;
  }
  return limit;
}

static uint64
vma_map_create(struct proc *p, uint64 addr, uint64 length, int prot,
               int flags, enum vma_type type, struct file *file,
               struct kbuf *kbuf, uint64 offset)
{
  struct mmap_object *object;
  struct vma_area *vma;
  uint64 map_length;
  uint64 start;

  if(addr != 0 || length == 0 || length > MAXVA)
    return MAP_FAILED;
  if((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return MAP_FAILED;
  if((prot & PROT_WRITE) && !(prot & PROT_READ))
    return MAP_FAILED;
  int sharing = flags & (MAP_PRIVATE | MAP_SHARED);
  if(sharing != MAP_PRIVATE && sharing != MAP_SHARED)
    return MAP_FAILED;

  map_length = PGROUNDUP(length);
  if(map_length < length)
    return MAP_FAILED;
  if((vma = vma_find_free(p)) == 0)
    return MAP_FAILED;
  if((start = vma_find_address(p, map_length)) == 0)
    return MAP_FAILED;
  if(start + length < start)
    return MAP_FAILED;
  if((object = mmap_object_create(type, file, flags, kbuf)) == 0)
    return MAP_FAILED;

  memset(vma, 0, sizeof(*vma));
  vma->used = 1;
  vma->type = type;
  vma->start = start;
  vma->end = start + map_length;
  vma->valid_end = start + length;
  vma->offset = offset;
  vma->prot = prot;
  vma->flags = flags;
  vma->object = object;
  return start;
}

uint64
vma_map_file(struct proc *p, uint64 addr, uint64 length, int prot,
             int flags, struct file *file, uint64 offset)
{
  if(flags != MAP_PRIVATE && flags != MAP_SHARED)
    return MAP_FAILED;
  if((offset % PGSIZE) != 0 || offset > 0xffffffffUL)
    return MAP_FAILED;
  if(file == 0 || file->type != FD_ENTRY || !file->readable)
    return MAP_FAILED;
  if(flags == MAP_SHARED && (prot & PROT_WRITE) && !file->writable)
    return MAP_FAILED;
  return vma_map_create(p, addr, length, prot, flags, VMA_FILE,
                        file, 0, offset);
}

uint64
vma_map_anon(struct proc *p, uint64 addr, uint64 length, int prot,
             int flags)
{
  if(flags != (MAP_PRIVATE | MAP_ANONYMOUS) &&
     flags != (MAP_SHARED | MAP_ANONYMOUS))
    return MAP_FAILED;
  return vma_map_create(p, addr, length, prot, flags, VMA_ANON,
                        0, 0, 0);
}

uint64
vma_map_device(struct proc *p, uint64 addr, uint64 length, int prot,
               int flags, struct file *file, uint64 offset)
{
  struct kbuf *kbuf;

  if(flags != MAP_SHARED || file == 0 || file->type != FD_DEVICE ||
     file->ops == 0 || file->ops->mmap == 0)
    return MAP_FAILED;
  if((prot & PROT_READ) && !file->readable)
    return MAP_FAILED;
  if((prot & PROT_WRITE) && !file->writable)
    return MAP_FAILED;
  kbuf = file->ops->mmap(file, offset, length, prot, flags);
  if(kbuf == 0)
    return MAP_FAILED;
  return vma_map_create(p, addr, length, prot, flags, VMA_KBUF,
                        0, kbuf, offset);
}

static int
vma_access_allowed(struct vma_area *vma, int access)
{
  if(access == VM_FAULT_READ)
    return (vma->prot & PROT_READ) != 0;
  if(access == VM_FAULT_WRITE)
    return (vma->prot & PROT_WRITE) != 0;
  if(access == VM_FAULT_EXEC)
    return (vma->prot & PROT_EXEC) != 0;
  return 0;
}

int
vm_fault(struct proc *p, uint64 va, int access)
{
  struct vma_area *vma;
  uint64 page;
  pte_t *pte;
  void *mem;
  uint64 read_length;
  uint64 file_offset;
  uint64 anon_index;
  uint64 kbuf_index;
  uint64 dirty_length;
  int cached_file_page = 0;
  int pte_flags = PTE_U;

  if(va >= MAXVA || (vma = vma_find(p, va)) == 0)
    return -1;
  if(!vma_access_allowed(vma, access))
    return -1;

  page = PGROUNDDOWN(va);
  pte = walk(p->pagetable, page, 0);
  if(pte && (*pte & PTE_V)){
    if(access == VM_FAULT_WRITE && (*pte & PTE_COW))
      return uvmcowmalloc(p->pagetable, page) ? 0 : -1;
    return 0;
  }

  if(vma->type == VMA_FILE){
    read_length = PGSIZE;
    if(page + read_length > vma->valid_end)
      read_length = vma->valid_end > page ? vma->valid_end - page : 0;
    file_offset = vma->offset + (page - vma->start);
    if(file_offset < vma->offset || file_offset > 0xffffffffUL)
      return -1;

    if(vma->flags & MAP_SHARED){
      dirty_length = (vma->prot & PROT_WRITE) ? read_length : 0;
      mem = mmap_file_page_get(vma->object->file, file_offset,
                               dirty_length);
      cached_file_page = 1;
    } else {
      mem = kalloc_page();
      if(mem)
        memset(mem, 0, PGSIZE);
      if(mem && read_length > 0 &&
         fileread_at(vma->object->file, (uint64)mem, file_offset,
                     read_length) < 0){
        kfree_page(mem);
        mem = 0;
      }
    }
  } else if(vma->type == VMA_KBUF){
    kbuf_index = (vma->offset + page - vma->start) / PGSIZE;
    mem = kbuf_page_get(vma->object->kbuf, kbuf_index);
  } else if(vma->type == VMA_ANON && (vma->flags & MAP_SHARED)){
    anon_index = (vma->offset + page - vma->start) / PGSIZE;
    mem = anon_page_get(vma->object->anon, anon_index);
  } else {
    mem = kalloc_page();
    if(mem)
      memset(mem, 0, PGSIZE);
  }
  if(mem == 0)
    return -1;

  if(vma->prot & PROT_READ)
    pte_flags |= PTE_R;
  if(vma->prot & PROT_WRITE)
    pte_flags |= PTE_W;
  if(vma->prot & PROT_EXEC)
    pte_flags |= PTE_X;
  if(mappages(p->pagetable, page, PGSIZE, (uint64)mem, pte_flags) < 0){
    if(cached_file_page)
      mmap_file_page_put(vma->object->file, file_offset, (uint64)mem);
    else
      kfree_page(mem);
    return -1;
  }
  upg2ukpg(p->pagetable, p->kpagetable, page, page + PGSIZE);
  sfence_vma();
  return 0;
}

static int
vma_writeback_page(struct vma_area *vma, pagetable_t pagetable,
                   uint64 page)
{
  pte_t *pte;
  uint64 length;
  uint64 file_offset;

  if(vma->type != VMA_FILE ||
     !(vma->flags & MAP_SHARED) || !(vma->prot & PROT_WRITE))
    return 0;
  if((pte = walk(pagetable, page, 0)) == 0 || !(*pte & PTE_V))
    return 0;

  length = PGSIZE;
  if(page + length > vma->valid_end)
    length = vma->valid_end > page ? vma->valid_end - page : 0;
  if(length == 0)
    return 0;
  file_offset = vma->offset + (page - vma->start);
  if(file_offset < vma->offset || file_offset > 0xffffffffUL)
    return -1;
  return mmap_file_page_writeback(vma->object->file, file_offset,
                                  PTE2PA(*pte), length);
}

static int
vma_writeback_range(struct proc *p, struct vma_area *vma,
                    uint64 start, uint64 end)
{
  for(uint64 page = start; page < end; page += PGSIZE){
    if(vma_writeback_page(vma, p->pagetable, page) < 0)
      return -1;
  }
  return 0;
}

static void
vma_unmap_pages(struct proc *p, struct vma_area *vma,
                uint64 start, uint64 end)
{
  for(uint64 page = start; page < end; page += PGSIZE){
    pte_t *pte = walk(p->pagetable, page, 0);
    uint64 pa = pte && (*pte & PTE_V) ? PTE2PA(*pte) : 0;

    uvmunmap(p->kpagetable, page, 1, 0);
    if(vma->type == VMA_FILE && (vma->flags & MAP_SHARED)){
      uvmunmap(p->pagetable, page, 1, 0);
      if(pa){
        uint64 file_offset = vma->offset + (page - vma->start);
        mmap_file_page_put(vma->object->file, file_offset, pa);
      }
    } else {
      uvmunmap(p->pagetable, page, 1, 1);
    }
  }
}

int
vma_unmap(struct proc *p, uint64 addr, uint64 length)
{
  struct vma_area *split_slot = 0;
  int found = 0;
  int needs_split = 0;
  uint64 end;

  if(length == 0 || (addr % PGSIZE) != 0 || addr >= MMAP_TOP)
    return -1;
  if(addr + length < addr)
    return -1;
  end = PGROUNDUP(addr + length);
  if(end < addr || end > MMAP_TOP)
    return -1;

  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];
    if(!vma_overlaps(vma, addr, end))
      continue;
    found = 1;
    if(vma->start < addr && end < vma->end)
      needs_split = 1;
  }
  if(!found)
    return -1;
  if(needs_split && (split_slot = vma_find_free(p)) == 0)
    return -1;

  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];
    uint64 start;
    uint64 finish;

    if(!vma_overlaps(vma, addr, end))
      continue;
    start = addr > vma->start ? addr : vma->start;
    finish = end < vma->end ? end : vma->end;
    if(vma_writeback_range(p, vma, start, finish) < 0)
      return -1;
  }

  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];
    struct vma_area old;
    uint64 start;
    uint64 finish;

    if(!vma_overlaps(vma, addr, end))
      continue;
    old = *vma;
    start = addr > old.start ? addr : old.start;
    finish = end < old.end ? end : old.end;
    vma_unmap_pages(p, &old, start, finish);

    if(start == old.start && finish == old.end){
      memset(vma, 0, sizeof(*vma));
      mmap_object_put(old.object);
    } else if(start == old.start){
      vma->start = finish;
      vma->offset = old.offset + (finish - old.start);
    } else if(finish == old.end){
      vma->end = start;
      if(vma->valid_end > start)
        vma->valid_end = start;
    } else {
      *split_slot = old;
      split_slot->start = finish;
      split_slot->offset = old.offset + (finish - old.start);
      mmap_object_get(old.object);
      vma->end = start;
      if(vma->valid_end > start)
        vma->valid_end = start;
      split_slot = 0;
    }
  }
  sfence_vma();
  return 0;
}

void
vma_destroy_all(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];

    if(!vma->used)
      continue;
    if(p->pagetable)
      vma_writeback_range(p, vma, vma->start, vma->end);
    if(p->pagetable && p->kpagetable)
      vma_unmap_pages(p, vma, vma->start, vma->end);
    mmap_object_put(vma->object);
    memset(vma, 0, sizeof(*vma));
  }
  sfence_vma();
}

int
vma_fork(struct proc *parent, struct proc *child)
{
  for(int i = 0; i < NVMA; i++){
    if(!parent->vmas[i].used)
      continue;
    child->vmas[i] = parent->vmas[i];
    mmap_object_get(child->vmas[i].object);
  }

  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &parent->vmas[i];

    if(!vma->used)
      continue;
    for(uint64 page = vma->start; page < vma->end; page += PGSIZE){
      pte_t *pte = walk(parent->pagetable, page, 0);
      uint flags;
      uint64 pa;
      uint64 file_offset = 0;
      uint64 dirty_length = 0;
      int cached_file_page;

      if(pte == 0 || !(*pte & PTE_V))
        continue;
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      cached_file_page = vma->type == VMA_FILE &&
                         (vma->flags & MAP_SHARED);
      if((vma->flags & MAP_PRIVATE) && (flags & PTE_W)){
        flags = (flags | PTE_COW) & ~PTE_W;
        *pte = PA2PTE(pa) | flags;
        upg2ukpg(parent->pagetable, parent->kpagetable,
                 page, page + PGSIZE);
      }
      if(cached_file_page){
        file_offset = vma->offset + (page - vma->start);
        if(vma->prot & PROT_WRITE){
          dirty_length = PGSIZE;
          if(page + dirty_length > vma->valid_end)
            dirty_length = vma->valid_end > page ?
                           vma->valid_end - page : 0;
        }
        if(mmap_file_page_hold(vma->object->file, file_offset, pa,
                               dirty_length) < 0)
          goto bad;
      }
      if(mappages(child->pagetable, page, PGSIZE, pa, flags) < 0){
        if(cached_file_page)
          mmap_file_page_put(vma->object->file, file_offset, pa);
        goto bad;
      }
      if(!cached_file_page)
        kaddquota((void *)pa);
      upg2ukpg(child->pagetable, child->kpagetable,
               page, page + PGSIZE);
    }
  }
  sfence_vma();
  return 0;

bad:
  vma_destroy_all(child);
  sfence_vma();
  return -1;
}
