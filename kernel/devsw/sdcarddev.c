//
// SD card block device — provides sector-based read/write via devsw.
//
// The device maintains a global sector position that starts at 0 and
// auto-increments after each 512-byte read/write.
//
// read:  read 512 bytes from the current sector into user buffer
// write: write 512 bytes from user buffer to the current sector
// ioctl: SDCARD_IOCTL_SEEK  — set sector position
//        SDCARD_IOCTL_TELL  — get sector position
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "file.h"
#include "defs.h"
#include "sdcard.h"
#include "sdcarddev.h"

static uint32 sdcard_sector;  // current sector position

static int
sdcarddev_read(int user_dst, uint64 dst, int n)
{
  // Only full-sector reads are supported.
  if (n != 512)
    return -1;

  char *buf = kalloc();
  if (buf == 0)
    return -1;

  sdcard_read_sector((uint8 *)buf, sdcard_sector);
  sdcard_sector++;

  int ret = 512;
  if (either_copyout(user_dst, dst, buf, 512) < 0)
    ret = -1;

  kfree(buf);
  return ret;
}

static int
sdcarddev_write(int user_src, uint64 src, int n)
{
  // Only full-sector writes are supported.
  if (n != 512)
    return -1;

  char *buf = kalloc();
  if (buf == 0)
    return -1;

  if (either_copyin(buf, user_src, src, 512) < 0) {
    kfree(buf);
    return -1;
  }

  sdcard_write_sector((uint8 *)buf, sdcard_sector);
  sdcard_sector++;

  kfree(buf);
  return 512;
}

static int
sdcarddev_ioctl(int minor, uint64 cmd, uint64 arg)
{
  struct proc *p = myproc();
  (void)minor;

  switch (cmd) {
  case SDCARD_IOCTL_SEEK:
    sdcard_sector = (uint32)arg;
    return 0;

  case SDCARD_IOCTL_TELL: {
    if (copyout(p->pagetable, arg, (char *)&sdcard_sector, sizeof(sdcard_sector)) < 0)
      return -1;
    return 0;
  }

  case SDCARD_IOCTL_NSECTORS: {
    uint32 ns = sdcard_nsectors();
    if (copyout(p->pagetable, arg, (char *)&ns, sizeof(ns)) < 0)
      return -1;
    return 0;
  }

  default:
    return -1;
  }
}

void
sdcarddev_init(void)
{
  sdcard_sector = 0;
  devsw[SDCARD_DEV].read  = sdcarddev_read;
  devsw[SDCARD_DEV].write = sdcarddev_write;
  devsw[SDCARD_DEV].ioctl = sdcarddev_ioctl;
}
