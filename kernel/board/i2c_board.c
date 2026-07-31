/* Board-level I2C definitions — controller instances, device objects.
 *
 * The generic DW I2C driver (kernel/driver/i2c.c) accesses controllers
 * via the extern table i2c_ctrls[] declared in board_i2c.h.  Device
 * descriptors are returned by i2c_device_get().
 */
#include "i2c_board.h"
#include "dev.h"
#include "printf.h"
#include "sleeplock.h"
#include "dmac.h"

/* ------------------------------------------------------------------ */
/*  Controller instances                                              */
/* ------------------------------------------------------------------ */

/* I2C0: 100 kHz, DMA channels 2 (TX) / 3 (RX) */
static struct i2c_controller i2c_ctrl_0 = {
    .i2c_data = {
        .speed_hz = 100000,
        .chan_tx = DMAC_CHANNEL2,
        .chan_rx = DMAC_CHANNEL3,
    },
};

/* Public controller table — the generic driver indexes this by bus number */
struct i2c_controller *i2c_ctrls[I2C_DEVICE_MAX] = {
    [I2C_DEVICE_0] = &i2c_ctrl_0,
};

/* ------------------------------------------------------------------ */
/*  Device descriptors (static const — never modified after boot)     */
/* ------------------------------------------------------------------ */

/* OLED display (SSD1306) on I2C0, address 0x3c, 7-bit */
static struct i2c_device i2c_oled_dev = {
    .bus_num = I2C_DEVICE_0,
    .slave_address = 0x3c,
    .address_width = 7,
};

/* MPU6050 accelerometer / gyroscope on I2C0, address 0x68, 7-bit */
static struct i2c_device i2c_mpu6050_dev = {
    .bus_num = I2C_DEVICE_0,
    .slave_address = 0x68,
    .address_width = 7,
};

static struct i2c_device *i2c_devices[] = {
    [I2C_DEV_OLED]    = &i2c_oled_dev,
    [I2C_DEV_MPU6050] = &i2c_mpu6050_dev,
};

struct i2c_device *
i2c_device_get(int minor)
{
    if(minor < 0 || minor >= (int)(sizeof(i2c_devices) / sizeof(i2c_devices[0])))
        return 0;
    return i2c_devices[minor];
}
