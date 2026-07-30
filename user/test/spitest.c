#include "types.h"
#include "fcntl.h"
#include "spidev.h"
#include "user.h"

#define CMD_JEDEC_ID 0x9F

static int
known_mid(uint8 mid)
{
  return mid == 0xEF || mid == 0x1C || mid == 0xC8;
}

int
main(void)
{
  uint8 cmd = CMD_JEDEC_ID;
  uint8 rx[3];
  struct spi_ioc_transfer xfer[2];
  int fd;
  int independent_fd;
  int fails = 0;
  uint32 speed;

  printf("SPI dev test\n");
  printf("============\n");

  fd = open("/dev/w25q64", O_RDWR);
  if (fd < 0) {
    printf("FAIL: dev(DEV_SPI)\n");
    exit(1);
  }

  if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, (uint64)&speed) < 0 ||
      speed != 1000000) {
    printf("FAIL: default SPI speed\n");
    fails++;
  }
  speed = 2000000;
  if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, (uint64)&speed) < 0) {
    printf("FAIL: set per-open SPI speed\n");
    fails++;
  }
  independent_fd = open("/dev/w25q64", O_RDWR);
  if (independent_fd < 0 ||
      ioctl(independent_fd, SPI_IOC_RD_MAX_SPEED_HZ, (uint64)&speed) < 0 ||
      speed != 1000000) {
    printf("FAIL: independent SPI open state\n");
    fails++;
  }
  if (independent_fd >= 0)
    close(independent_fd);
  speed = 20000000;
  if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, (uint64)&speed) >= 0) {
    printf("FAIL: SPI speed exceeded board limit\n");
    fails++;
  }

  memset(xfer, 0, sizeof(xfer));
  xfer[0].tx_buf = (uint64)&cmd;
  xfer[0].len = 1;
  xfer[1].rx_buf = (uint64)rx;
  xfer[1].len = 3;
  if (ioctl(fd, SPI_IOC_MESSAGE(2), (uint64)xfer) < 0) {
    printf("FAIL: SPI_IOC_MESSAGE\n");
    fails++;
  } else {
    printf("JEDEC: %x %x %x\n", rx[0], rx[1], rx[2]);
    if (!known_mid(rx[0])) {
      printf("FAIL: unknown manufacturer ID\n");
      fails++;
    }
  }

  close(fd);
  printf("SPI test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
