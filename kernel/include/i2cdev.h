#ifndef _I2CDEV_H
#define _I2CDEV_H

#include "types.h"
#include "dev.h"
#include "i2c.h"

#define I2C_IOCTL_TRANSFER  4  // arg = &i2c_transfer

#define I2C_MAX_MSGS  2

struct i2c_rdwr_ioctl_data {
    uint32 nmsgs;
    struct i2c_msg *msgs;
};

void i2cdev_init(void);

#endif /* _I2CDEV_H */
