// SPI Protocol Implementation
#include "stdbool.h"
#include "errno.h"
#include "printf.h"
#include "sysctl.h"
#include "utils.h"
#include "riscv.h"
#include "string.h"
#include "kalloc.h"
#include "dmac.h"
#include "memlayout.h"
#include "spi_board.h"
#include "gpiohs.h"
#include "irq.h"
#include "proc.h"
#include "trap.h"

volatile spi_t *spi[4] = {
    (volatile spi_t *)SPI0_V,
    (volatile spi_t *)SPI1_V,
    (volatile spi_t *)SPI2_V
};

#define SPI_FIFO_DEPTH 32
#define SPI_DMA_WML 16
// Interrupt-driven DMA wait budget (ticks @5ms); the DMAC channel completion
// IRQs wake the sleeper, this only guards a lost IRQ / wedged line.
#define SPI_DMA_TIMEOUT_TICKS 2000UL

#define DW_SPI_BUF_RX(type)						\
static void spi_dw_buf_rx_##type(struct spi_dw_data *spi_dw)		\
{									\
	unsigned int val = spi[spi_dw->index]->dr[0];	\
									\
	if (spi_dw->rx_buf) {						\
		*(type *)spi_dw->rx_buf = val;				\
		spi_dw->rx_buf += sizeof(type);			\
	}								\
}

#define DW_SPI_BUF_TX(type)						\
static void spi_dw_buf_tx_##type(struct spi_dw_data *spi_dw)		\
{									\
	type val = 0;							\
									\
	if (spi_dw->tx_buf) {						\
		val = *(type *)spi_dw->tx_buf;				\
		spi_dw->tx_buf += sizeof(type);			\
	}								\
									\
	spi_dw->count -= sizeof(type);					\
									\
	spi[spi_dw->index]->dr[0] = val;			\
}

DW_SPI_BUF_RX(uint8)
DW_SPI_BUF_TX(uint8)
DW_SPI_BUF_RX(uint16)
DW_SPI_BUF_TX(uint16)
DW_SPI_BUF_RX(uint32)
DW_SPI_BUF_TX(uint32)

// ---------- SPI: Init and Configuration ----------

static int spi_clk_init(uint8 spi_num)
{
    // configASSERT(spi_num < SPI_DEVICE_MAX && spi_num != 2);
    // if(spi_num == 3)
        // sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_SPI3, 1);
    sysctl_clock_enable(SYSCTL_CLOCK_SPI0 + spi_num);
    // spi_clk = 390MHz
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_SPI0 + spi_num, 0);
    return 0;
}

static void spi_set_tmod(uint8 spi_num, uint32 tmod)
{
    // configASSERT(spi_num < SPI_DEVICE_MAX);
    volatile spi_t *spi_handle = spi[spi_num];
    uint8 tmod_offset = 0;
    switch(spi_num)
    {
        case 0:
        case 1:
        case 2:
            tmod_offset = 8;
            break;
        case 3:
        default:
            tmod_offset = 10;
            break;
    }
    set_bit(&spi_handle->ctrlr0, 3 << tmod_offset, tmod << tmod_offset);
}

static void spi_setup(struct spi_device *dev, spi_frame_format_t frame_format, uint32 endian) {

    spi_device_num_t spi_num = dev->bus_num;
    spi_work_mode_t work_mode = (spi_work_mode_t)dev->mode;
    uint64 data_bit_length = dev->bits_per_word;
    uint8 dfs_offset = 0, frf_offset = 0, work_mode_offset = 0;
    uint32 dfs_mask = 0, frf_mask = 0, work_mode_mask = 0;

    switch(spi_num)
    {
        case 0:
        case 1:
            dfs_offset = 16;
            frf_offset = 21;
            work_mode_offset = 6;

            dfs_mask = 0x1F << 16;
            frf_mask = 0x3 << 21;
            work_mode_mask = 0x3 << 6;
            break;
        case 2:
            // configASSERT(!"Spi Bus 2 Not Support!");
            break;
        case 3:
        default:
            dfs_offset = 0;
            frf_offset = 22;
            work_mode_offset = 8;

            dfs_mask = 0x1F << 0;
            frf_mask = 0x3 << 22;
            work_mode_mask = 0x3 << 8;
            break;
    }

    switch(frame_format)
    {
        case SPI_FF_DUAL:
            // configASSERT(data_bit_length % 2 == 0);
            break;
        case SPI_FF_QUAD:
            // configASSERT(data_bit_length % 4 == 0);
            break;
        case SPI_FF_OCTAL:
            // configASSERT(data_bit_length % 8 == 0);
            break;
        default:
            break;
    }

    volatile spi_t *const spi_adapter = spi[spi_num];
    set_bit(&spi_adapter->ctrlr0, dfs_mask, ((data_bit_length - 1) << dfs_offset));
    set_bit(&spi_adapter->ctrlr0, frf_mask, (frame_format << frf_offset));
    set_bit(&spi_adapter->ctrlr0, work_mode_mask, (work_mode << work_mode_offset));
    spi_adapter->endian = endian;
}

static void spi_dw_init(spi_device_num_t spi_num)
{
    // configASSERT(data_bit_length >= 4 && data_bit_length <= 32);
    // configASSERT(spi_num < SPI_DEVICE_MAX && spi_num != 2);

    spi_clk_init(spi_num);
    
    volatile spi_t *const spi_adapter = spi[spi_num];
    if(spi_adapter->baudr == 0)
        spi_adapter->baudr = 0x40;
    spi_adapter->imr = 0x00;

    /* Interrupt-mode FIFO thresholds. */
    spi_adapter->txftlr = 0x00;
    spi_adapter->rxftlr = 0x00;
    spi_adapter->dmacr = 0x00;
    spi_adapter->dmatdlr = 0x10;
    spi_adapter->dmardlr = 0x00;
    spi_adapter->ser = 0x00;
    spi_adapter->ssienr = 0x00;
    spi_adapter->spi_ctrlr0 = 0;

    spi_set_tmod(spi_num, SPI_TMOD_TRANS_RECV);

    struct spi_controller *spi_ctrl = spi_ctrls[spi_num];
    spi_ctrl->bus_num = spi_num;
    spi_ctrl->spi_data.index = spi_num;
}

static void spi_dw_isr(void *data);
static void spi_dma_isr(void *data);
void spi_init(void) {
    static char names[SPI_DEVICE_MAX][10];

    for (int i = 0; i < SPI_DEVICE_MAX; i ++) {
        if (spi_ctrls[i] == 0)
            continue;

        struct spi_dw_data *spi_data = &spi_ctrls[i]->spi_data;
        spi_dw_init(i);
        snprintf(names[i], sizeof(names[i]), "spi_%d", i);
        initsleeplock(&spi_ctrls[i]->lock, names[i]);
        initlock(&spi_ctrls[i]->isr_lock, names[i]);
        irq_register(SPI0_IRQ + i, spi_dw_isr, spi_ctrls[i]);
        /* DMAC channel completion IRQ = DMAC_CH0_IRQ + channel, only for controllers
         * that actually use DMA (SPI1's CH4/CH5 are owned by UART1). */
        if (spi_data->dma_enable) {
            if (spi_data->chan_tx < DMAC_CHANNEL_MAX)
                irq_register(DMAC_CH0_IRQ + spi_data->chan_tx, spi_dma_isr,
                             (void *)(uintptr_t)spi_data->chan_tx);
            if (spi_data->chan_rx < DMAC_CHANNEL_MAX)
                irq_register(DMAC_CH0_IRQ+ spi_data->chan_rx, spi_dma_isr,
                             (void *)(uintptr_t)spi_data->chan_rx);
        }
    }
}

int
spi_set_clk_rate(spi_device_num_t spi_num, uint32 hz)
{
    uint32 input_hz;
    uint32 divisor;

    if(spi_num >= SPI_DEVICE_MAX || hz == 0)
        return -EINVAL;
    // clock is PLL0/2 = 390MHz
    input_hz = sysctl_clock_get_freq(SYSCTL_CLOCK_SPI0 + spi_num);
    if(input_hz == 0)
        return -EINVAL;
    divisor = (input_hz + hz - 1) / hz;
    if(divisor < 2)
        divisor = 2;
    if(divisor & 1)
        divisor++;
    if(divisor > 0xfffe)
        divisor = 0xfffe;
    spi[spi_num]->baudr = divisor;
    return 0;
}

static spi_transfer_width_t spi_get_frame_size(spi_device_num_t spi_num, volatile spi_t *spi_handle)
{
    uint8 dfs_offset = 0;
    uint32 data_bit_length = 0;
    switch(spi_num)
    {
        case 0:
        case 1:
            dfs_offset = 16;
            break;
        case 2:
            // configASSERT(!"Spi Bus 2 Not Support!");
            break;
        case 3:
        default:
            dfs_offset = 0;
            break;
    }

    data_bit_length = (spi_handle->ctrlr0 >> dfs_offset) & 0x1F;
    if(data_bit_length < 8)
        return SPI_TRANS_CHAR;
    else if(data_bit_length < 16)
        return SPI_TRANS_SHORT;
    return SPI_TRANS_INT;
}

// ---------- SPI: Interrupt transfer ----------

// Shared FIFO engines.
static int
spi_dw_tx_fill(struct spi_dw_data *spi_data, volatile spi_t *spi_master, spi_transfer_width_t width)
{
  int n = 0;
  while (spi_data->count && (SPI_FIFO_DEPTH - spi_master->txflr) > 0) {
    switch (width) {
    case SPI_TRANS_CHAR:  spi_dw_buf_tx_uint8(spi_data);  break;
    case SPI_TRANS_SHORT: spi_dw_buf_tx_uint16(spi_data); break;
    default:              spi_dw_buf_tx_uint32(spi_data); break;
    }
    n++;
  }
  return n;
}

static int
spi_dw_rx_drain(struct spi_dw_data *spi_data, volatile spi_t *spi_master, spi_transfer_width_t width)
{
  int n = 0;
  while (spi_master->rxflr && spi_data->rx_count) {
    switch (width) {
    case SPI_TRANS_CHAR:  spi_dw_buf_rx_uint8(spi_data);  spi_data->rx_count -= 1; break;
    case SPI_TRANS_SHORT: spi_dw_buf_rx_uint16(spi_data); spi_data->rx_count -= 2; break;
    default:              spi_dw_buf_rx_uint32(spi_data); spi_data->rx_count -= 4; break;
    }
    n++;
  }
  return n;
}

// Wait for the in-flight transfer to finish.  Caller must hold spi_ctrl->isr_lock.
static int
spi_wait_xfer(struct spi_controller *spi_ctrl, uint timeout_ticks)
{
  struct proc *p = myproc();
  uint start = ticks;

  for (;;) {
    if (spi_ctrl->spi_data.xfer_done) {
      int err = spi_ctrl->spi_data.xfer_err;
      spi_ctrl->spi_data.xfer_done = 0;   /* consume: arm the next transfer */
      return err;
    }
    if ((uint)(ticks - start) > timeout_ticks) {
      /* We hold isr_lock, so the ISR is parked and cannot be mid-printf --
       * safe to report here.  rxflr>0 at timeout means echoes DID land in the
       * RX FIFO but were never drained (throughput / ISR-delivery problem);
       * rxflr==0 means nothing was clocked out at all. */
      if (p)
        printf("spi%d INT TIMEOUT: xfer_err=%d rxflr=%d isr=0x%x\n",
               spi_ctrl->bus_num, spi_ctrl->spi_data.xfer_err,
               (uint)spi[spi_ctrl->bus_num]->rxflr,
               (uint)spi[spi_ctrl->bus_num]->isr);
      spi_ctrl->spi_data.xfer_done = 0;   /* leave the next transfer armed */
      return -ETIMEDOUT;
    }
    if (p) {
      sleep(&ticks, &spi_ctrl->isr_lock);
    } else {
      release(&spi_ctrl->isr_lock);
      while (!spi_ctrl->spi_data.xfer_done && (uint)(ticks - start) <= timeout_ticks)
        ;
      acquire(&spi_ctrl->isr_lock);
    }
  }
}

// ---------- SPI isr ----------
static void
spi_dma_isr(void *data)
{
  dmac_intr((dmac_channel_number_t)(uintptr_t)data);
}

static void
spi_dw_isr(void *data)
{
  struct spi_controller *spi_ctrl = data;
  struct spi_dw_data *spi_data = &spi_ctrl->spi_data;
  volatile spi_t *spi_master = spi[spi_ctrl->bus_num];
  spi_transfer_width_t width = spi_get_frame_size(spi_ctrl->bus_num, spi_master);
  uint32 isr = spi_master->isr;

  acquire(&spi_ctrl->isr_lock);
  if (isr & SPI_ISR_RXO) {               /* RX FIFO overflow: drained too slowly */
    (void)spi_master->rxoicr;
    spi_data->xfer_err = -EOVERFLOW;
    spi_data->xfer_done = 1;
    spi_master->imr = 0;
  } else if (isr & SPI_ISR_TXO) {        /* TX FIFO overflow: overfed a full FIFO */
    (void)spi_master->txoicr;
    spi_data->xfer_err = -EINVAL;        /* programming error: FIFO overfed */
    spi_data->xfer_done = 1;
    spi_master->imr = 0;
  } else {
    if (isr & SPI_ISR_TXE) {             /* TX FIFO empty (txftlr=0): refill */
      spi_dw_tx_fill(spi_data, spi_master, width);
      if (!spi_data->count)
        spi_master->imr &= ~SPI_IMR_TXE;         /* sent it all: wait for the echoes */
    }
    if (isr & SPI_ISR_RXF) {             /* RX FIFO at threshold (rxftlr=0): drain */
      spi_dw_rx_drain(spi_data, spi_master, width);
      if (!spi_data->rx_count) {                /* last echo in: transfer complete */
        spi_data->xfer_done = 1;
        spi_master->imr = 0;
      }
    }
  }
  if (spi_data->xfer_done)
    wakeup(&ticks);                  
  release(&spi_ctrl->isr_lock);
}

static int
spi_dw_int_transfer(struct spi_dw_data *spi_data, struct spi_transfer *transfer)
{
  struct spi_controller *spi_ctrl = spi_ctrls[spi_data->index];
  volatile spi_t *spi_master = spi[spi_data->index];
  spi_transfer_width_t width = spi_get_frame_size(spi_data->index, spi_master);

  int ret;

  if (transfer->len % width != 0)
    return -EINVAL;

  spi_data->tx_buf = transfer->tx_buf;
  spi_data->rx_buf = transfer->rx_buf;

  acquire(&spi_ctrl->isr_lock);
  spi_data->count = spi_data->rx_count = transfer->len;
  (void)spi_master->icr;                            /* clear any residual interrupt state */
  /* prefill the TX FIFO so the first frames go out without waiting on TXE */
  spi_dw_tx_fill(spi_data, spi_master, width);
  /* Enable TXE (refill), RXF (drain, the ">=1 entry" threshold) plus the two
   * overflow guards.  imr=0x1B: with the corrected bit layout, RXF is bit4. */
  spi_master->imr = SPI_IMR_TXE | SPI_IMR_RXF | SPI_IMR_RXO | SPI_IMR_TXO;
  ret = spi_wait_xfer(spi_ctrl, SPI_INT_TIMEOUT_TICKS);
  spi_master->imr = 0;
  release(&spi_ctrl->isr_lock);
  if (ret < 0)
    printf("spi%d INT len=%d failed: %s\n", spi_data->index, transfer->len,
           ret == -ETIMEDOUT ? "timeout" :
           ret == -EOVERFLOW ? "RX FIFO overflow (ISR too slow)" : "TX FIFO overflow");
  return ret;
}

// ---------- SPI: DMA transfer ----------

static int spi_dw_dma_xfer(struct spi_dw_data *spi_data, const void *tx_buf,
                            void *rx_buf, uint64 len) {
    spi_device_num_t spi_num = spi_data->index;
    volatile spi_t *spi_handle = spi[spi_num];

    spi_handle->dmacr = 0x3;     // enable send and receive dma

    /* configuration dma request source */
    sysctl_dma_select((sysctl_dma_channel_t)spi_data->chan_tx, SYSCTL_DMA_SELECT_SSI0_TX_REQ + spi_num * 2);
    sysctl_dma_select((sysctl_dma_channel_t)spi_data->chan_rx, SYSCTL_DMA_SELECT_SSI0_RX_REQ + spi_num * 2);

    /* configuration dma transfer */
    dmac_set_single_mode(spi_data->chan_rx, (void *)(&spi_handle->dr[0]), rx_buf, DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
                         DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, len);
    dmac_set_single_mode(spi_data->chan_tx, tx_buf, (void *)(&spi_handle->dr[0]), DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                             DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, len);

    /* wait for the DMAC completion IRQs (interrupt-driven, with a timeout
     * fallback so a lost IRQ cannot hang the caller) */
    int ret = dmac_wait_idle_timeout(spi_data->chan_tx, SPI_DMA_TIMEOUT_TICKS);
    if (ret == 0)
        ret = dmac_wait_idle_timeout(spi_data->chan_rx, SPI_DMA_TIMEOUT_TICKS);

    spi_handle->dmacr = 0x00;    // clear dma enable

    return ret;
}

static int spi_dw_dma_transfer(struct spi_dw_data *spi_data, struct spi_transfer *transfer) {

    spi_device_num_t spi_num = spi_data->index;
    volatile spi_t *spi_handle = spi[spi_num];
    spi_transfer_width_t frame_width = spi_get_frame_size(spi_num, spi_handle);
    uint64 len = transfer->len;
    const uint8 *tx_buf = transfer->tx_buf;
    uint8 *rx_buf = transfer->rx_buf;
    uint64 i, count = 0;

    /* alloc dma data buffer to send command data and receive data */
    uint32 *write_cmd = kalloc_page();  ;
    uint32 *read_buf;
    
    /* according to frame width reorganization data */
    switch(frame_width)
    {  
          
        case SPI_TRANS_INT:
            // copy data: solve addr aligen and convert data len to 32bit
            for(i = 0; i < len / 4; i++)
                write_cmd[i] = ((uint32 *)tx_buf)[i];
            read_buf = &write_cmd[i];
            count = len / 4;
            break;
        case SPI_TRANS_SHORT:
            for(i = 0; i < len / 2; i++)
                write_cmd[i] = ((uint16 *)tx_buf)[i];
            read_buf = &write_cmd[i];
            count = len / 2;
            break;
        default:
            for(i = 0; i < len; i++)
                write_cmd[i] = tx_buf[i];
            read_buf = &write_cmd[i];
            count = len;
            break;
    }
    
    int ret = spi_dw_dma_xfer(spi_data, write_cmd, read_buf, count);
    if (ret < 0) {
        kfree_page(write_cmd);
        return ret;
    }

    switch(frame_width)
    {
        case SPI_TRANS_INT:
            for(i = 0; i < count; i++)
                ((uint32 *)rx_buf)[i] = read_buf[i];
            break;
        case SPI_TRANS_SHORT:
            for(i = 0; i < count; i++)
                ((uint16 *)rx_buf)[i] = read_buf[i];
            break;
        default:
            for(i = 0; i < count; i++)
                rx_buf[i] = read_buf[i];
            break;
    }

    kfree_page(write_cmd);

    return 0;
}

// ---------- SPI signal entry point: transfer ----------

static bool spi_can_dma(struct spi_dw_data *spi_data, struct spi_transfer *transfer)
{
    spi_device_num_t spi_num = spi_data->index;
    spi_transfer_width_t frame_width;
    uint32 dma_unit;
    uint64 frames;

    if(!spi_data->dma_enable || !transfer)
        return false;

    if(!transfer->tx_buf || !transfer->rx_buf)
        return false;

    frame_width = spi_get_frame_size(spi_num, spi[spi_num]);
    if(frame_width != SPI_TRANS_CHAR &&
       frame_width != SPI_TRANS_SHORT &&
       frame_width != SPI_TRANS_INT)
        return false;

    dma_unit = SPI_DMA_WML * frame_width;
    if(transfer->len < dma_unit)
        return false;

    if(transfer->len % dma_unit)
        return false;

    frames = transfer->len / frame_width;
    if(frames * 2 * sizeof(uint32) > PGSIZE)
        return false;

    return true;
}

static void spi_set_cs(struct spi_device *dev, bool enable) {

    volatile spi_t *spi_handle = spi[dev->bus_num];
    if (!dev->cs_gpio) {
        /* spi cs */
        if (enable) spi_handle->ser = 1U << dev->chip_select;
        else spi_handle->ser =0;
    } else {
        /* spi cs-gpio */
        spi_handle->ser = 1U << dev->chip_select;    // generate clk
        if (enable) gpiohs_set_pin(dev->cs_gpio, GPIO_PV_LOW);
        else gpiohs_set_pin(dev->cs_gpio, GPIO_PV_HIGH);
    }
}

static int __spi_transfer(struct spi_device *dev, struct spi_transfer *xfers, uint64 num) {

    int ret = 0;
    struct spi_controller *spi_ctrl = spi_ctrls[dev->bus_num];
    struct spi_dw_data *spi_data = &spi_ctrl->spi_data;
    volatile spi_t *spi_handle = spi[dev->bus_num];

    spi_set_cs(dev, true);
    
    spi_handle->ssienr = 0x00;
    spi_setup(dev, SPI_FF_STANDARD, 0);
    spi_handle->ssienr = 0x01;

    for (int i = 0; i < num; i ++) {
        /* Large block-aligned frames go through DMA; all other transfers use
         * the interrupt path.  Before the scheduler starts, spi_wait_xfer()
         * waits for the ISR without sleeping. */
        if (spi_can_dma(spi_data, &xfers[i])) {
            ret = spi_dw_dma_transfer(spi_data, &xfers[i]);
        } else {
            ret = spi_dw_int_transfer(spi_data, &xfers[i]);
        }
        if(ret < 0) {
            printf("spi%d len=%d transfer failed: ret=%d\n", dev->bus_num,
                   xfers[i].len, ret);
            break;
        }
    }
    spi_set_cs(dev, false);
    
    return ret;
}

int spi_transfer(struct spi_device *dev, struct spi_transfer *xfers, uint64 num) {
    int ret;
    
    struct spi_controller *spi_ctrl = spi_ctrls[dev->bus_num];
    acquiresleep(&spi_ctrl->lock);
    ret = __spi_transfer(dev, xfers, num);
    releasesleep(&spi_ctrl->lock);
    return ret;
}

/* sd card */
int spi_write(struct spi_device *dev, const void *buf, uint64 len) 
{
    int ret = 0;
    uint8 *rx = kmalloc(len);
    memset(rx, 0xff, len);
    struct spi_transfer xfer = {
		.tx_buf = buf,
		.rx_buf = rx,
		.len = len,
	};

    ret = __spi_transfer(dev, &xfer, 1);
    kfree(rx);

    return ret;
}

/* sd card */
int spi_read(struct spi_device *dev, void *buf, uint64 len) 
{
    int ret;

    uint8 *tx = kmalloc(len);
    memset(tx, 0xff, len);
    struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = buf,
		.len = len,
	};

    ret = __spi_transfer(dev, &xfer, 1);
    kfree(tx);

    return ret;
}
