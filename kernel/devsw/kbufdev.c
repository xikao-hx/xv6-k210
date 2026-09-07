#include "dev.h"
#include "fcntl.h"
#include "file.h"
#include "kbufdev.h"
#include "mmap.h"
#include "printf.h"
#include "vm.h"

#define KBUFDEV_SIZE (2 * PGSIZE)

static int
kbufdev_open(struct file *file)
{
  if(file->minor != 0)
    return -1;
  return 0;
}

static int
kbufdev_mmap(struct file *file, struct vma_area *vma, uint64 offset)
{
  uint64 length = vma->valid_end - vma->start;

  (void)file;
  if(vma->flags != MAP_SHARED || (vma->prot & PROT_EXEC))
    return -1;
  if(offset + length < offset || offset + length > KBUFDEV_SIZE)
    return -1;
  return 0;
}

static const struct file_operations kbufdev_ops = {
  .open = kbufdev_open,
  .mmap = kbufdev_mmap,
};

void
kbufdev_init(void)
{
  if(device_register(DEV_KBUF, "kbuf", &kbufdev_ops) < 0)
    panic("kbuf device register");
}
