#ifndef __SPI_H
#define __SPI_H

#include "types.h"
#include "stdbool.h"
#include "spi-dw.h"
#include "sleeplock.h"
#include "spinlock.h"
#include "spi_board.h"
#include "dmac.h"

struct spi_transfer {
    const uint8 *tx_buf;
    uint8 *rx_buf;
    uint32 len;
};

struct spi_dw_data {
    uint8 index;
    void *rx_buf;
    const void *tx_buf;
    unsigned int tx_count;           /* TX bytes still to send (decremented by frame width) */
    unsigned int bytes_per_word;
    uint64 rx_count;              /* RX bytes still to receive (interrupt mode; isr_lock-held) */
    volatile int xfer_done;       /* transfer finished (interrupt mode) */
    volatile int xfer_err;        /* errno (<0) on RXO/TXO overflow (interrupt mode) */
    dmac_channel_number_t chan_tx;
    dmac_channel_number_t chan_rx;
    uint8 chip_select;
    bool dma_enable;
};

struct spi_controller {
    spi_device_num_t bus_num;
    struct spi_dw_data spi_data;
    struct sleeplock lock;        /* per-controller transfer mutex (existing) */
    struct spinlock isr_lock;     /* ISR <-> waiter coordination (new) */
};

struct spi_device {
  spi_device_num_t bus_num;
  spi_chip_select_t chip_select;
  uint8 cs_gpio;
  uint32 max_speed_hz;
  uint8 mode;
  uint8 bits_per_word;
};

void spi_init(void);
int spi_set_clk_rate(spi_device_num_t spi_num, uint32 hz);
int spi_transfer(struct spi_device *dev, struct spi_transfer *xfers, uint64 num);
int spi_write(struct spi_device *dev, const void *buf, uint64 len);
int spi_read(struct spi_device *dev, void *buf, uint64 len);

#endif /* __SPI_H */
