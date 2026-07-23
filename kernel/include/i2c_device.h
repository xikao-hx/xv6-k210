#ifndef __I2C_DEVICE_H
#define __I2C_DEVICE_H

#include "i2c.h"
#include "sleeplock.h"
#include "types.h"

struct i2c_controller {
  i2c_device_number_t bus_num;
  struct sleeplock lock;
};

struct i2c_device {
  const char *name;
  int minor;
  struct i2c_controller *controller;
  uint16 address;
  uint32 max_hz;
};

const struct i2c_device *i2c_device_get(int);
int i2c_controller_transfer(struct i2c_controller *,
                            const struct i2c_device *,
                            struct i2c_msg *, int);

#endif
