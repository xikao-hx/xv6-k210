#include "dev.h"
#include "eagerdev.h"
#include "fcntl.h"
#include "file.h"
#include "mmap.h"
#include "printf.h"
#include "vm.h"

#define EAGERDEV_SIZE (2 * PGSIZE)

static int
eagerdev_open(struct file *file)
{
  if(file->minor != 0)
    return -1;
  return 0;
}

static int
eagerdev_mmap(struct file *file, struct vma_area *vma, uint64 offset)
{
  uint64 length = vma->valid_end - vma->start;

  (void)file;
  if(vma->flags != MAP_SHARED || (vma->prot & PROT_EXEC))
    return -1;
  if(offset + length < offset || offset + length > EAGERDEV_SIZE)
    return -1;
  vma->populate = 1;
  return 0;
}

static const struct file_operations eagerdev_ops = {
  .open = eagerdev_open,
  .mmap = eagerdev_mmap,
};

void
eagerdev_init(void)
{
  if(device_register(DEV_EAGER, "eager", &eagerdev_ops) < 0)
    panic("eager device register");
}
