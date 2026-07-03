#ifndef _I2CDEV_H
#define _I2CDEV_H

#include "types.h"
#include "dev.h"
#include "i2c.h"

// ioctl commands
#define I2C_IOCTL_INIT      1  // arg = &i2cdev_init
#define I2C_IOCTL_TRANSFER  4  // arg = &i2c_transfer

// I2C bus number encoding in minor number.
#define I2C_BUS(minor)  I2C_MINOR_BUS(minor)
#define I2C_MAX_MSGS  2

struct i2cdev_init {
    uint32 clk_rate;    // I2C clock rate (e.g. 50000)
    uint32 slave_addr;  // 7-bit slave address (e.g. 0x68)
};

struct i2c_rdwr_ioctl_data {
    uint32 nmsgs;
    struct i2c_msg *msgs;
};

void i2cdev_init(void);

#endif /* _I2CDEV_H */
