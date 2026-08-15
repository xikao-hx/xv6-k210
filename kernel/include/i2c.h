#ifndef __I2C_DEVICE_H
#define __I2C_DEVICE_H

#include "sleeplock.h"
#include "spinlock.h"
#include "types.h"
#include "i2c-dw.h"
#include "stdbool.h"
#include "dmac.h"

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

// In-flight state of one I2C transaction, shared between the transfer
// function and the ISR (guarded by controller->isr_lock).  One transfer is
// one atomic transaction -- no ring buffer.  The ISR drives the hardware
// FIFOs, the caller blocks on xfer.done.
struct i2c_xfer {
  const uint8 *tx_src;            /* send: source data; recv: 0 (ISR emits CMD) */
  uint tx_cmds;                   /* data_cmd words still to write */
  uint8 *rx_dst;                  /* recv: destination buffer */
  uint rx_left;                   /* bytes still to receive */
  int need_restart;               /* first command word carries RESTART */
  int is_lastmsg;                 /* last command word carries STOP */
  volatile int done;              /* transfer finished (success or error) */
  volatile int err;               /* 0 = ok; <0 = TX_ABRT / timeout */
};

struct i2c_controller {
  i2c_device_number_t bus_num;
  struct i2c_dw_data i2c_data;
  struct sleeplock lock;          /* per-controller transfer mutex (existing) */
  struct spinlock isr_lock;       /* ISR <-> waiter coordination (new) */
  struct i2c_xfer xfer;           /* in-flight transaction (new, isr_lock-held) */
};

// Default transfer timeout: 100 ticks = 500 ms at the K210 5 ms tick.  A
// 129-byte OLED frame takes ~2.9 ms at 400 kHz, so this leaves ample margin
// while still bounding the hang window on an unresponsive/absent slave.
#define I2C_INT_TIMEOUT_TICKS 100

struct i2c_device {
  i2c_device_number_t bus_num;
  uint16 slave_address;
  uint32_t address_width;
};

void i2c_init(void);
int i2c_transfer(struct i2c_device *dev, struct i2c_msg *msgs, int num);
void i2c_irq(void *ctx);          // PLIC handler for the I2C IRQ

#ifdef SW

/* software i2c */
void sw_i2c_init(void);
void sw_i2c_start(void);
void sw_i2c_stop(void);
void sw_i2c_send_byte(uint8_t byte);

#endif

#endif
