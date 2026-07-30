#ifndef __I2C_DEVICE_H
#define __I2C_DEVICE_H

#include "sleeplock.h"
#include "types.h"
#include "dmac.h"
#include "i2c-dw.h"
#include "stdbool.h"

struct i2c_msg {
    uint16 addr;    // slave address (7-bit)
    uint16 flags;
#define I2C_M_RD    0x0001  // read data, from slave to master
    uint16 len;     // msg length
    uint8 *buf;     // user-space pointer to msg data
};

struct i2c_dw_data {
    uint8 index;
    uint32 speed_hz;
    dmac_channel_number_t chan_tx;
    dmac_channel_number_t chan_rx;
    bool dma_enable;
};

struct i2c_controller {
  i2c_device_number_t bus_num;
  struct i2c_dw_data i2c_data;
  struct sleeplock lock;
};

struct i2c_device {
  i2c_device_number_t bus_num;
  uint16 slave_address;
  uint32_t address_width;
};

void i2c_init(void);
int i2c_transfer(struct i2c_device *dev, struct i2c_msg *msgs, int num);

#ifdef SW

/* software i2c */
void sw_i2c_init(void);
void sw_i2c_start(void);
void sw_i2c_stop(void);
void sw_i2c_send_byte(uint8_t byte);

#endif

#endif
