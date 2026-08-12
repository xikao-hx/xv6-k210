#ifndef __USER_MPU6050_H
#define __USER_MPU6050_H

#include "types.h"

// MPU6050 (accel + gyro) user-space driver over the I2C device interface
// (/dev/mpu6050 -> I2C_IOCTL_TRANSFER).  Pure integer; the sensor is used
// for tilt only in this port (see MPU6050_X_TILT_THRESHOLD).

// 7-bit I2C address and the registers the port touches
#define MPU6050_ADDR      0x68
#define PWR_MGMT_1        0x6B
#define ACCEL_CONFIG      0x1C
#define ACCEL_XOUT_H      0x3B
#define ACCEL_YOUT_H      0x3D
#define ACCEL_ZOUT_H      0x3F
#define WHO_AM_I          0x75

// Default accel range is +/-2 g: 1 g = 16384 LSB (ACCEL_CONFIG = 0x00).
#define MPU6050_LSB_PER_G 16384

// Tilt rule for THIS board: X is horizontal, tilting forward drives +X.
// Flat resting is +/-100 LSB noise; 12 degrees of forward tilt measured
// +3400 LSB, so +2000 is far above noise and well below a real tilt.
#define MPU6050_X_TILT_THRESHOLD 2000

// All functions take the device fd returned by mpu6050_open().
int  mpu6050_open(void);                 // open /dev/mpu6050, returns fd or -1
int  mpu6050_write_reg(int fd, uint8 reg, uint8 val);
int  mpu6050_read_reg(int fd, uint8 reg, uint8 *val);
int  mpu6050_read_s16(int fd, uint8 reg_hi, short *val);
int  mpu6050_read_accel(int fd, short *ax, short *ay, short *az);
int  mpu6050_tilt(int fd);               // 1 = forward tilt, 0 = not, -1 = err
int  mpu6050_reset(int fd);              // soft reset -> wake -> +/-2g, 0/-1
int  mpu6050_to_mg(int raw);             // raw LSB -> integer milli-g

#endif
