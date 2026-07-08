#include "types.h"
#include "spidev.h"
#include "user.h"

#define CMD_READ_DATA 0x03
#define W25Q64_MINOR SPI_MINOR(1, 0)
#define TEST_LEN 256

static uint32
checksum(uint8 *buf, int n)
{
  uint32 sum = 0;

  for (int i = 0; i < n; i++)
    sum = (sum << 5) - sum + buf[i];
  return sum;
}

int
main(void)
{
  uint32 clk_rate = 1000000;
  uint8 tx[4];
  uint8 rx[TEST_LEN];
  struct spi_ioc_transfer xfer[2];
  int fd;
  int fails = 0;

  printf("DMA-backed SPI test\n");
  printf("===================\n");

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

  memset(tx, 0, sizeof(tx));
  tx[0] = CMD_READ_DATA;
  memset(rx, 0, sizeof(rx));

  memset(xfer, 0, sizeof(xfer));
  xfer[0].tx_buf = (uint64)tx;
  xfer[0].len = 4;
  xfer[1].rx_buf = (uint64)rx;
  xfer[1].len = sizeof(rx);
  if (ioctl(fd, SPI_IOC_MESSAGE(2), (uint64)xfer) < 0) {
    printf("FAIL: SPI DMA-backed read\n");
    fails++;
  } else {
    printf("read %d bytes, checksum=0x%x\n", TEST_LEN, checksum(rx, TEST_LEN));
  }

  close(fd);
  printf("DMA-backed SPI test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
