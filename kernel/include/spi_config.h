#ifndef _SPI_CONFIG_H
#define _SPI_CONFIG_H

#include "types.h"
#include "dmac.h"
#include "memlayout.h"
#include "spi.h"

volatile spi_t *spi[4] = {
    (volatile spi_t *)SPI0_V,
    (volatile spi_t *)SPI1_V,
    (volatile spi_t *)SPI_SLAVE_V,
    (volatile spi_t *)SPI2_V
};

struct spi_dw_data spi_data_0 = {
    .index = SPI_DEVICE_0,
    .chan_tx = DMAC_CHANNEL0,
    .chan_rx = DMAC_CHANNEL1,
};

struct spi_dw_data spi_data_1 = {
    .index = SPI_DEVICE_1,
    .chan_tx = DMAC_CHANNEL4,
    .chan_rx = DMAC_CHANNEL5,
};

struct spi_dw_data *spi_dw[SPI_DEVICE_MAX] = {
    [SPI_DEVICE_0] = &spi_data_0,
    [SPI_DEVICE_1] = &spi_data_1,
};

#endif /* _SPI_ROUTER_H */
