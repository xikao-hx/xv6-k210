#include "device.h"
#include "dev.h"
#include "dmac.h"
#include "i2c_device.h"

static struct i2c_controller i2c_controllers[] = {
  { .bus_num = I2C_DEVICE_0 },
};

static const struct i2c_device i2c_devices[] = {
  {
    .name = "oled",
    .minor = I2C_DEV_OLED,
    .controller = &i2c_controllers[0],
    .address = 0x3c,
    .max_hz = 100000,
  },
  {
    .name = "mpu6050",
    .minor = I2C_DEV_MPU6050,
    .controller = &i2c_controllers[0],
    .address = 0x68,
    .max_hz = 50000,
  },
};

void
k210_devices_init(void)
{
  initsleeplock(&i2c_controllers[0].lock, "i2c0");
}

const struct i2c_device *
i2c_device_get(int minor)
{
  for(uint i = 0; i < NELEM(i2c_devices); i++) {
    if(i2c_devices[i].minor == minor)
      return &i2c_devices[i];
  }
  return 0;
}

int
i2c_controller_transfer(struct i2c_controller *ctl,
                        const struct i2c_device *dev,
                        struct i2c_msg *msgs, int nmsgs)
{
  int ret;

  if(ctl == 0 || dev == 0 || msgs == 0 || nmsgs <= 0 ||
     dev->controller != ctl)
    return -1;

  acquiresleep(&ctl->lock);
  i2c_init(ctl->bus_num, dev->address, 7, dev->max_hz);
  ret = i2c_transfer(ctl->bus_num, DMAC_CHANNEL2, DMAC_CHANNEL3,
                     msgs, nmsgs);
  releasesleep(&ctl->lock);
  return ret;
}
