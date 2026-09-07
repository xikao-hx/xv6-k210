#include "fcntl.h"
#include "file.h"
#include "kalloc.h"
#include "mmap.h"
#include "printf.h"
#include "proc.h"
#include "spinlock.h"
#include "string.h"
#include "vm.h"

#include "mmap_internal.h"

struct kbuf_page {
  void *address;
  struct kbuf_page *next;
};

struct kbuf {
  struct spinlock lock;
  int refcnt;
  uint64 npages;
  struct kbuf_page *pages;
};

static void
kbuf_destroy(struct kbuf *kbuf)
{
  struct kbuf_page *page = kbuf->pages;

  while(page){
    struct kbuf_page *next = page->next;
    kfree_page(page->address);
    kfree(page);
    page = next;
  }
  kfree(kbuf);
}

static struct kbuf *
kbuf_create(uint64 size)
{
  struct kbuf *kbuf;
  struct kbuf_page **next;
  uint64 rounded;

  if(size == 0)
    return 0;
  rounded = PGROUNDUP(size);
  if(rounded < size)
    return 0;
  if((kbuf = kmalloc(sizeof(*kbuf))) == 0)
    return 0;
  memset(kbuf, 0, sizeof(*kbuf));
  initlock(&kbuf->lock, "kbuf");
  kbuf->refcnt = 1;
  kbuf->npages = rounded / PGSIZE;

  next = &kbuf->pages;
  for(uint64 i = 0; i < kbuf->npages; i++){
    struct kbuf_page *page = kmalloc(sizeof(*page));

    if(page == 0 || (page->address = kalloc_page()) == 0){
      if(page)
        kfree(page);
      kbuf_destroy(kbuf);
      return 0;
    }
    memset(page->address, 0, PGSIZE);
    page->next = 0;
    *next = page;
    next = &page->next;
  }
  return kbuf;
}

static void
kbuf_put(struct kbuf *kbuf)
{
  int destroy;

  acquire(&kbuf->lock);
  if(kbuf->refcnt < 1)
    panic("kbuf_put");
  kbuf->refcnt--;
  destroy = kbuf->refcnt == 0;
  release(&kbuf->lock);
  if(destroy)
    kbuf_destroy(kbuf);
}

static void *
kbuf_page_address(struct kbuf *kbuf, uint64 index)
{
  struct kbuf_page *page;

  if(index >= kbuf->npages)
    return 0;
  page = kbuf->pages;
  while(index-- > 0)
    page = page->next;
  return page->address;
}

static void *
kbuf_page_get(struct kbuf *kbuf, uint64 index)
{
  void *address = kbuf_page_address(kbuf, index);

  if(address)
    kaddquota(address);
  return address;
}

static int
mmap_kbuf_fault(struct vma_area *vma, uint64 page, void **mem)
{
  struct kbuf *kbuf;
  uint64 index;

  if(vma->object == 0 || vma->object->type != VMA_KBUF)
    return -1;
  kbuf = vma->object->kbuf;
  if(kbuf == 0)
    return -1;
  index = (vma->offset + page - vma->start) / PGSIZE;
  *mem = kbuf_page_get(kbuf, index);
  return *mem ? 0 : -1;
}

static const struct vma_ops mmap_kbuf_ops = {
  .fault = mmap_kbuf_fault,
};

int
mmap_kbuf_object_init(struct mmap_object *object, struct file *file,
                      uint64 size)
{
  object->kbuf = kbuf_create(size);
  if(object->kbuf == 0)
    return -1;
  object->file = filedup(file);
  return 0;
}

void
mmap_kbuf_object_destroy(struct mmap_object *object)
{
  if(object->file)
    fileclose(object->file);
  if(object->kbuf)
    kbuf_put(object->kbuf);
}

uint64
vma_map_device(struct proc *p, uint64 addr, uint64 length, int prot,
               int flags, struct file *file, uint64 offset)
{
  struct mmap_object *object;
  struct vma_area *vma;
  uint64 backing_size;
  uint64 valid_length;
  uint64 start;

  if(flags != MAP_SHARED || file == 0 || file->type != FD_DEVICE ||
     file->ops == 0 || file->ops->mmap == 0 || (prot & PROT_EXEC))
    return MAP_FAILED;
  if((prot & PROT_READ) && !file->readable)
    return MAP_FAILED;
  if((prot & PROT_WRITE) && !file->writable)
    return MAP_FAILED;

  start = mmap_map_create(p, addr, length, prot, flags, VMA_KBUF, 0,
                          &mmap_kbuf_ops, offset);
  if(start == MAP_FAILED)
    return MAP_FAILED;
  vma = mmap_vma_find(p, start);
  valid_length = vma ? vma->valid_end - vma->start : 0;
  if(vma == 0 || vma->ops == 0 || vma->ops->fault == 0 ||
     (offset % PGSIZE) != 0 || valid_length == 0 ||
     offset + valid_length < offset ||
     file->ops->mmap(file, vma, offset) < 0){
    vma_unmap(p, start, length);
    return MAP_FAILED;
  }

  backing_size = offset + valid_length;
  object = mmap_object_create(VMA_KBUF, file, flags, backing_size);
  if(object == 0){
    vma_unmap(p, start, length);
    return MAP_FAILED;
  }
  vma->object = object;
  if(vma->populate && mmap_populate(p, vma) < 0){
    vma_unmap(p, start, length);
    return MAP_FAILED;
  }
  return start;
}

void *
vma_device_page_address(struct proc *p, struct file *file, uint64 va,
                        uint64 index)
{
  struct vma_area *vma = mmap_vma_find(p, va);
  uint64 object_index;

  if(vma == 0 || vma->start != va || vma->type != VMA_KBUF ||
     vma->object == 0 || vma->object->file != file ||
     vma->object->kbuf == 0)
    return 0;
  if(index >= (vma->end - vma->start) / PGSIZE)
    return 0;
  object_index = vma->offset / PGSIZE + index;
  return kbuf_page_address(vma->object->kbuf, object_index);
}
