#include "types.h"
#include "i2cdev.h"
#include "user.h"

/* MPU6050 register addresses */
#define MPU6050_ADDR      0x68

#define SMPLRT_DIV        0x19
#define CONFIG            0x1A
#define GYRO_CONFIG       0x1B
#define ACCEL_CONFIG      0x1C
#define ACCEL_XOUT_H      0x3B
#define ACCEL_XOUT_L      0x3C
#define ACCEL_YOUT_H      0x3D
#define ACCEL_YOUT_L      0x3E
#define ACCEL_ZOUT_H      0x3F
#define ACCEL_ZOUT_L      0x40
#define TEMP_OUT_H        0x41
#define TEMP_OUT_L        0x42
#define GYRO_XOUT_H       0x43
#define GYRO_XOUT_L       0x44
#define GYRO_YOUT_H       0x45
#define GYRO_YOUT_L       0x46
#define GYRO_ZOUT_H       0x47
#define GYRO_ZOUT_L       0x48
#define PWR_MGMT_1        0x6B
#define PWR_MGMT_2        0x6C
#define WHO_AM_I          0x75

static int i2c_fd;

static int mpu_init(void)
{
  struct i2cdev_init cfg;
  cfg.clk_rate = 50000;
  cfg.slave_addr = MPU6050_ADDR;

  i2c_fd = dev(0, I2C_DEV_MAJOR, 0);  // I2C0
  if(i2c_fd < 0) {
    printf("mpu6050: dev() failed\n");
    return -1;
  }
  if(ioctl(i2c_fd, I2C_IOCTL_INIT, (uint64)&cfg) < 0) {
    printf("mpu6050: ioctl init failed\n");
    return -1;
  }
  return 0;
}

static int mpu_write_reg(uint8 reg, uint8 val)
{
  uint8 buf[2] = {reg, val};
  struct i2c_msg msg;
  struct i2c_transfer xfer;
  msg.addr = MPU6050_ADDR;
  msg.flags = 0;
  msg.len = 2;
  msg.buf = (uint64)buf;
  xfer.nmsgs = 1;
  xfer.msgs[0] = msg;
  return ioctl(i2c_fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

static int mpu_read_reg(uint8 reg, uint8 *val)
{
  struct i2c_msg msgs[2];
  struct i2c_transfer xfer;
  msgs[0].addr = MPU6050_ADDR;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = (uint64)&reg;
  msgs[1].addr = MPU6050_ADDR;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 1;
  msgs[1].buf = (uint64)val;
  xfer.nmsgs = 2;
  xfer.msgs[0] = msgs[0];
  xfer.msgs[1] = msgs[1];
  return ioctl(i2c_fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

static int mpu_read_s16(uint8 reg_hi, short *val)
{
  uint8 buf[2];
  struct i2c_msg msgs[2];
  struct i2c_transfer xfer;
  msgs[0].addr = MPU6050_ADDR;
  msgs[0].flags = 0;
  msgs[0].len = 1;
  msgs[0].buf = (uint64)&reg_hi;
  msgs[1].addr = MPU6050_ADDR;
  msgs[1].flags = I2C_M_RD;
  msgs[1].len = 2;
  msgs[1].buf = (uint64)buf;
  xfer.nmsgs = 2;
  xfer.msgs[0] = msgs[0];
  xfer.msgs[1] = msgs[1];
  int ret = ioctl(i2c_fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
  if(ret == 0)
    *val = (short)((buf[0] << 8) | buf[1]);
  return ret;
}

int main(void)
{
  printf("MPU6050 Test on I2C0\n");
  printf("====================\n");

  if(mpu_init() < 0) {
    printf("FAIL: mpu_init\n");
    exit(1);
  }

  /* 1. Read WHO_AM_I */
  uint8 id;
  if(mpu_read_reg(WHO_AM_I, &id) < 0) {
    printf("FAIL: mpu_read_reg(WHO_AM_I)\n");
    exit(1);
  }
  printf("WHO_AM_I:  0x%x  (expected 0x68)\n", id);
  if(id != MPU6050_ADDR) {
    printf("WARNING: unexpected device ID (is MPU6050 connected?)\n");
  }

  /* 2. Initialize MPU6050 */
  printf("Initializing MPU6050...\n");

  if(mpu_write_reg(PWR_MGMT_1, 0x01) < 0) {  // clock = PLL X-axis gyro
    printf("FAIL: mpu_write_reg(PWR_MGMT_1)\n");
    exit(1);
  }
  sleep(1);

  if(mpu_write_reg(PWR_MGMT_2, 0x00) < 0) {  // no low-power modes
    printf("FAIL: mpu_write_reg(PWR_MGMT_2)\n");
    exit(1);
  }
  if(mpu_write_reg(SMPLRT_DIV, 0x09) < 0) {
    printf("FAIL: mpu_write_reg(SMPLRT_DIV)\n");
    exit(1);
  }
  if(mpu_write_reg(CONFIG, 0x06) < 0) {
    printf("FAIL: mpu_write_reg(CONFIG)\n");
    exit(1);
  }
  if(mpu_write_reg(GYRO_CONFIG, 0x18) < 0) {  // ±2000°/s
    printf("FAIL: mpu_write_reg(GYRO_CONFIG)\n");
    exit(1);
  }
  if(mpu_write_reg(ACCEL_CONFIG, 0x18) < 0) {  // ±16g
    printf("FAIL: mpu_write_reg(ACCEL_CONFIG)\n");
    exit(1);
  }
  printf("OK\n");

  /* 3. Read sensor data */
  short ax, ay, az, temp, gx, gy, gz;

  if(mpu_read_s16(ACCEL_XOUT_H, &ax) < 0 ||
     mpu_read_s16(ACCEL_YOUT_H, &ay) < 0 ||
     mpu_read_s16(ACCEL_ZOUT_H, &az) < 0) {
    printf("FAIL: mpu_read_s16(ACCEL)\n");
    exit(1);
  }

  if(mpu_read_s16(TEMP_OUT_H, &temp) < 0) {
    printf("FAIL: mpu_read_s16(TEMP)\n");
    exit(1);
  }

  if(mpu_read_s16(GYRO_XOUT_H, &gx) < 0 ||
     mpu_read_s16(GYRO_YOUT_H, &gy) < 0 ||
     mpu_read_s16(GYRO_ZOUT_H, &gz) < 0) {
    printf("FAIL: mpu_read_s16(GYRO)\n");
    exit(1);
  }

  printf("\nSensor Data:\n");
  printf("  Accel:  X=%d  Y=%d  Z=%d\n", ax, ay, az);
  printf("  Gyro:   X=%d  Y=%d  Z=%d\n", gx, gy, gz);
  printf("  Temp:   %d (%.1f C)\n", temp, (double)temp / 340.0 + 36.53);

  printf("\nMPU6050 test PASSED!\n");
  exit(0);
}
