// SPI Protocol Implementation
#include "stdbool.h"
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

volatile spi_t *spi[4] = {
    (volatile spi_t *)SPI0_V,
    (volatile spi_t *)SPI1_V,
    (volatile spi_t *)SPI_SLAVE_V,
    (volatile spi_t *)SPI2_V
};

#define DMAC_WAIT_TIMEOUT 10000UL
#define SPI_POLL_WAIT_TIMEOUT 10000000UL
#define SPI_FIFO_DEPTH 32
#define SPI_DMA_WML 16

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

static int spi_clk_init(uint8 spi_num)
{
    // configASSERT(spi_num < SPI_DEVICE_MAX && spi_num != 2);
    // if(spi_num == 3)
        // sysctl_clock_set_clock_select(SYSCTL_CLOCK_SELECT_SPI3, 1);
    sysctl_clock_enable(SYSCTL_CLOCK_SPI0 + spi_num);
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
        spi_adapter->baudr = 0x14;
    spi_adapter->imr = 0x00;
    spi_adapter->dmacr = 0x00;
    spi_adapter->dmatdlr = 0x10;
    spi_adapter->dmardlr = 0x00;
    spi_adapter->ser = 0x00;
    spi_adapter->ssienr = 0x00;
    spi_adapter->spi_ctrlr0 = 0;

    spi_set_tmod(spi_num, SPI_TMOD_TRANS_RECV);

    struct spi_controller *spi_ctrl = spi_ctrls[spi_num];
    spi_ctrl->bus_num = spi_num;
    spi_ctrl->spi_data.dma_enable = true;
    spi_ctrl->spi_data.index = spi_num;
}

void spi_init(void) {

    for (int i = 0; i < SPI_DEVICE_MAX; i ++) {
        char name[10];
        if (spi_ctrls[i] == 0)
            continue;
        
        spi_dw_init(i);
        snprintf(name, sizeof(name), "spi_%d", i);
        initsleeplock(&spi_ctrls[i]->lock, name);
    }
}

int
spi_set_clk_rate(spi_device_num_t spi_num, uint32 hz)
{
    uint32 input_hz;
    uint32 divisor;

    if(spi_num >= SPI_DEVICE_MAX || hz == 0)
        return -1;
    /*
     * spi_clk_init() fixes the SPI0/SPI1 threshold at zero, so their input
     * clock matches the CPU clock on this K210 configuration. Avoid reading
     * the SPI threshold bitfield here: that read is unreliable on real K210
     * hardware and can collapse the requested divider to its minimum value.
     */
    input_hz = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    if(input_hz == 0)
        return -1;
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

static int spi_dw_poll_transfer(struct spi_dw_data *spi_data, struct spi_transfer *transfer) {

    int ret = 0;
    spi_device_num_t spi_num = spi_data->index;
    volatile spi_t *spi_handle = spi[spi_num];
    spi_transfer_width_t frame_width = spi_get_frame_size(spi_num, spi_handle);
    uint64 tx_len = 0, rx_len = 0;
    uint64 fifo_len, idle;
    int progress;

    spi_data->tx_buf = transfer->tx_buf;
    spi_data->rx_buf = transfer->rx_buf;
    spi_data->count = transfer->len;
    
    if (!spi_data->tx_buf) {
        spi_data->tx_buf = kmalloc(spi_data->count);
    }

    if((spi_data->count % frame_width) != 0)
        return -1;

    tx_len = rx_len = spi_data->count / frame_width;
    /*
     * SPI is full-duplex.  Drain RX while feeding TX so reads longer than the
     * 32-frame FIFO cannot overflow and leave the receive loop stuck forever.
     */
    idle = 0;
    while(rx_len) {
        progress = 0;

        fifo_len = spi_handle->rxflr;
        while(fifo_len && rx_len) {
            switch(frame_width)
            {
                case SPI_TRANS_CHAR:
                    spi_dw_buf_rx_uint8(spi_data);
                    break;
                case SPI_TRANS_SHORT:
                    spi_dw_buf_rx_uint16(spi_data);
                    break;
                case SPI_TRANS_INT:
                default:
                    spi_dw_buf_rx_uint32(spi_data);
                    break;
            }
            fifo_len--;
            rx_len--;
            progress = 1;
        }

        fifo_len = SPI_FIFO_DEPTH - spi_handle->txflr;
        while(fifo_len && tx_len) {
            switch(frame_width)
            {
                case SPI_TRANS_CHAR:
                    spi_dw_buf_tx_uint8(spi_data);
                    break;
                case SPI_TRANS_SHORT:
                    spi_dw_buf_tx_uint16(spi_data);
                    break;
                case SPI_TRANS_INT:
                default:
                    spi_dw_buf_tx_uint32(spi_data);
                    break;
            }
            fifo_len--;
            tx_len--;
            progress = 1;
        }

        if(progress) {
            idle = 0;
        } else if(++idle > SPI_POLL_WAIT_TIMEOUT) {
            ret = -1;
            break;
        }
    }

    return ret;
}

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
    
    /* wait dma trasfer finish */
    dmac_wait_done(spi_data->chan_tx, DMAC_WAIT_TIMEOUT);
    dmac_wait_done(spi_data->chan_rx, DMAC_WAIT_TIMEOUT);
    
    spi_handle->dmacr = 0x00;    // clear dma enable
    
    return 0;
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
    
    spi_dw_dma_xfer(spi_data, write_cmd, read_buf, count);

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
        if (spi_can_dma(spi_data, &xfers[i])) {
            ret = spi_dw_dma_transfer(spi_data, &xfers[i]);
        } else {
            ret = spi_dw_poll_transfer(spi_data, &xfers[i]);
        }
        if(ret < 0)
            break;
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

/* sd 卡 */
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

/* sd 卡 */
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
