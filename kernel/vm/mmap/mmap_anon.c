#include "fcntl.h"
#include "kalloc.h"
#include "mmap.h"
#include "proc.h"
#include "string.h"

#include "mmap_internal.h"

struct anon_page {
  uint64 index;
  uint64 pa;
  struct anon_page *next;
};

struct anon_object {
  struct spinlock lock;
  struct anon_page *pages;
};

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
  kaddquota(mem);
  release(&anon->lock);
  return mem;
}

static int
mmap_anon_fault(struct vma_area *vma, uint64 page, void **mem)
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

static const struct vma_ops mmap_anon_ops = {
  .fault = mmap_anon_fault,
};

int
mmap_anon_object_init(struct mmap_object *object, int flags)
{
  if(flags & MAP_SHARED){
    object->anon = anon_object_create();
    if(object->anon == 0)
      return -1;
  }
  return 0;
}

void
mmap_anon_object_destroy(struct mmap_object *object)
{
  if(object->anon)
    anon_object_destroy(object->anon);
}

uint64
vma_map_anon(struct proc *p, uint64 addr, uint64 length, int prot,
             int flags)
{
  struct mmap_object *object;
  uint64 start;

  if(flags != (MAP_PRIVATE | MAP_ANONYMOUS) &&
     flags != (MAP_SHARED | MAP_ANONYMOUS))
    return MAP_FAILED;

  object = mmap_object_create(VMA_ANON, 0, flags, 0);
  if(object == 0)
    return MAP_FAILED;
  start = mmap_map_create(p, addr, length, prot, flags, VMA_ANON,
                          object, &mmap_anon_ops, 0);
  if(start == MAP_FAILED)
    mmap_object_put(object);
  return start;
}
