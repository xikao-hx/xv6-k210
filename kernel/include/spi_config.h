#ifndef _SPI_CONFIG_H
#define _SPI_CONFIG_H

#include "types.h"
#include "dev.h"
#include "spi.h"

struct spidev_data {
    int minor;
    struct spi_device *dev;
    struct sleeplock lock;
    uint32 speed_hz;
};

struct spi_device spi_w25q64_dev = {
    .bus_num = SPI_DEVICE_1,
    .chip_select = SPI_CHIP_SELECT_0,
    .max_speed_hz = 10000000,
    .mode = SPI_WORK_MODE_0,
    .bits_per_word = 8,
};

struct spi_device spi_sd_dev = {
	.bus_num = SPI_DEVICE_0,
	.chip_select = SPI_CHIP_SELECT_3,
	.max_speed_hz = 10000000,
	.mode = SPI_WORK_MODE_0,
	.bits_per_word = 8,
};

static struct spi_device *spi_devices[SPI_CHIP_SELECT_MAX] = {
    [SPI_DEV_W25Q64] = &spi_w25q64_dev,
};

#endif /* _SPI_CONFIG_H */
