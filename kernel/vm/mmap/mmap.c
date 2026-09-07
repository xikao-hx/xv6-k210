#include "fcntl.h"
#include "kalloc.h"
#include "mmap.h"
#include "printf.h"
#include "proc.h"
#include "string.h"
#include "vm.h"

#include "mmap_internal.h"

static struct vma_area *
vma_find_free(struct proc *p)
{
  for(int i = 0; i < NVMA; i++){
    if(!p->vmas[i].used)
      return &p->vmas[i];
  }
  return 0;
}

struct vma_area *
mmap_vma_find(struct proc *p, uint64 va)
{
  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];
    if(vma->used && vma->start <= va && va < vma->end)
      return vma;
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

static struct mmap_object *
mmap_object_alloc(enum vma_type type)
{
  struct mmap_object *object = kmalloc(sizeof(*object));

  if(object == 0)
    return 0;
  memset(object, 0, sizeof(*object));
  initlock(&object->lock, "mmap_object");
  object->refcnt = 1;
  object->type = type;
  return object;
}

struct mmap_object *
mmap_object_create(enum vma_type type, struct file *file, int flags,
                   uint64 size)
{
  struct mmap_object *object = mmap_object_alloc(type);
  int ret;

  if(object == 0)
    return 0;
  if(type == VMA_FILE)
    ret = mmap_file_object_init(object, file);
  else if(type == VMA_ANON)
    ret = mmap_anon_object_init(object, flags);
  else if(type == VMA_KBUF)
    ret = mmap_kbuf_object_init(object, file, size);
  else
    ret = -1;
  if(ret < 0){
    kfree(object);
    return 0;
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

void
mmap_object_put(struct mmap_object *object)
{
  int destroy = 0;

  acquire(&object->lock);
  if(object->refcnt < 1)
    panic("mmap_object_put");
  object->refcnt--;
  if(object->refcnt == 0)
    destroy = 1;
  release(&object->lock);

  if(destroy){
    if(object->type == VMA_FILE)
      mmap_file_object_destroy(object);
    else if(object->type == VMA_ANON)
      mmap_anon_object_destroy(object);
    else if(object->type == VMA_KBUF)
      mmap_kbuf_object_destroy(object);
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

uint64
mmap_map_create(struct proc *p, uint64 addr, uint64 length, int prot,
                int flags, enum vma_type type, struct mmap_object *object,
                const struct vma_ops *ops, uint64 offset)
{
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
  vma->ops = ops;
  return start;
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
  int pte_flags = PTE_U;

  if(va >= MAXUVA || (vma = mmap_vma_find(p, va)) == 0)
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
  if(vma->ops == 0 || vma->ops->fault == 0)
    return -1;
  if(vma->ops->fault(vma, page, &mem) < 0 || mem == 0)
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
  if(upg2ukpg(p->pagetable, p->kpagetable,
              page, page + PGSIZE) < 0)
    panic("vm_fault: upg2ukpg");
  sfence_vma();
  return 0;
}

static void
vma_unmap_pages(struct proc *p, uint64 start, uint64 end)
{
  for(uint64 page = start; page < end; page += PGSIZE){
    uvmunmap(p->kpagetable, page, 1, 0);
    uvmunmap(p->pagetable, page, 1, 1);
  }
}

int
mmap_populate(struct proc *p, struct vma_area *vma)
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
      continue;
    if(vma->ops->fault(vma, page, &mem) < 0 || mem == 0)
      goto rollback;
    if(mappages(p->pagetable, page, PGSIZE, (uint64)mem, pte_flags) < 0){
      kfree_page(mem);
      goto rollback;
    }
    if(upg2ukpg(p->pagetable, p->kpagetable,
                page, page + PGSIZE) < 0)
      panic("mmap_populate: upg2ukpg");
  }
  sfence_vma();
  vma->ops = 0;
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

  for(int i = 0; i < NVMA; i++){
    struct vma_area *vma = &p->vmas[i];
    uint64 start;
    uint64 finish;

    if(!vma_overlaps(vma, addr, end))
      continue;
    start = addr > vma->start ? addr : vma->start;
    finish = end < vma->end ? end : vma->end;
    if(mmap_file_writeback_range(p, vma, start, finish) < 0)
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
    vma_unmap_pages(p, start, finish);

    if(start == old.start && finish == old.end){
      memset(vma, 0, sizeof(*vma));
      if(old.object)
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
      mmap_file_writeback_range(p, vma, vma->start, vma->end);
    if(p->pagetable && p->kpagetable)
      vma_unmap_pages(p, vma->start, vma->end);
    if(vma->object)
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

      if(pte == 0 || !(*pte & PTE_V))
        continue;
      pa = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);
      if((vma->flags & MAP_PRIVATE) && (flags & PTE_W)){
        flags = (flags | PTE_COW) & ~PTE_W;
        *pte = PA2PTE(pa) | flags;
        if(upg2ukpg(parent->pagetable, parent->kpagetable,
                    page, page + PGSIZE) < 0)
          panic("vma_fork: parent upg2ukpg");
      }
      if(mappages(child->pagetable, page, PGSIZE, pa, flags) < 0)
        goto bad;
      kaddquota((void *)pa);
      if(upg2ukpg(child->pagetable, child->kpagetable,
                  page, page + PGSIZE) < 0)
        panic("vma_fork: child upg2ukpg");
    }
  }
  sfence_vma();
  return 0;

bad:
  vma_destroy_all(child);
  sfence_vma();
  return -1;
}
