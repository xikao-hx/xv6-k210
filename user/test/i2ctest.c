#include "types.h"
#include "fcntl.h"
#include "i2cdev.h"
#include "oledfb.h"
#include "oled.h"
#include "mpu6050.h"
#include "user.h"

// usage: i2ctest [oled | mpu | all]   (default: all)

#define OLED_ADDR 0x3C
#define MPU_ADDR  0x68

static int
i2c_probe_write(int fd, uint8 addr)
{
  uint8 cmd[2] = {0x00, 0xAE};   /* OLED display-off cmd; harmless to MPU */
  struct i2c_msg msg;
  struct i2c_rdwr_ioctl_data xfer;

  msg.addr = addr;
  msg.flags = 0;
  msg.len = sizeof(cmd);
  msg.buf = cmd;

  xfer.nmsgs = 1;
  xfer.msgs = &msg;
  return ioctl(fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

static int
test_oled(void)
{
  int fd;
  int fails = 0;

  printf("--- OLED (0x3c) ---\n");

  fd = open("/dev/oled", O_RDWR);
  if (fd < 0) {
    printf("  FAIL: open /dev/oled\n");
    return 1;
  }

  if (i2c_probe_write(fd, OLED_ADDR) < 0) {
    printf("  FAIL: probe 0x%x\n", OLED_ADDR);
    fails++;
  } else {
    printf("  probe 0x%x: OK\n", OLED_ADDR);
  }

  close(fd);

  OLED_init();   /* opens /dev/oledfb + mmap; exits on failure */
  OLED_Clear();
  OLED_ShowString(0, 0, "I2C TEST PASS", OLED_8X16);
  OLED_ShowString(0, 16, "OLED 0x3C OK", OLED_8X16);
  OLED_ShowString(0, 32, "xv6-k210", OLED_8X16);
  OLED_ShowString(0, 48, "devsw/i2c", OLED_8X16);
  if (OLED_Flush() < 0) {
    printf("  FAIL: display flush\n");
    fails++;
  } else {
    printf("  display via oledfb: OK\n");
  }

  return fails;
}

static int
test_mpu(void)
{
  int fd;
  int fails = 0;
  uint8 id = 0;
  short ax, ay, az;

  printf("--- MPU6050 (0x68) ---\n");

  fd = mpu6050_open();
  if (fd < 0) {
    printf("  FAIL: open /dev/mpu6050\n");
    return 1;
  }

  if (i2c_probe_write(fd, MPU_ADDR) < 0) {
    printf("  FAIL: probe 0x%x\n", MPU_ADDR);
    fails++;
  } else {
    printf("  probe 0x%x: OK\n", MPU_ADDR);
  }

  if (mpu6050_read_reg(fd, WHO_AM_I, &id) < 0) {
    printf("  FAIL: read WHO_AM_I\n");
    fails++;
  } else if (id != MPU_ADDR) {
    printf("  FAIL: WHO_AM_I 0x%x, want 0x%x\n", id, MPU_ADDR);
    fails++;
  } else {
    printf("  WHO_AM_I 0x%x: OK\n", id);
  }

  if (mpu6050_reset(fd) < 0) {
    printf("  FAIL: reset\n");
    fails++;
  }

  if (mpu6050_read_accel(fd, &ax, &ay, &az) < 0) {
    printf("  FAIL: read accel\n");
    fails++;
  } else {
    printf("  accel X/Y/Z: %d %d %d (mg: %d %d %d)\n",
           ax, ay, az,
           mpu6050_to_mg(ax), mpu6050_to_mg(ay), mpu6050_to_mg(az));
    printf("  tilt (thr +%d): %d\n", MPU6050_X_TILT_THRESHOLD, mpu6050_tilt(fd));
  }

  close(fd);

  return fails;
}

int
main(int argc, char *argv[])
{
  int test_all = 1;
  int test_oled_sel = 1;
  int test_mpu_sel = 1;
  int fails = 0;

  if (argc > 1) {
    test_all = 0;
    test_oled_sel = 0;
    test_mpu_sel = 0;
    for(int i = 1; i < argc; i++) {
      if (strcmp(argv[i], "oled") == 0)
        test_oled_sel = 1;
      else if (strcmp(argv[i], "mpu") == 0)
        test_mpu_sel = 1;
      else if (strcmp(argv[i], "all") == 0)
        test_all = 1;
      else {
        printf("usage: i2ctest [oled | mpu | all]\n");
        printf("  oled  - probe + init OLED (0x3c)\n");
        printf("  mpu   - WHO_AM_I + reset + accel/tilt MPU6050 (0x68)\n");
        printf("  all   - both (default)\n");
        exit(1);
      }
    }
  }

  if (test_all || test_oled_sel)
    fails += test_oled();
  if (test_all || test_mpu_sel)
    fails += test_mpu();

  printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
  exit(fails ? 1 : 0);
}
