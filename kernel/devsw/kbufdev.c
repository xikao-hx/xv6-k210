#include "dev.h"
#include "fcntl.h"
#include "file.h"
#include "kbuf.h"
#include "kbufdev.h"
#include "printf.h"
#include "proc.h"
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

static struct kbuf *
kbufdev_mmap(struct file *file, uint64 offset, uint64 length,
             int prot, int flags)
{
  struct kbuf *kbuf = file->private_data;

  if(kbuf == 0 || flags != MAP_SHARED || (prot & PROT_EXEC))
    return 0;
  if((offset % PGSIZE) != 0 || length == 0 ||
     offset + length < offset || offset + length > kbuf_size(kbuf))
    return 0;
  return kbuf;
}

static int
kbufdev_region(struct kbuf *kbuf, struct kbuf_ioctl_region *region,
               int check)
{
  uint64 end;
  uint64 offset;

  if(region->length == 0)
    return 0;
  end = (uint64)region->offset + region->length;
  if(end < region->offset || end > kbuf_size(kbuf))
    return -1;

  offset = region->offset;
  while(offset < end){
    uchar *page = kbuf_page_address(kbuf, offset / PGSIZE);
    uint64 in_page = offset % PGSIZE;
    uint64 count = PGSIZE - in_page;

    if(count > end - offset)
      count = end - offset;
    for(uint64 i = 0; i < count; i++){
      if(check){
        if(page[in_page + i] != region->value)
          return -1;
      } else {
        page[in_page + i] = region->value;
      }
    }
    offset += count;
  }
  return 0;
}

static int
kbufdev_ioctl(struct file *file, uint64 cmd, uint64 arg)
{
  struct kbuf_ioctl_region region;
  struct kbuf *kbuf = file->private_data;

  if(kbuf == 0 ||
     copyin(myproc()->pagetable, (char *)&region, arg, sizeof(region)) < 0)
    return -1;
  if(cmd == KBUF_IOCTL_FILL)
    return kbufdev_region(kbuf, &region, 0);
  if(cmd == KBUF_IOCTL_CHECK)
    return kbufdev_region(kbuf, &region, 1);
  return -1;
}

static const struct file_operations kbufdev_ops = {
  .open = kbufdev_open,
  .ioctl = kbufdev_ioctl,
  .mmap = kbufdev_mmap,
  .close = kbufdev_close,
};

void
kbufdev_init(void)
{
  if(device_register(DEV_KBUF, "kbuf", &kbufdev_ops) < 0)
    panic("kbuf device register");
}
