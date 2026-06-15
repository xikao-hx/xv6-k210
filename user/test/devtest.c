#include "types.h"
#include "file.h"
#include "sdcarddev.h"
#include "uartdev.h"
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
  struct uart_baud_info baud;
  uint32 nsectors = 0;
  int fd;
  int fails = 0;

  printf("devsw interface test\n");
  printf("====================\n");

  fails += check_open("console", CONSOLE, 0);
  fails += check_open("stats", STATS, 0);
  fails += check_open("uart", UART_DEV, 0);
  fails += check_open("sdcard", SDCARD_DEV, 0);
  fails += check_open("spi1.cs0", SPI_DEV, 4);
  fails += check_open("i2c0", I2C_DEV, 0);

  fd = dev(O_RDWR, UART_DEV, 0);
  if (fd < 0 || ioctl(fd, UART_IOCTL_GET_BAUD_INFO, (uint64)&baud) < 0) {
    printf("FAIL: uart ioctl\n");
    fails++;
  } else {
    printf("uart actual baud: %u\n", baud.actual);
  }
  if (fd >= 0)
    close(fd);

  fd = dev(O_RDWR, SDCARD_DEV, 0);
  if (fd < 0 || ioctl(fd, SDCARD_IOCTL_NSECTORS, (uint64)&nsectors) < 0) {
    printf("FAIL: sdcard ioctl\n");
    fails++;
  } else {
    printf("sdcard sectors: %u\n", nsectors);
  }
  if (fd >= 0)
    close(fd);

  printf("devsw test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
