#include "dev.h"
#include "device.h"
#include "string.h"
#include "types.h"

struct device_path {
  const char *path;
  int major;
  int minor;
};

static const struct device_path device_paths[] = {
  { "/dev/console", DEV_CONSOLE, 0 },
  { "/dev/stats", DEV_STATS, 0 },
#ifndef QEMU
  { "/dev/sdcard", DEV_SDCARD, 0 },
  { "/dev/w25q64", DEV_SPI, SPI_DEV_W25Q64 },
  { "/dev/oled", DEV_I2C, I2C_DEV_OLED },
  { "/dev/mpu6050", DEV_I2C, I2C_DEV_MPU6050 },
#endif
};

int
device_path_lookup(const char *path, int *major, int *minor)
{
  for(uint i = 0; i < NELEM(device_paths); i++) {
    int len = strlen(device_paths[i].path);
    if(strlen(path) == len &&
       strncmp(path, device_paths[i].path, len) == 0) {
      *major = device_paths[i].major;
      *minor = device_paths[i].minor;
      return 0;
    }
  }
  return -1;
}
