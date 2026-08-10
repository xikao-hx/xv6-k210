#include "dev.h"
#include "eagerdev.h"
#include "fcntl.h"
#include "file.h"
#include "kbuf.h"
#include "mmap.h"
#include "printf.h"
#include "vm.h"
#include "proc.h"

#define EAGERDEV_SIZE (2 * PGSIZE)

static int
eagerdev_open(struct file *file)
{
  if(file->minor != 0)
    return -1;
  // Pages are allocated up front by the driver (not at fault time).
  file->private_data = kbuf_create(EAGERDEV_SIZE);
  return file->private_data ? 0 : -1;
}

static int
eagerdev_close(struct file *file)
{
  if(file->private_data)
    kbuf_put(file->private_data);
  return 0;
}

static int
eagerdev_mmap(struct file *file, struct vma_area *vma, uint64 offset)
{
  struct kbuf *kbuf = file->private_data;
  uint64 length = vma->end - vma->start;
  struct proc *p = myproc();

  if(kbuf == 0 || vma->flags != MAP_SHARED || (vma->prot & PROT_EXEC))
    return -1;
  if((offset % PGSIZE) != 0 || length == 0 ||
     offset + length < offset || offset + length > kbuf_size(kbuf))
    return -1;

  vma->data = kbuf;
  if(vma_populate(p, vma) < 0){
    printf("mmap vma_populate failed\n");
    return -1;
  }

  return 0;
}

static const struct file_operations eagerdev_ops = {
  .open = eagerdev_open,
  .mmap = eagerdev_mmap,
  .close = eagerdev_close,
};

void
eagerdev_init(void)
{
  if(device_register(DEV_EAGER, "eager", &eagerdev_ops) < 0)
    panic("eager device register");
}
