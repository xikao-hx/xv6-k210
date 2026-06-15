#include "types.h"
#include "spidev.h"
#include "user.h"

/* W25Q64 commands */
#define CMD_JEDEC_ID      0x9F
#define CMD_READ_DATA     0x03
#define CMD_WRITE_ENABLE  0x06
#define CMD_PAGE_PROGRAM  0x02
#define CMD_SECTOR_ERASE  0x20
#define CMD_READ_STATUS1  0x05

#define W25Q64_PAGE_SIZE   256
#define W25Q64_SECTOR_SIZE 4096

/* SPI1, CS0 → minor = (1 << 2) | 0 = 4 */
#define W25Q64_MINOR 4

static int spi_fd;

static int w64_known_manufacturer(uint8 mid) {
  return mid == 0xEF || mid == 0x1C || mid == 0xC8;
}

static int w64_init(void) {
  uint32 clk_rate = 1000000;  /* 1 MHz */
  spi_fd = dev(0, SPI_DEV_MAJOR, W25Q64_MINOR);  // dev(omode, major, minor)
  if(spi_fd < 0) {
    printf("w25q64: dev() failed\n");
    return -1;
  }
  if(ioctl(spi_fd, SPI_IOCTL_INIT, (uint64)&clk_rate) < 0) {
    printf("w25q64: ioctl init failed\n");
    return -1;
  }
  return 0;
}

static int w64_read_id(uint8 *mid, uint16 *did) {
  uint8 cmd = CMD_JEDEC_ID;
  uint8 rx[3];
  struct spidev_transfer xfer;
  xfer.tx_buf = (uint64)&cmd;
  xfer.rx_buf = (uint64)rx;
  xfer.len = 1 + 3;      /* cmd + 3 bytes response */
  xfer.cmd_len = 1;      /* 1 command byte */
  if(ioctl(spi_fd, SPI_IOCTL_TRANSFER, (uint64)&xfer) < 0)
    return -1;
  *mid = rx[0];
  *did = ((uint16)rx[1] << 8) | rx[2];
  return 0;
}

static int w64_read_status(void) {
  uint8 cmd = CMD_READ_STATUS1;
  uint8 rx = 0;
  struct spidev_transfer xfer;
  xfer.tx_buf = (uint64)&cmd;
  xfer.rx_buf = (uint64)&rx;
  xfer.len = 1 + 1;
  xfer.cmd_len = 1;
  if(ioctl(spi_fd, SPI_IOCTL_TRANSFER, (uint64)&xfer) < 0)
    return -1;
  return rx;
}

static void w64_wait_busy(void) {
  int timeout = 1000000;
  while(timeout--) {
    if((w64_read_status() & 0x01) == 0)
      return;
  }
  printf("w25q64: wait busy timeout!\n");
}

static void w64_write_enable(void) {
  uint8 cmd = CMD_WRITE_ENABLE;
  struct spidev_transfer xfer;
  xfer.tx_buf = (uint64)&cmd;
  xfer.rx_buf = 0;
  xfer.len = 1;
  xfer.cmd_len = 0;
  if(ioctl(spi_fd, SPI_IOCTL_TRANSFER, (uint64)&xfer) < 0)
    printf("w25q64: write enable failed\n");
}

static int w64_read_data(uint32 addr, uint8 *buf, uint32 len) {
  uint8 cmd[4];
  cmd[0] = CMD_READ_DATA;
  cmd[1] = (addr >> 16) & 0xFF;
  cmd[2] = (addr >> 8) & 0xFF;
  cmd[3] = addr & 0xFF;
  struct spidev_transfer xfer;
  xfer.tx_buf = (uint64)cmd;
  xfer.rx_buf = (uint64)buf;
  xfer.len = 4 + len;
  xfer.cmd_len = 4;
  return ioctl(spi_fd, SPI_IOCTL_TRANSFER, (uint64)&xfer);
}

static int w64_sector_erase(uint32 addr) {
  w64_write_enable();
  uint8 cmd[4];
  cmd[0] = CMD_SECTOR_ERASE;
  cmd[1] = (addr >> 16) & 0xFF;
  cmd[2] = (addr >> 8) & 0xFF;
  cmd[3] = addr & 0xFF;
  struct spidev_transfer xfer;
  xfer.tx_buf = (uint64)cmd;
  xfer.rx_buf = 0;
  xfer.len = 4;
  xfer.cmd_len = 0;
  int ret = ioctl(spi_fd, SPI_IOCTL_TRANSFER, (uint64)&xfer);
  w64_wait_busy();
  return ret;
}

static int w64_page_program(uint32 addr, uint8 *data, uint32 len) {
  if(len > W25Q64_PAGE_SIZE)
    len = W25Q64_PAGE_SIZE;
  w64_write_enable();
  w64_wait_busy();
  /* pack cmd + addr + data into one tx buffer */
  uint8 buf[4 + W25Q64_PAGE_SIZE];
  buf[0] = CMD_PAGE_PROGRAM;
  buf[1] = (addr >> 16) & 0xFF;
  buf[2] = (addr >> 8) & 0xFF;
  buf[3] = addr & 0xFF;
  for(uint32 i = 0; i < len; i++)
    buf[4 + i] = data[i];
  struct spidev_transfer xfer;
  xfer.tx_buf = (uint64)buf;
  xfer.rx_buf = 0;
  xfer.len = 4 + len;
  xfer.cmd_len = 0;
  int ret = ioctl(spi_fd, SPI_IOCTL_TRANSFER, (uint64)&xfer);
  w64_wait_busy();
  return ret;
}

int main(void) {
  printf("W25Q64 Test on SPI1\n");
  printf("===================\n");

  if(w64_init() < 0) {
    printf("FAIL: w64_init\n");
    exit(1);
  }

  /* 1. Read JEDEC ID */
  uint8 mid;
  uint16 did;
  if(w64_read_id(&mid, &did) < 0) {
    printf("FAIL: w64_read_id\n");
    exit(1);
  }
  printf("JEDEC ID:  MID=0x%x, DID=0x%x\n", mid, did);
  if(!w64_known_manufacturer(mid)) {
    printf("WARNING: unexpected manufacturer ID (expected 0xEF/Winbond, 0x1C/eON, 0xC8/GigaDevice)\n");
    printf("Check wiring for SPI1: IO15=MOSI, IO16=MISO, IO17=SCLK, IO18=CS0, plus 3V3 and GND.\n");
    printf("Abort before erase/program because the flash is not responding.\n");
    exit(1);
  }

  /* 2. Erase sector 0 */
  printf("Erasing sector 0...\n");
  if(w64_sector_erase(0) < 0) {
    printf("FAIL: w64_sector_erase\n");
    exit(1);
  }
  printf("OK\n");

  /* 3. Verify sector is erased (all 0xFF) */
  uint8 readback[256];
  if(w64_read_data(0, readback, 256) < 0) {
    printf("FAIL: w64_read_data (verify erase)\n");
    exit(1);
  }
  int erased = 1;
  for(int i = 0; i < 256; i++) {
    if(readback[i] != 0xFF) { erased = 0; break; }
  }
  printf("Sector 0 erased: %s\n", erased ? "YES (all 0xFF)" : "NO");

  /* 4. Write test pattern */
  uint8 pattern[256];
  for(int i = 0; i < 256; i++)
    pattern[i] = i;
  printf("Writing test pattern to page 0...\n");
  if(w64_page_program(0, pattern, 256) < 0) {
    printf("FAIL: w64_page_program\n");
    exit(1);
  }
  printf("OK\n");

  /* 5. Read back and verify */
  if(w64_read_data(0, readback, 256) < 0) {
    printf("FAIL: w64_read_data (verify write)\n");
    exit(1);
  }
  int match = 1;
  for(int i = 0; i < 256; i++) {
    if(readback[i] != pattern[i]) { match = 0; break; }
  }
  printf("Write verify: %s\n", match ? "PASS" : "FAIL");

  if(!match) {
    printf("First 16 bytes written:  ");
    for(int i = 0; i < 16; i++) printf("%x ", pattern[i]);
    printf("\n");
    printf("First 16 bytes readback: ");
    for(int i = 0; i < 16; i++) printf("%x ", readback[i]);
    printf("\n");
  }

  printf("\nW25Q64 test %s!\n", match ? "PASSED" : "FAILED");
  exit(0);
}
