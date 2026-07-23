#include "device.h"
#include "dev.h"
#include "dmac.h"
#include "i2c_device.h"
#include "spi_device.h"

static struct spi_controller spi_controllers[] = {
  { .bus_num = SPI_DEVICE_0 },
  { .bus_num = SPI_DEVICE_1 },
};

static struct i2c_controller i2c_controllers[] = {
  { .bus_num = I2C_DEVICE_0 },
};

static const struct spi_device spi_devices[] = {
  {
    .name = "w25q64",
    .minor = SPI_DEV_W25Q64,
    .controller = &spi_controllers[1],
    .chip_select = SPI_CHIP_SELECT_0,
    .default_hz = 1000000,
    .max_hz = 10000000,
    .mode = SPI_WORK_MODE_0,
    .bits_per_word = 8,
  },
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
  initsleeplock(&spi_controllers[0].lock, "spi0");
  initsleeplock(&spi_controllers[1].lock, "spi1");
  initsleeplock(&i2c_controllers[0].lock, "i2c0");
}

const struct spi_device *
spi_device_get(int minor)
{
  for(uint i = 0; i < NELEM(spi_devices); i++) {
    if(spi_devices[i].minor == minor)
      return &spi_devices[i];
  }
  return 0;
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
spi_controller_transfer(struct spi_controller *ctl,
                        const struct spi_device *dev,
                        const struct spi_file_context *ctx,
                        struct spi_transfer *xfers, int nxfers)
{
  int ret;

  if(ctl == 0 || dev == 0 || ctx == 0 || xfers == 0 || nxfers <= 0)
    return -1;
  if(dev->controller != ctl || ctx->device != dev ||
     ctx->speed_hz == 0 || ctx->speed_hz > dev->max_hz ||
     ctx->mode > SPI_WORK_MODE_3 || ctx->bits_per_word != 8)
    return -1;

  acquiresleep(&ctl->lock);
  spi_init(ctl->bus_num, (spi_work_mode_t)ctx->mode,
           SPI_FF_STANDARD, ctx->bits_per_word, 0);
  if(spi_set_clk_rate(ctl->bus_num, ctx->speed_hz) < 0)
    ret = -1;
  else
    ret = spi_transfer(ctl->bus_num, dev->chip_select, xfers, nxfers);
  releasesleep(&ctl->lock);
  return ret;
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
