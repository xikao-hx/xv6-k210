#include "fcntl.h"
#include "file.h"
#include "kalloc.h"
#include "mmap.h"
#include "printf.h"
#include "proc.h"
#include "string.h"
#include "vm.h"
#include "kbuf.h"

#define MAP_FAILED ((uint64)-1)

// ------ VMA Helper Function ------

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

// alloc virtual address
static uint64
vma_find_address(struct proc *p, uint64 length)
{
  uint64 bottom = PGROUNDUP(p->sz);
  uint64 top = MMAP_TOP;

  if(bottom < p->sz || bottom > top)
    return 0;
  while(length <= top - bottom){
    uint64 start = top - length;
    struct vma_area *overlap = 0;

    for(int i = 0; i < NVMA; i++){
      if(vma_overlaps(&p->vmas[i], start, top) &&
         (overlap == 0 || p->vmas[i].start > overlap->start))
        overlap = &p->vmas[i];
    }
    if(overlap == 0)
      return start;
    top = overlap->start;
  }
  return 0;
}

// ------ Unified ANON Object Creation ------

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

  // Why this second traversal?
  // 1. Another thread may have created the same page during our allocation
  // 2. We must prevent duplicate pages for the same index
  // 3. We need to atomically insert our page if still missing
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

// ------ Unified Fault Handler (vma_ops) ------

// Zero a fresh page, then read the file range intersecting the faulted page.
// The core-mm file fault handler; used by MAP_PRIVATE/MAP_SHARED file VMAs.
static int
file_vma_fault(struct vma_area *vma, uint64 page, void **mem)
{
  uint64 read_length;
  uint64 file_offset;
  void *mem_page;

  mem_page = kalloc_page();
  if(mem_page == 0)
    return -1;
  memset(mem_page, 0, PGSIZE);

  read_length = PGSIZE;
  if(page + read_length > vma->valid_end)
    read_length = vma->valid_end > page ? vma->valid_end - page : 0;
  file_offset = vma->offset + (page - vma->start);
  if(file_offset < vma->offset || file_offset > 0xffffffffUL){
    kfree_page(mem_page);
    return -1;
  }
  if(read_length > 0 &&
     fileread_at(vma->object->file, (uint64)mem_page, file_offset,
                 read_length) < 0){
    kfree_page(mem_page);
    return -1;
  }
  *mem = mem_page;
  return 0;
}

// Anonymous fault: shared maps the anon object page, private allocates a
// fresh zero page.  The core-mm anonymous fault handler.
static int
anon_vma_fault(struct vma_area *vma, uint64 page, void **mem)
{
  uint64 anon_index;
  void *mem_page;

  if(vma->flags & MAP_SHARED){
    anon_index = (vma->offset + page - vma->start) / PGSIZE;
    mem_page = anon_page_get(vma->object->anon, anon_index);
  } else {
    mem_page = kalloc_page();
    if(mem_page)
      memset(mem_page, 0, PGSIZE);
  }
  if(mem_page == 0)
    return -1;
  *mem = mem_page;
  return 0;
}

static int
kbufdev_vma_fault(struct vma_area *vma, uint64 page, void **mem)
{
  struct kbuf *kbuf = vma->data;
  uint64 index;

  if(kbuf == 0)
    return -1;
  index = (vma->offset + page - vma->start) / PGSIZE;
  *mem = kbuf_page_get(kbuf, index);
  return *mem ? 0 : -1;
}

static const struct vma_ops file_vma_ops = {
  .fault = file_vma_fault,
};

static const struct vma_ops anon_vma_ops = {
  .fault = anon_vma_fault,
};

static const struct vma_ops kbufdev_vma_ops = {
  .fault = kbufdev_vma_fault,
};

// ------ Unified MMAP Object Creation ------

static struct mmap_object *
mmap_object_create(enum vma_type type, struct file *file, int flags)
{
  struct mmap_object *object = kmalloc(sizeof(*object));

  if(object == 0)
    return 0;
  memset(object, 0, sizeof(*object));
  initlock(&object->lock, "mmap_object");
  object->refcnt = 1;
  object->type = type;
  // A device mapping holds the device file (not the kbuf) so the buffer
  // stays alive for the object's lifetime via fileclose->device close.
  if(type == VMA_FILE || type == VMA_KBUF)
    object->file = filedup(file);
  if (type == VMA_ANON && (flags & MAP_SHARED)) {
    object->anon = anon_object_create();
    if (object->anon == 0) {
      kfree(object);
      return 0;
    }
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
  if(object->refcnt == 0) {
    destroy = 1;
    file = object->file;
  }
  release(&object->lock);

  if(destroy) {
    if(file)
      fileclose(file);
    if(object->anon)
      anon_object_destroy(object->anon);
    kfree(object);
  }
}

// ------ Unified VMA Creation ------

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
               uint64 offset)
{
  struct mmap_object *object;
  struct vma_area *vma;
  uint64 map_length;
  uint64 start;

  if(addr != 0 || length == 0 || length > MMAP_TOP)
    return MAP_FAILED;
  if((prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0)
    return MAP_FAILED;
  if((prot & PROT_WRITE) && !(prot & PROT_READ))
    return MAP_FAILED;
  int sharing = flags & (MAP_PRIVATE | MAP_SHARED);
  if (sharing != MAP_PRIVATE && sharing != MAP_SHARED)
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
  if((object = mmap_object_create(type, file, flags)) == 0)
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
  // File and anonymous faults are core-mm logic; a device VMA gets its
  // ops from the device's mmap callback after this returns.
  vma->ops = 0;
  vma->data = 0;
  if(type == VMA_FILE)
    vma->ops = &file_vma_ops;
  else if(type == VMA_ANON)
    vma->ops = &anon_vma_ops;
  else
     vma->ops = &kbufdev_vma_ops;
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
                        file, offset);
}

uint64
vma_map_anon(struct proc *p, uint64 addr, uint64 length, int prot,
             int flags)
{
  if(flags != (MAP_PRIVATE | MAP_ANONYMOUS) &&
      flags != (MAP_SHARED | MAP_ANONYMOUS))
    return MAP_FAILED;
  return vma_map_create(p, addr, length, prot, flags, VMA_ANON, 0, 0);
}

uint64
vma_map_device(struct proc *p, uint64 addr, uint64 length, int prot,
               int flags, struct file *file, uint64 offset)
{
  struct vma_area *vma;
  uint64 start;

  if(flags != MAP_SHARED || file == 0 || file->type != FD_DEVICE ||
     file->ops == 0 || file->ops->mmap == 0)
    return MAP_FAILED;
  if((prot & PROT_READ) && !file->readable)
    return MAP_FAILED;
  if((prot & PROT_WRITE) && !file->writable)
    return MAP_FAILED;
  // Create the VMA/object first (Linux mmap_region shape), then let the
  // device's mmap callback validate against the VMA and configure ops/data.
  start = vma_map_create(p, addr, length, prot, flags, VMA_KBUF,
                         file, offset);
  if(start == MAP_FAILED)
    return MAP_FAILED;
  vma = vma_find(p, start);
  if(vma == 0 || vma->ops == 0 || vma->ops->fault == 0 ||
        file->ops->mmap(file, vma, offset) < 0){
    vma_unmap(p, start, length);
    return MAP_FAILED;
  }

  return start;
}

// ------ VM Fault Handling ------

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
  int pte_flags = PTE_U;

  if(va >= MAXUVA || (vma = vma_find(p, va)) == 0)
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

  // Unified dispatch: the backing type decides how the page is produced.
  // File/anon use core-mm handlers; a device uses the ops its driver set.
  if(vma->ops == 0 || vma->ops->fault == 0)
    return -1;
  if(vma->ops->fault(vma, page, &mem) < 0)
    return -1;
  if(mem == 0)
    return -1;

  if(vma->prot & PROT_READ)
    pte_flags |= PTE_R;
  if(vma->prot & PROT_WRITE)
    pte_flags |= PTE_W;
  if(vma->prot & PROT_EXEC)
    pte_flags |= PTE_X;
  if(mappages(p->pagetable, page, PGSIZE, (uint64)mem, pte_flags) < 0){
    kfree_page(mem);
    return -1;
  }
  upg2ukpg(p->pagetable, p->kpagetable, page, page + PGSIZE);
  sfence_vma();
  return 0;
}

// ------ WriteBack Handling------

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
  return filewrite_at(vma->object->file, PTE2PA(*pte), file_offset,
                      length) < 0 ? -1 : 0;
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

// ------ Unified Lifecycle Management for VMA (Virtual Memory Areas) ------

static void
vma_unmap_pages(struct proc *p, uint64 start, uint64 end)
{
  for(uint64 page = start; page < end; page += PGSIZE){
    uvmunmap(p->kpagetable, page, 1, 0);
    uvmunmap(p->pagetable, page, 1, 1);
  }
}

// Export page-table work to drivers, modeled after Linux remap_pfn_range:
// a device driver calls this from its f_op->mmap to install every page of a
// page-backed VMA up front, so the mapping is fully resident when mmap
// returns.  On success the fault handler is dropped (vma->ops = 0); any
// later fault on the range is a hard error.  On failure, already-installed
// pages are rolled back and the driver should fail the mmap request.
int
vma_populate(struct proc *p, struct vma_area *vma)
{
  uint64 page;
  pte_t *pte;
  void *mem;
  int pte_flags = PTE_U;

  if(vma->ops == 0 || vma->ops->fault == 0)
    return -1;
  if(vma->prot & PROT_READ)
    pte_flags |= PTE_R;
  if(vma->prot & PROT_WRITE)
    pte_flags |= PTE_W;
  if(vma->prot & PROT_EXEC)
    pte_flags |= PTE_X;

  for(page = vma->start; page < vma->end; page += PGSIZE){
    pte = walk(p->pagetable, page, 0);
    if(pte && (*pte & PTE_V))
      continue;  // already resident
    if(vma->ops->fault(vma, page, &mem) < 0 || mem == 0)
      goto rollback;
    if(mappages(p->pagetable, page, PGSIZE, (uint64)mem, pte_flags) < 0){
      kfree_page(mem);
      goto rollback;
    }
    upg2ukpg(p->pagetable, p->kpagetable, page, page + PGSIZE);
  }
  sfence_vma();
  vma->ops = 0;  // fully resident: no fault handler, faults are errors
  return 0;

rollback:
  vma_unmap_pages(p, vma->start, page);
  return -1;
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
  if(length > MMAP_TOP - addr)
    return -1;
  end = PGROUNDUP(addr + length);
  if(end > MMAP_TOP)
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

  // write back
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

  // vma split
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
    vma_unmap_pages(p, start, finish);

    if(start == old.start && finish == old.end){
      memset(vma, 0, sizeof(*vma));
      mmap_object_put(old.object);
    } else if(start == old.start){
      vma->start = finish;
      // VMA splitting: when start changes, the file offset must also change; 
      // otherwise, the corresponding file offset would be unknown.
      vma->offset = old.offset + (finish - old.start);
    } else if(finish == old.end){
      vma->end = start;
      if(vma->valid_end > start)
        vma->valid_end = start;
    } else {  // the latter vma
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
      vma_unmap_pages(p, vma->start, vma->end);
    mmap_object_put(vma->object);
    memset(vma, 0, sizeof(*vma));
  }
  sfence_vma();
}

int
vma_fork(struct proc *parent, struct proc *child)
{
  // child copy parent's vma
  for(int i = 0; i < NVMA; i++){
    if(!parent->vmas[i].used)
      continue;
    child->vmas[i] = parent->vmas[i];
    mmap_object_get(child->vmas[i].object);
  }

  // child add pagetable map
  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &parent->vmas[i];

    if(!vma->used)
      continue;
    for(uint64 page = vma->start; page < vma->end; page += PGSIZE){
      pte_t *pte = walk(parent->pagetable, page, 0);
      uint flags;
      uint64 pa;

      if(pte == 0 || !(*pte & PTE_V))
        continue;
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      // PRIVATE mapping: share page via COW on fork, 
      // child and parent proccess independent page
      if((vma->flags & MAP_PRIVATE) && (flags & PTE_W)){
        flags = (flags | PTE_COW) & ~PTE_W;
        *pte = PA2PTE(pa) | flags;
        upg2ukpg(parent->pagetable, parent->kpagetable,
                 page, page + PGSIZE);
      }
      if(mappages(child->pagetable, page, PGSIZE, pa, flags) < 0)
        goto bad;
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
