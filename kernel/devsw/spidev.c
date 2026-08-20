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
