#include "types.h"
#include "fcntl.h"
#include "i2cdev.h"
#include "mpu6050.h"
#include "user.h"

int
mpu6050_write_reg(int fd, uint8 reg, uint8 val)
{
  uint8 buf[2] = {reg, val};
  struct i2c_msg msg;
  struct i2c_rdwr_ioctl_data xfer;

  msg.addr = MPU6050_ADDR;
  msg.flags = 0;
  msg.len = 2;
  msg.buf = buf;
  xfer.nmsgs = 1;
  xfer.msgs = &msg;
  return ioctl(fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

int
mpu6050_read_reg(int fd, uint8 reg, uint8 *val)
{
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data xfer;

  msgs[0].addr = MPU6050_ADDR;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg;
  msgs[1].addr = MPU6050_ADDR;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 1;
  msgs[1].buf = val;
  xfer.nmsgs = 2;
  xfer.msgs = msgs;
  return ioctl(fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

int
mpu6050_read_s16(int fd, uint8 reg_hi, short *val)
{
  uint8 buf[2];
  struct i2c_msg msgs[2];
  struct i2c_rdwr_ioctl_data xfer;

  msgs[0].addr = MPU6050_ADDR;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = &reg_hi;
  msgs[1].addr = MPU6050_ADDR;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 2;
  msgs[1].buf = buf;
  xfer.nmsgs = 2;
  xfer.msgs = msgs;
  if (ioctl(fd, I2C_IOCTL_TRANSFER, (uint64)&xfer) == 0) {
    *val = (short)((buf[0] << 8) | buf[1]);
    return 0;
  }
  return -1;
}

int
mpu6050_open(void)
{
  return open("/dev/mpu6050", O_RDWR);
}

int
mpu6050_read_accel(int fd, short *ax, short *ay, short *az)
{
  if (mpu6050_read_s16(fd, ACCEL_XOUT_H, ax) != 0)
    return -1;
  if (mpu6050_read_s16(fd, ACCEL_YOUT_H, ay) != 0)
    return -1;
  if (mpu6050_read_s16(fd, ACCEL_ZOUT_H, az) != 0)
    return -1;
  return 0;
}

int
mpu6050_tilt(int fd)
{
  short ax;

  if (mpu6050_read_s16(fd, ACCEL_XOUT_H, &ax) != 0)
    return -1;
  return ax > MPU6050_X_TILT_THRESHOLD;
}

int
mpu6050_reset(int fd)
{
  if (mpu6050_write_reg(fd, PWR_MGMT_1, 0x80) < 0)   // DEVICE_RESET
    return -1;
  sleep(20);                                         // ~100 ms reset time
  if (mpu6050_write_reg(fd, PWR_MGMT_1, 0x01) < 0)   // wake (PLL X-axis gyro)
    return -1;
  if (mpu6050_write_reg(fd, ACCEL_CONFIG, 0x00) < 0) // +/-2 g (default)
    return -1;
  sleep(2);                                          // 10 ms settle
  return 0;
}

int
mpu6050_to_mg(int raw)
{
  return raw * 1000 / MPU6050_LSB_PER_G;
}
