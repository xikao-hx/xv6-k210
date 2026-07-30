#ifndef _I2C_CONFIG_H
#define _I2C_CONFIG_H

#include "types.h"
#include "dev.h"
#include "i2c.h"

struct i2cdev_data {
    int minor;
    struct i2c_device *dev;
    struct sleeplock lock;
};

struct i2c_device i2c_oled_dev = {
    .bus_num = I2C_DEVICE_0,
    .slave_address = 0x3c,
    .address_width = 7,
};

struct i2c_device i2c_mpu6050_dev = {
    .bus_num = I2C_DEVICE_0,
    .slave_address = 0x68,
    .address_width = 7,
};

static struct i2c_device *i2c_devices[] = {
    [I2C_DEV_OLED] = &i2c_oled_dev,
    [I2C_DEV_MPU6050] = &i2c_mpu6050_dev,
};

#endif /* _I2C_CONFIG_H */
