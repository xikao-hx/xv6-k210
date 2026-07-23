#include "types.h"
#include "dev.h"
#include "sdcarddev.h"
#include "console.h"
#include "user.h"
#include "fcntl.h"

static int
check_open(const char *name, int major, int minor)
{
  int fd = dev(O_RDWR, major, minor);

  if (fd < 0) {
    printf("FAIL: open %s\n", name);
    return 1;
  }
  printf("open %s: fd=%d\n", name, fd);
  close(fd);
  return 0;
}

int
main(void)
{
  struct console_baud_info baud;
#ifndef QEMU
  uint32 nsectors = 0;
#endif
  int fd;
  int fails = 0;

  printf("devsw interface test\n");
  printf("====================\n");

  fails += check_open("console", DEV_CONSOLE, 0);
  fd = dev(O_RDONLY, DEV_STATS, 0);
  if (fd < 0) {
    printf("FAIL: open stats read-only\n");
    fails++;
  } else {
    close(fd);
  }
  if (dev(O_WRONLY, DEV_STATS, 0) >= 0) {
    printf("FAIL: stats accepted write-only open\n");
    fails++;
  }
#ifndef QEMU
  fails += check_open("sdcard", DEV_SDCARD, 0);
  fails += check_open("w25q64", DEV_SPI, SPI_DEV_W25Q64);
  fails += check_open("oled", DEV_I2C, I2C_DEV_OLED);
  fails += check_open("mpu6050", DEV_I2C, I2C_DEV_MPU6050);
#endif

  if (dev(O_RDWR, DEV_CONSOLE, 99) >= 0 ||
      dev(O_RDWR, 99, 0) >= 0) {
    printf("FAIL: invalid device identity accepted\n");
    fails++;
  }

  fd = dev(O_RDWR, DEV_CONSOLE, 0);
  if (fd < 0 || ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&baud) < 0) {
    printf("FAIL: console ioctl\n");
    fails++;
  } else {
    printf("console actual baud: %u\n", baud.actual);
  }
  if (fd >= 0)
    close(fd);

#ifndef QEMU
  fd = dev(O_RDWR, DEV_SDCARD, 0);
  if (fd < 0 || ioctl(fd, SDCARD_IOCTL_NSECTORS, (uint64)&nsectors) < 0) {
    printf("FAIL: sdcard ioctl\n");
    fails++;
  } else {
    printf("sdcard sectors: %u\n", nsectors);
  }
  if (fd >= 0)
    close(fd);
#endif

  printf("devsw test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
