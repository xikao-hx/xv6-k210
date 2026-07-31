#ifndef _BOARD_I2C_H
#define _BOARD_I2C_H

#include "i2c.h"
#include "dev.h"

extern struct i2c_controller *i2c_ctrls[I2C_DEVICE_MAX];
struct i2c_device *i2c_device_get(int minor);

#endif /* _BOARD_I2C_H */
