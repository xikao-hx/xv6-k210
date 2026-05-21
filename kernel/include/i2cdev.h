#ifndef _I2CDEV_H
#define _I2CDEV_H

#include "types.h"

#define I2C_DEV_MAJOR  4

// ioctl commands
#define I2C_IOCTL_INIT      1  // arg = &i2cdev_init
#define I2C_IOCTL_TRANSFER  4  // arg = &i2c_transfer

// I2C bus number encoding in minor number
#define I2C_BUS(minor)  ((minor) & 3)
#define I2C_MAX_MSGS  2

struct i2cdev_init {
    uint32 clk_rate;    // I2C clock rate (e.g. 50000)
    uint32 slave_addr;  // 7-bit slave address (e.g. 0x68)
};

struct i2c_msg {
    uint16 addr;    // slave address (7-bit)
    uint16 flags;
#define I2C_M_RD    0x0001  // read data, from slave to master
    uint16 len;     // msg length
    uint64 buf;     // user-space pointer to msg data
};

struct i2c_transfer {
    uint32 nmsgs;
    struct i2c_msg msgs[I2C_MAX_MSGS];
};

void i2cdev_init(void);

#endif /* _I2CDEV_H */
