#include "dev.h"
#include "fcntl.h"
#include "file.h"
#include "kbuf.h"
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
  file->private_data = kbuf_create(KBUFDEV_SIZE);
  return file->private_data ? 0 : -1;
}

static int
kbufdev_close(struct file *file)
{
  if(file->private_data)
    kbuf_put(file->private_data);
  return 0;
}

// Configure a freshly created device VMA, mirroring Linux f_op->mmap: the
// driver validates the request against the VMA and installs its vm_ops.
static int
kbufdev_mmap(struct file *file, struct vma_area *vma, uint64 offset)
{
  struct kbuf *kbuf = file->private_data;
  uint64 length = vma->end - vma->start;

  if(kbuf == 0 || vma->flags != MAP_SHARED || (vma->prot & PROT_EXEC))
    return -1;
  if((offset % PGSIZE) != 0 || length == 0 ||
     offset + length < offset || offset + length > kbuf_size(kbuf))
    return -1;
  vma->data = kbuf;
  return 0;
}

static const struct file_operations kbufdev_ops = {
  .open = kbufdev_open,
  .mmap = kbufdev_mmap,
  .close = kbufdev_close,
};

void
kbufdev_init(void)
{
  if(device_register(DEV_KBUF, "kbuf", &kbufdev_ops) < 0)
    panic("kbuf device register");
}