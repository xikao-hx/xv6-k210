#ifndef _SPI_BOARD_H
#define _SPI_BOARD_H

#include "spi.h"
#include "dev.h"

extern struct spi_controller *spi_ctrls[SPI_DEVICE_MAX];
struct spi_device *spi_device_get(int minor);

#endif /* _SPI_BOARD_H */
