#ifndef __DEV_H
#define __DEV_H

#include "param.h"

// Stable major numbers for dev(omode, major, minor).
#define DEV_CONSOLE  1
#define DEV_STATS    2
#define DEV_SPI      3
#define DEV_I2C      4
#define DEV_SDCARD   5

// Stable logical device numbers. Hardware topology stays in board code.
#define SPI_DEV_W25Q64  0

#define I2C_DEV_OLED     0
#define I2C_DEV_MPU6050  1

struct file_operations;
struct device {
  const char *name;
  const struct file_operations *ops;
};

extern struct device devices[NDEV];
int device_register(int major, const char *name, 
                const struct file_operations *ops);
struct device *device_get(int major);

#endif

