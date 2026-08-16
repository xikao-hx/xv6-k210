#ifndef __I2C_BOARD_H
#define __I2C_BOARD_H

#include "i2c.h"
#include "dev.h"

extern struct i2c_controller *i2c_ctrls[I2C_DEVICE_MAX];
struct i2c_device *i2c_device_get(int minor);

#endif /* __I2C_BOARD_H */
