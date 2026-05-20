#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "file.h"
#include "dmac.h"
#include "spi.h"
#include "utils.h"
#include "spidev.h"
#include "defs.h"
#include "string.h"

// minor: (spi_bus << 2) | chip_select
#define SPI_BUS(minor)   (((minor) >> 2) & 3)
#define SPI_CS(minor)    ((minor) & 3)

static void
spi_reset_tmod(spi_device_num_t spi_num)
{
  uint8 tmod_off = (spi_num == 3) ? 10 : 8;
  set_bit(&spi[spi_num]->ctrlr0, 3 << tmod_off, SPI_TMOD_TRANS << tmod_off);
}

static int
spidev_read(int user_dst, uint64 dst, int n)
{
  // SPI half-duplex read is not supported via standard read()
  return -1;
}

static int
spidev_write(int user_src, uint64 src, int n)
{
  // SPI half-duplex write is not supported via standard write()
  return -1;
}

static int
spidev_ioctl(int minor, uint64 cmd, uint64 arg)
{
  struct proc *p = myproc();
  spi_device_num_t spi_bus = SPI_BUS(minor);
  spi_chip_select_t chip_sel = SPI_CS(minor);

  switch(cmd) {
  case SPI_IOCTL_INIT: {
    // arg is the clock rate (uint32 from user)
    uint32 clk_rate;
    if(copyin(p->pagetable, (char *)&clk_rate, arg, sizeof(clk_rate)) < 0)
      return -1;
    spi_init(spi_bus, SPI_WORK_MODE_0, SPI_FF_STANDARD, 8, 0);
    return 0;
  }

  case SPI_IOCTL_TRANSFER: {
    struct spidev_transfer xfer;
    if(copyin(p->pagetable, (char *)&xfer, arg, sizeof(xfer)) < 0)
      return -1;

    if(xfer.len == 0)
      return 0;

    // allocate page for tx data (if any)
    uint8 *tx_buf = 0;
    if(xfer.tx_buf != 0) {
      tx_buf = kalloc();
      if(tx_buf == 0)
        return -1;
      if(xfer.len > 4096)
        xfer.len = 4096;
      if(copyin(p->pagetable, (char *)tx_buf, xfer.tx_buf, xfer.len) < 0) {
        kfree(tx_buf);
        return -1;
      }
    }

    // allocate page for combined command+data (DMA needs contiguous)
    uint8 *dma_buf = kalloc();
    if(dma_buf == 0) {
      if(tx_buf) kfree(tx_buf);
      return -1;
    }

    // track actual rx length for copyout
    uint32 rx_copy_len = 0;

    if(xfer.tx_buf != 0) {
      if(xfer.rx_buf != 0) {
        // EEROM mode: send cmd_len bytes from tx_buf as command+address,
        // then receive (len - cmd_len) bytes into dma_buf (CS stays asserted).
        uint32 cmd_len = xfer.cmd_len ? xfer.cmd_len : xfer.len;
        uint32 rx_len = (xfer.len > cmd_len) ? (xfer.len - cmd_len) : 0;
        if(rx_len > 4096) rx_len = 4096;
        if(cmd_len > xfer.len) cmd_len = xfer.len;

        if(rx_len > 0) {
          spi_receive_data_standard(spi_bus, chip_sel, tx_buf, cmd_len, dma_buf, rx_len);
          rx_copy_len = rx_len;
        } else {
          // command-only transfer
          spi_reset_tmod(spi_bus);
          spi_send_data_standard(spi_bus, chip_sel, 0, 0, tx_buf, cmd_len);
        }
      } else {
        // send only
        spi_reset_tmod(spi_bus);
        spi_send_data_standard(spi_bus, chip_sel, 0, 0, tx_buf, xfer.len);
      }
    } else if(xfer.rx_buf != 0) {
      // receive only
      spi_receive_data_standard(spi_bus, chip_sel, 0, 0, dma_buf, xfer.len);
      rx_copy_len = xfer.len;
    }

    // copy received data back to user
    if(rx_copy_len > 0) {
      if(copyout(p->pagetable, xfer.rx_buf, (char *)dma_buf, rx_copy_len) < 0) {
        kfree(dma_buf);
        if(tx_buf) kfree(tx_buf);
        return -1;
      }
    }

    kfree(dma_buf);
    if(tx_buf) kfree(tx_buf);
    return 0;
  }

  default:
    return -1;
  }
}

void
spidev_init(void)
{
  devsw[SPI_DEV].read = spidev_read;
  devsw[SPI_DEV].write = spidev_write;
  devsw[SPI_DEV].ioctl = spidev_ioctl;
}

#ifdef TEST
/* ----------------------------------------------------------------- */
/*  Kernel-space W25Q64 test on SPI1                                 */
/* ----------------------------------------------------------------- */

#define W64_CMD_JEDEC_ID       0x9F
#define W64_CMD_READ_DATA      0x03
#define W64_CMD_WRITE_ENABLE   0x06
#define W64_CMD_WRITE_DISABLE  0x04
#define W64_CMD_PAGE_PROGRAM   0x02
#define W64_CMD_SECTOR_ERASE   0x20
#define W64_CMD_READ_STATUS1   0x05
#define W64_CMD_READ_STATUS2   0x35
#define W64_CMD_WRITE_STATUS   0x01   /* WRSR */

/* Status Register 1 bits */
#define SR1_BUSY  0x01
#define SR1_WEL   0x02
#define SR1_BP0   0x04
#define SR1_BP1   0x08
#define SR1_BP2   0x10
#define SR1_TB    0x20
#define SR1_SEC   0x40
#define SR1_SRP0  0x80

static uint8 w64_read_status1(void) {
  uint8 cmd = W64_CMD_READ_STATUS1;
  uint8 rx;
  spi_receive_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, &cmd, 1, &rx, 1);
  return rx;
}

static void w64_wait_busy(void) {
  int timeout = 1000000;
  while(timeout--) {
    if((w64_read_status1() & SR1_BUSY) == 0)
      return;
  }
  printf("W25Q64: wait busy timeout!\n");
}

static void w64_write_enable(void) {
  uint8 cmd = W64_CMD_WRITE_ENABLE;
  spi_reset_tmod(SPI_DEVICE_1);
  spi_send_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, 0, 0, &cmd, 1);
}

void
spidev_test_w25q64(void)
{
  uint8 rx[4];
  uint8 cmd[4];
  uint8 readback[256];
  uint8 pattern[256];
  uint8 wbuf[4 + 256];
  int i, match;
  uint8 sr1;

  printf("\n--- W25Q64 test on SPI1 (kernel-space) ---\n");

  /* Configure SPI1: mode 0, standard, 8-bit */
  spi_init(SPI_DEVICE_1, SPI_WORK_MODE_0, SPI_FF_STANDARD, 8, 0);
  /* Start with ~1 MHz */
  spi[SPI_DEVICE_1]->baudr = 200;

  /* ---- 1. Read JEDEC ID ---- */
  cmd[0] = W64_CMD_JEDEC_ID;
  spi_receive_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, cmd, 1, rx, 3);
  printf("  JEDEC ID: %x %x %x\n", rx[0], rx[1], rx[2]);

  if(rx[0] != 0xEF && rx[0] != 0x1C && rx[0] != 0xC8) {
    printf("  WARNING: unexpected manufacturer ID (is W25Q64 connected?)\n");
  }

  /* ---- Read and clear status register protection ---- */
  // sr1 = w64_read_status1();
  // printf("  Status Reg 1: 0x%x", sr1);
  // if(sr1 & SR1_BP0) printf(" BP0");
  // if(sr1 & SR1_BP1) printf(" BP1");
  // if(sr1 & SR1_BP2) printf(" BP2");
  // if(sr1 & SR1_SRP0) printf(" SRP0");
  // printf("\n");

  // if(sr1 & (SR1_BP0 | SR1_BP1 | SR1_BP2)) {
  //   printf("  Clearing block protection...\n");
  //   w64_write_enable();
  //   w64_wait_busy();
  //   cmd[0] = W64_CMD_WRITE_STATUS;
  //   cmd[1] = sr1 & ~(SR1_BP0 | SR1_BP1 | SR1_BP2); /* keep other bits, clear BP */
  //   cmd[2] = 0x00; /* status register 2 = 0 (clear QE/SRP) */
  //   spi_reset_tmod(SPI_DEVICE_1);
  //   spi_send_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, 0, 0, cmd, 3);
  //   w64_wait_busy();
  //   sr1 = w64_read_status1();
  //   printf("  Status Reg 1 after clear: 0x%x\n", sr1);
  // }

  /* Bump to ~10 MHz */
  spi[SPI_DEVICE_1]->baudr = 20;

  /* ---- 2. Erase sector 0 ---- */
  printf("  Erasing sector 0... ");
  cmd[0] = W64_CMD_SECTOR_ERASE;
  cmd[1] = 0; cmd[2] = 0; cmd[3] = 0;
  w64_write_enable();
  sr1 = w64_read_status1();
  if(!(sr1 & SR1_WEL))
    printf("\n  WARNING: WEL not set after WREN! sr1=0x%x", sr1);
  spi_reset_tmod(SPI_DEVICE_1);
  spi_send_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, 0, 0, cmd, 4);
  w64_wait_busy();
  printf("done\n");

  /* ---- 3. Verify erased ---- */
  cmd[0] = W64_CMD_READ_DATA;
  cmd[1] = 0; cmd[2] = 0; cmd[3] = 0;
  spi_receive_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, cmd, 4, readback, 256);
  match = 1;
  for(i = 0; i < 256; i++) {
    if(readback[i] != 0xFF) { match = 0; break; }
  }
  printf("  Erase verify: %s\n", match ? "PASS (all 0xFF)" : "FAIL");

  /* ---- 4. Write test pattern ---- */
  for(i = 0; i < 256; i++)
    pattern[i] = i;
  printf("  Writing page 0... ");
  wbuf[0] = W64_CMD_PAGE_PROGRAM;
  wbuf[1] = 0; wbuf[2] = 0; wbuf[3] = 0;
  for(i = 0; i < 256; i++) wbuf[4 + i] = pattern[i];
  w64_write_enable();
  sr1 = w64_read_status1();
  if(!(sr1 & SR1_WEL))
    printf("\n  WARNING: WEL not set after WREN! sr1=0x%x", sr1);
  spi_reset_tmod(SPI_DEVICE_1);
  spi_send_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, 0, 0, wbuf, 4 + 256);
  w64_wait_busy();
  printf("done\n");

  /* ---- 5. Read back and verify ---- */
  cmd[0] = W64_CMD_READ_DATA;
  cmd[1] = 0; cmd[2] = 0; cmd[3] = 0;
  spi_receive_data_standard(SPI_DEVICE_1, SPI_CHIP_SELECT_0, cmd, 4, readback, 256);
  match = 1;
  for(i = 0; i < 256; i++) {
    if(readback[i] != pattern[i]) { match = 0; break; }
  }
  printf("  Write verify: %s\n", match ? "PASS" : "FAIL");

  if(!match) {
    printf("  Written: ");
    for(i = 0; i < 16; i++) printf("%x ", pattern[i]);
    printf("\n  Read:    ");
    for(i = 0; i < 16; i++) printf("%x ", readback[i]);
    printf("\n");
  }

  printf("--- W25Q64 test %s ---\n\n", match ? "PASSED" : "FAILED");
}
#endif
