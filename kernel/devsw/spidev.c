#include "file.h"
#include "kalloc.h"
#include "printf.h"
#include "proc.h"
#include "utils.h"
#include "vm.h"
#include "string.h"
#include "spi.h"
#include "spidev.h"
#include "spi_board.h"
#include "gpiohs.h"

/* Bounce buffer size: one page per direction, fixed for the lifetime of an open. */
#define SPIDEV_BUFSIZ PGSIZE

static int
spidev_open(struct file *f)
{
  struct spidev_data *spidev;

  spidev = kmalloc(sizeof(struct spidev_data));
  if(spidev == 0)
    return -1;

  initsleeplock(&spidev->lock, "spi file");

  spidev->minor = f->minor;
  spidev->dev = spi_device_get(f->minor);
  spidev->speed_hz = 1000000;

  /* Pre-allocate fixed bounce buffers for this open instance.  Every
   * SPI_IOC_MESSAGE transfer bounces tx/rx data through these pages,
   * so no allocation is needed in the ioctl path.
   */
  spidev->tx_buffer = kalloc_page();
  spidev->rx_buffer = kalloc_page();
  if(spidev->tx_buffer == 0 || spidev->rx_buffer == 0) {
    if(spidev->tx_buffer)
      kfree_page(spidev->tx_buffer);
    if(spidev->rx_buffer)
      kfree_page(spidev->rx_buffer);
    kfree(spidev);
    return -1;
  }

  f->private_data = spidev;

  return 0;
}

static int
spidev_close(struct file *f)
{
  struct spidev_data *spidev = f->private_data;

  if(spidev) {
    if(spidev->tx_buffer)
      kfree_page(spidev->tx_buffer);
    if(spidev->rx_buffer)
      kfree_page(spidev->rx_buffer);
    kfree(spidev);
    f->private_data = 0;
  }

  return 0;
}

static int
spidev_read(struct file *f, uint64 dst, int n)
{
  (void)f;
  (void)dst;
  (void)n;
  // SPI half-duplex read is not supported via standard read()
  return -1;
}

static int
spidev_write(struct file *f, uint64 src, int n)
{
  (void)f;
  (void)src;
  (void)n;
  // SPI half-duplex write is not supported via standard write()
  return -1;
}

static struct spi_ioc_transfer *
spidev_get_ioc_message(uint64 cmd, struct spi_ioc_transfer *u_ioc, uint32 *n_ioc)
{
  struct proc *p = myproc();

  struct spi_ioc_transfer *ioc;
  uint tmp;

  if(_IOC_TYPE(cmd) != SPI_IOC_MAGIC ||
     _IOC_NR(cmd) != _IOC_NR(SPI_IOC_MESSAGE(0)) ||
     _IOC_DIR(cmd) != _IOC_WRITE)
    return 0;

  tmp = _IOC_SIZE(cmd);
  if(tmp > PGSIZE || (tmp % sizeof(struct spi_ioc_transfer)) != 0)
    return 0;

  *n_ioc = tmp / sizeof(struct spi_ioc_transfer);
  if(*n_ioc == 0)
    return 0;

  ioc = kmalloc(tmp);
  if (ioc == 0)
    return 0;

  if (copyin(p->pagetable, (char *)ioc, (uint64)u_ioc, tmp) < 0) {
    kfree(ioc);
    return 0;
  }
    
  return ioc;
}

static int
spidev_message(struct spidev_data *spidev,
               struct spi_ioc_transfer *u_xfers, uint32 n_xfers)
{
  struct proc *p = myproc();
  struct spi_transfer *k_xfers;
  struct spi_transfer *k_tmp;
  struct spi_ioc_transfer *u_tmp;
  uint8 *tx_buf, *rx_buf;
  uint32 n, total, rx_total;
  int status = -1;

  if(spidev->tx_buffer == 0 || spidev->rx_buffer == 0)
    return -1;

  /* One kernel transfer per message segment, bounced through the fixed
   * per-open buffers.  The driver keeps CS asserted across the whole
   * message, so this preserves the single-CS-assertion semantics.
   */
  k_xfers = kalloc_page();
  if(k_xfers == 0)
    return -1;
  memset(k_xfers, 0, PGSIZE);

  tx_buf = spidev->tx_buffer;
  rx_buf = spidev->rx_buffer;
  total = 0;
  rx_total = 0;

  /* 0xFF-fill the TX bounce buffer: rx-only segments send this dummy. */
  memset(spidev->tx_buffer, 0xff, SPIDEV_BUFSIZ);

  for (n = n_xfers, k_tmp = k_xfers, u_tmp = u_xfers; n; n--, k_tmp++, u_tmp++) {
    k_tmp->len = u_tmp->len;
    k_tmp->tx_buf = tx_buf;

    total += k_tmp->len;
    if (total > SPIDEV_BUFSIZ) {
      status = -1;
      goto done;
    }
    tx_buf += k_tmp->len;

    if (u_tmp->tx_buf) {
      if (copyin(p->pagetable, (char *)k_tmp->tx_buf, u_tmp->tx_buf, u_tmp->len) < 0) {
        status = -1;
        goto done;
      }
    }

    if (u_tmp->rx_buf) {
      /* this segment needs room in the RX bounce buffer */
      rx_total += k_tmp->len;
      if (rx_total > SPIDEV_BUFSIZ) {
        status = -1;
        goto done;
      }
      k_tmp->rx_buf = rx_buf;
      rx_buf += k_tmp->len;
    }
  }

  if(total == 0)
    goto done;

  if(spi_transfer(spidev->dev, k_xfers, n_xfers) < 0)
    goto done;

  /* copy any rx data out of the bounce buffer */
  rx_buf = spidev->rx_buffer;
  for (n = n_xfers, u_tmp = u_xfers; n; n--, u_tmp++) {
    if (u_tmp->rx_buf) {
      if (copyout(p->pagetable, u_tmp->rx_buf, (char *)rx_buf, u_tmp->len) < 0) {
        status = -1;
        goto done;
      }
      rx_buf += u_tmp->len;
    }
  }
  status = total;

done:
  kfree_page(k_xfers);
  return status;
}

static int
spidev_ioctl(struct file *f, uint64 cmd, uint64 arg)
{
  int ret = 0;
  struct proc *p = myproc();
  struct spidev_data *spidev = f->private_data;
  uint n_ioc = 0;
  struct spi_ioc_transfer *ioc = 0;
  uint32 value;

  if(spidev == 0)
    return -1;

  struct spi_device *dev = spidev->dev;
  acquiresleep(&spidev->lock);
  switch(cmd) {
    case SPI_IOC_RD_MODE:
      value = dev->mode;
      ret = copyout(p->pagetable, arg, (char *)&value, sizeof(value));
      break;
    case SPI_IOC_WR_MODE:
      if(copyin(p->pagetable, (char *)&value, arg, sizeof(value)) < 0 ||
         value > SPI_WORK_MODE_3) {
          ret = -1;
          break;
      }
        
      dev->mode = value;
      ret = 0;
      break;
    case SPI_IOC_RD_MAX_SPEED_HZ:
      value = spidev->speed_hz;
      ret = copyout(p->pagetable, arg, (char *)&value, sizeof(value));
      break;
    case SPI_IOC_WR_MAX_SPEED_HZ:
      if(copyin(p->pagetable, (char *)&value, arg, sizeof(value)) < 0 ||
         value == 0 || value > dev->max_speed_hz) {
          ret = -1;
          break;
      }
        
      spidev->speed_hz = value;
      ret = 0;
      break;
    default:
      ioc = spidev_get_ioc_message(cmd, (struct spi_ioc_transfer *)arg, &n_ioc);
      if (!ioc) {
        ret = -1;
        break;
      }

      if(!n_ioc)
        break;

      ret = spidev_message(spidev, ioc, n_ioc);
      kfree(ioc);
      break;
  }
  releasesleep(&spidev->lock);

  return ret;
}

static const struct file_operations spidev_ops = {
  .open = spidev_open,
  .read = spidev_read,
  .write = spidev_write,
  .ioctl = spidev_ioctl,
  .close = spidev_close,
};

void
spidev_init(void)
{
  // gpiohs_set_drive_mode(SD_SELECT, GPIO_DM_OUTPUT);
    gpiohs_set_drive_mode(W25Q64_SELECT, GPIO_DM_OUTPUT);
    // gpiohs_set_pin(SD_SELECT, GPIO_PV_HIGH);
    gpiohs_set_pin(W25Q64_SELECT, GPIO_PV_HIGH);
    
  spi_init();
  if(device_register(DEV_SPI, "spi", &spidev_ops) < 0)
    panic("spi device register");
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
w25q64_get_id(void)
{
  uint8 rx[4];
  uint8 cmd[4];

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
