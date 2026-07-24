#ifndef __DEV_H
#define __DEV_H

// Stable major numbers for dev(omode, major, minor).
#define DEV_CONSOLE  1
#define DEV_STATS    2
#define DEV_SPI      3
#define DEV_I2C      4
#define DEV_SDCARD   5
#define DEV_KBUF     6

// Stable logical device numbers. Hardware topology stays in board code.
#define SPI_DEV_W25Q64  0

#define I2C_DEV_OLED     0
#define I2C_DEV_MPU6050  1

#endif
