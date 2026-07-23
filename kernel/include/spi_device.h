#ifndef __SPI_DEVICE_H
#define __SPI_DEVICE_H

#include "sleeplock.h"
#include "spi.h"
#include "types.h"

struct spi_controller {
  spi_device_num_t bus_num;
  struct sleeplock lock;
};

struct spi_device {
  const char *name;
  int minor;
  struct spi_controller *controller;
  spi_chip_select_t chip_select;
  uint32 default_hz;
  uint32 max_hz;
  uint8 mode;
  uint8 bits_per_word;
};

struct spi_file_context {
  const struct spi_device *device;
  struct sleeplock lock;
  uint32 speed_hz;
  uint8 mode;
  uint8 bits_per_word;
};

const struct spi_device *spi_device_get(int);
int spi_controller_transfer(struct spi_controller *,
                            const struct spi_device *,
                            const struct spi_file_context *,
                            struct spi_transfer *, int);

#endif
