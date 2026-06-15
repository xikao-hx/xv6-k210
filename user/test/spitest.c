#include "types.h"
#include "spidev.h"
#include "user.h"

#define CMD_JEDEC_ID 0x9F
#define W25Q64_MINOR SPI_MINOR(1, 0)

static int
known_mid(uint8 mid)
{
  return mid == 0xEF || mid == 0x1C || mid == 0xC8;
}

int
main(void)
{
  uint32 clk_rate = 1000000;
  uint8 cmd = CMD_JEDEC_ID;
  uint8 rx[3];
  struct spidev_transfer xfer;
  int fd;
  int fails = 0;

  printf("SPI dev test\n");
  printf("============\n");

  fd = dev(0, DEV_SPI, W25Q64_MINOR);
  if (fd < 0) {
    printf("FAIL: dev(DEV_SPI)\n");
    exit(1);
  }

  if (ioctl(fd, SPI_IOCTL_INIT, (uint64)&clk_rate) < 0) {
    printf("FAIL: SPI_IOCTL_INIT\n");
    close(fd);
    exit(1);
  }

  xfer.tx_buf = (uint64)&cmd;
  xfer.rx_buf = (uint64)rx;
  xfer.len = 4;
  xfer.cmd_len = 1;
  if (ioctl(fd, SPI_IOCTL_TRANSFER, (uint64)&xfer) < 0) {
    printf("FAIL: SPI_IOCTL_TRANSFER\n");
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
