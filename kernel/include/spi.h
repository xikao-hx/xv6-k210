/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef _SPI_H
#define _SPI_H

#include "types.h"
#include "dmac.h"
#include "stdbool.h"
#include "spi-dw.h"
#include "sleeplock.h"

struct spi_transfer {
    const uint8 *tx_buf;
    uint8 *rx_buf;
    uint32 len;
};

struct spi_dw_data {
    uint8 index;
    void *rx_buf;
    const void *tx_buf;
    unsigned int count;
    unsigned int bytes_per_word;
    dmac_channel_number_t chan_tx;
    dmac_channel_number_t chan_rx;
    spi_chip_select_t chip_select;
    bool dma_enable;
};

struct spi_controller {
    spi_device_num_t bus_num;
    struct spi_dw_data spi_data;
    struct sleeplock lock;
};

struct spi_device {
  spi_device_num_t bus_num;
  spi_chip_select_t chip_select;
  uint32 max_speed_hz;
  uint8 mode;
  uint8 bits_per_word;
};

void spi_init(void);
int spi_set_clk_rate(spi_device_num_t spi_num, uint32 hz);
int spi_transfer(struct spi_device *dev, struct spi_transfer *xfers, uint64 num);
int spi_write(struct spi_device *dev, const void *buf, uint64 len);
int spi_read(struct spi_device *dev, void *buf, uint64 len);

#endif /* _DRIVER_SPI_H */
