#include "types.h"
#include "dev.h"
#include "uartdev.h"
#include "user.h"
#include "fcntl.h"

int
main(void)
{
  struct uart_baud_info baud;
  struct uart_raw_stats raw;
  int fd;
  int fails = 0;

  printf("UART dev test\n");
  printf("=============\n");

  fd = dev(O_RDWR, DEV_UART, 0);
  if (fd < 0) {
    printf("FAIL: dev(DEV_UART)\n");
    exit(1);
  }

  if (ioctl(fd, UART_IOCTL_GET_BAUD_INFO, (uint64)&baud) < 0) {
    printf("FAIL: GET_BAUD_INFO\n");
    fails++;
  } else {
    printf("baud: requested=%u actual=%u div=%u clock=%u\n",
           baud.requested, baud.actual, baud.div, baud.clock);
  }

  if (ioctl(fd, UART_IOCTL_GET_RAW_STATS, (uint64)&raw) < 0) {
    printf("FAIL: GET_RAW_STATS\n");
    fails++;
  } else {
    printf("raw: mode=%u buffered=%u dropped=%u capacity=%u\n",
           raw.mode, raw.buffered, raw.dropped, raw.capacity);
  }

  if (write(fd, "uarttest: write path OK\n", 24) != 24) {
    printf("FAIL: write path\n");
    fails++;
  }

  close(fd);
  printf("UART test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
