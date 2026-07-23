#include "types.h"
#include "fcntl.h"
#include "i2cdev.h"
#include "oled.h"
#include "user.h"

#define OLED_ADDR 0x3C

static int fd;

static int
i2c_probe_write(uint8 addr)
{
  uint8 cmd[2] = {0x00, 0xAE};
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

int
main(void)
{
  int fails = 0;

  printf("I2C dev test\n");
  printf("============\n");

  fd = open("/dev/oled", O_RDWR);
  if (fd < 0) {
    printf("FAIL: dev(DEV_I2C)\n");
    exit(1);
  }

  if (i2c_probe_write(OLED_ADDR) < 0) {
    printf("FAIL: probe OLED at 0x%x\n", OLED_ADDR);
    fails++;
  } else {
    printf("OLED probe: 0x%x OK\n", OLED_ADDR);
  }
  if (i2c_probe_write(0x68) >= 0) {
    printf("FAIL: OLED fd accepted another I2C address\n");
    fails++;
  } else {
    printf("fixed-address binding: PASS\n");
  }

  close(fd);

  if (oled_init() < 0) {
    printf("FAIL: oled_init\n");
    fails++;
  } else {
    oled_write_row(0, "I2C TEST PASS");
    oled_write_row(1, "OLED 0x3C OK");
    oled_write_row(2, "xv6-k210");
    oled_write_row(3, "devsw/i2c");
    printf("OLED display updated\n");
  }

  printf("I2C test %s, failures=%d\n", fails ? "FAILED" : "PASSED", fails);
  exit(fails ? 1 : 0);
}
