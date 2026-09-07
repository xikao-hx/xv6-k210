#include "fcntl.h"
#include "file.h"
#include "kalloc.h"
#include "mmap.h"
#include "proc.h"
#include "string.h"
#include "vm.h"

#include "mmap_internal.h"

static int
mmap_file_fault(struct vma_area *vma, uint64 page, void **mem)
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

static const struct vma_ops mmap_file_ops = {
  .fault = mmap_file_fault,
};

int
mmap_file_object_init(struct mmap_object *object, struct file *file)
{
  object->file = filedup(file);
  return 0;
}

void
mmap_file_object_destroy(struct mmap_object *object)
{
  if(object->file)
    fileclose(object->file);
}

static int
mmap_file_writeback_page(struct vma_area *vma, pagetable_t pagetable,
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

int
mmap_file_writeback_range(struct proc *p, struct vma_area *vma,
                          uint64 start, uint64 end)
{
  if(vma->type != VMA_FILE)
    return 0;
  for(uint64 page = start; page < end; page += PGSIZE){
    if(mmap_file_writeback_page(vma, p->pagetable, page) < 0)
      return -1;
  }
  return 0;
}

uint64
vma_map_file(struct proc *p, uint64 addr, uint64 length, int prot,
             int flags, struct file *file, uint64 offset)
{
  struct mmap_object *object;
  uint64 start;

  if(flags != MAP_PRIVATE && flags != MAP_SHARED)
    return MAP_FAILED;
  if((offset % PGSIZE) != 0 || offset > 0xffffffffUL)
    return MAP_FAILED;
  if(file == 0 || file->type != FD_ENTRY || !file->readable)
    return MAP_FAILED;
  if(flags == MAP_SHARED && (prot & PROT_WRITE) && !file->writable)
    return MAP_FAILED;

  object = mmap_object_create(VMA_FILE, file, flags, 0);
  if(object == 0)
    return MAP_FAILED;
  start = mmap_map_create(p, addr, length, prot, flags, VMA_FILE,
                          object, &mmap_file_ops, offset);
  if(start == MAP_FAILED)
    mmap_object_put(object);
  return start;
}
