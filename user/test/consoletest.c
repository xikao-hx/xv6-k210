#include "types.h"
#include "dev.h"
#include "console.h"
#include "user.h"
#include "fcntl.h"

int
main(void)
{
  struct console_baud_info baud;
  struct console_rx_stats rx;
  int fd;
  int fails = 0;

  printf("Console device test\n");
  printf("===================\n");

  fd = open("/dev/console", O_RDWR);
  if (fd < 0) {
    printf("FAIL: dev(DEV_CONSOLE)\n");
    exit(1);
  }

  if (ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&baud) < 0) {
    printf("FAIL: GET_BAUD_INFO\n");
    fails++;
  } else {
    printf("baud: requested=%u actual=%u div=%u clock=%u\n",
           baud.requested, baud.actual, baud.div, baud.clock);
  }

  if (ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)&rx) < 0) {
    printf("FAIL: GET_RX_STATS\n");
    fails++;
  } else {
    printf("rx: mode=%u buffered=%u dropped=%u capacity=%u\n",
           rx.mode, rx.buffered, rx.dropped, rx.capacity);
  }

  if (ioctl(fd, CONSOLE_IOCTL_GET_MODE, 0) != CONSOLE_MODE_TTY) {
    printf("FAIL: initial mode\n");
    fails++;
  }
  if (ioctl(fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_RAW) < 0 ||
      ioctl(fd, CONSOLE_IOCTL_GET_MODE, 0) != CONSOLE_MODE_RAW) {
    printf("FAIL: RAW mode\n");
    fails++;
  }
  if (ioctl(fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_TTY) < 0) {
    printf("FAIL: restore TTY mode\n");
    fails++;
  }

  if (write(fd, "consoletest: write path OK\n", 27) != 27) {
    printf("FAIL: write path\n");
    fails++;
  }

  close(fd);
  printf("Console test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
