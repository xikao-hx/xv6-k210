#include "types.h"
#include "dev.h"
#include "sdcarddev.h"
#include "console.h"
#include "user.h"
#include "fcntl.h"

static int
check_open(const char *path, int major, int minor)
{
  // Create device node, then open it
  if(mknod(path, major, minor) < 0){
    printf("FAIL: mknod %s\n", path);
    return 1;
  }
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    printf("FAIL: open %s\n", path);
    return 1;
  }
  printf("open %s: fd=%d\n", path, fd);
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

  // Standard device nodes are created by init at boot;
  // we also test creating our own to verify the full path.
  fails += check_open("/dev/console", DEV_CONSOLE, 0);

  fd = open("/dev/stats", O_RDONLY);
  if (fd < 0) {
    printf("FAIL: open stats read-only\n");
    fails++;
  } else {
    close(fd);
  }
  if (open("/dev/stats", O_WRONLY) >= 0) {
    printf("FAIL: stats accepted write-only open\n");
    fails++;
  }

#ifndef QEMU
  fails += check_open("/dev/sdcard", DEV_SDCARD, 0);
  fails += check_open("/dev/w25q64", DEV_SPI, SPI_DEV_W25Q64);
  fails += check_open("/dev/oled", DEV_I2C, I2C_DEV_OLED);
  fails += check_open("/dev/mpu6050", DEV_I2C, I2C_DEV_MPU6050);
#endif

  // Invalid device identity: mknod with bogus identity should still work
  // (it just writes numbers into the directory entry), but open should fail.
  mknod("/dev/_bad_console", DEV_CONSOLE, 99);
  if (open("/dev/_bad_console", O_RDWR) >= 0) {
    printf("FAIL: invalid minor accepted\n");
    fails++;
  }
  mknod("/dev/_bad_major", 99, 0);
  if (open("/dev/_bad_major", O_RDWR) >= 0) {
    printf("FAIL: invalid major accepted\n");
    fails++;
  }

  fd = open("/dev/console", O_RDWR);
  if (fd < 0 || ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&baud) < 0) {
    printf("FAIL: console ioctl\n");
    fails++;
  } else {
    printf("console actual baud: %u\n", baud.actual);
  }
  if (fd >= 0)
    close(fd);

#ifndef QEMU
  fd = open("/dev/sdcard", O_RDWR);
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
