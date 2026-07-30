//
// SD card block device — provides sector-based read/write via devsw.
//
// Each open instance maintains a sector position that starts at 0 and
// is shared by dup/fork through struct file.
//
// read:  read 512 bytes from the current sector into user buffer
// write: write 512 bytes from user buffer to the current sector
// ioctl: SDCARD_IOCTL_SEEK  — set sector position
//        SDCARD_IOCTL_TELL  — get sector position
//        SDCARD_IOCTL_INVALIDATE_CACHE — drop FS caches after raw writes
//

#include "buf.h"
#include "fat32.h"
#include "file.h"
#include "kalloc.h"
#include "printf.h"
#include "proc.h"
#include "sdcard.h"
#include "sdcarddev.h"
#include "sleeplock.h"
#include "vm.h"
#include "dev.h"

struct sdcard_file_context {
  struct sleeplock lock;
  uint32 sector;
};

static int
sdcarddev_open(struct file *f)
{
  struct sdcard_file_context *ctx;

  if(f->minor != 0)
    return -1;
  ctx = kmalloc(sizeof(*ctx));
  if(ctx == 0)
    return -1;
  initsleeplock(&ctx->lock, "sdcard file");
  ctx->sector = 0;
  f->private_data = ctx;
  return 0;
}

static int
sdcarddev_read(struct file *f, uint64 dst, int n)
{
  struct sdcard_file_context *ctx = f->private_data;

  // Only full-sector reads are supported.
  if (ctx == 0 || n != 512)
    return -1;

  char *buf = kalloc_page();
  if (buf == 0)
    return -1;

  int ret = -1;
  acquiresleep(&ctx->lock);
  sdcard_read_sector((uint8 *)buf, ctx->sector);
  if (either_copyout(1, dst, buf, 512) >= 0) {
    ctx->sector++;
    ret = 512;
  }
  releasesleep(&ctx->lock);

  kfree_page(buf);
  return ret;
}

static int
sdcarddev_write(struct file *f, uint64 src, int n)
{
  struct sdcard_file_context *ctx = f->private_data;

  // Only full-sector writes are supported.
  if (ctx == 0 || n != 512)
    return -1;

  char *buf = kalloc_page();
  if (buf == 0)
    return -1;

  if (either_copyin(buf, 1, src, 512) < 0) {
    kfree_page(buf);
    return -1;
  }

  acquiresleep(&ctx->lock);
  sdcard_write_sector((uint8 *)buf, ctx->sector);
  ctx->sector++;
  releasesleep(&ctx->lock);

  kfree_page(buf);
  return 512;
}

static int
sdcarddev_ioctl(struct file *f, uint64 cmd, uint64 arg)
{
  struct proc *p = myproc();
  struct sdcard_file_context *ctx = f->private_data;

  if(ctx == 0)
    return -1;

  switch (cmd) {
  case SDCARD_IOCTL_SEEK:
    acquiresleep(&ctx->lock);
    ctx->sector = (uint32)arg;
    releasesleep(&ctx->lock);
    return 0;

  case SDCARD_IOCTL_TELL: {
    uint32 sector;
    acquiresleep(&ctx->lock);
    sector = ctx->sector;
    releasesleep(&ctx->lock);
    if (copyout(p->pagetable, arg, (char *)&sector, sizeof(sector)) < 0)
      return -1;
    return 0;
  }

  case SDCARD_IOCTL_NSECTORS: {
    uint32 ns = sdcard_nsectors();
    if (copyout(p->pagetable, arg, (char *)&ns, sizeof(ns)) < 0)
      return -1;
    return 0;
  }

  case SDCARD_IOCTL_INVALIDATE_CACHE:
    fat32_invalidate();
    binvalidate(0);
    return 0;

  default:
    return -1;
  }
}

static int
sdcarddev_close(struct file *f)
{
  if(f->private_data)
    kfree(f->private_data);
  return 0;
}

static const struct file_operations sdcard_ops = {
  .open = sdcarddev_open,
  .read = sdcarddev_read,
  .write = sdcarddev_write,
  .ioctl = sdcarddev_ioctl,
  .close = sdcarddev_close,
};

void
sdcarddev_init(void)
{
  if(device_register(DEV_SDCARD, "sdcard", &sdcard_ops) < 0)
    panic("sdcard device register");
}
