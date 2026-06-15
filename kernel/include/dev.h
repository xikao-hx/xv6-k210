#ifndef __DEV_H
#define __DEV_H

// Stable major numbers for dev(omode, major, minor).
#define DEV_CONSOLE  1
#define DEV_STATS    2
#define DEV_SPI      3
#define DEV_I2C      4
#define DEV_SDCARD   5
#define DEV_UART     6

// Minor number encodings exposed to user space.
#define SPI_MINOR(bus, chip_select)  (((bus) << 2) | ((chip_select) & 3))
#define SPI_MINOR_BUS(minor)         (((minor) >> 2) & 3)
#define SPI_MINOR_CS(minor)          ((minor) & 3)

#define I2C_MINOR(bus)               ((bus) & 3)
#define I2C_MINOR_BUS(minor)         ((minor) & 3)

#endif
