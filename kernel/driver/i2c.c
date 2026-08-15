/* Copyright 2018 Canaan Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "irq.h"
#include "kalloc.h"
#include "log.h"
#include "memlayout.h"
#include "proc.h"
#include "stdbool.h"
#include "sysctl.h"
#include "trap.h"
#include "errno.h"
#include "i2c_board.h"

#define I2C_INT_TIMEOUT_TICKS 100
#define DMA_THRESHOLD 16
#define I2C_DMA_MAX_LEN 1024
#define I2C_TX_ABRT_SOURCE_MASK 0x0001FFFFU

volatile i2c_t *const i2c[3] = {
    (volatile i2c_t *)I2C0_V,
    (volatile i2c_t *)I2C1_V,
    (volatile i2c_t *)I2C2_V
};

// ---------- I2C Init ----------
extern void i2c_irq(void *ctx);
static void i2c_clk_init(i2c_device_number_t i2c_num)
{
    sysctl_clock_enable(SYSCTL_CLOCK_I2C0 + i2c_num);

    // PLL0 = 390MHz
    // i2c_clk = PLL0 / ((threshold+1) * 2)
    // i2c_clk = PLL0 / 8 = 97.5MHz
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_I2C0 + i2c_num, 3);
    if (sysctl_clock_get_threshold(SYSCTL_THRESHOLD_I2C0 + i2c_num) != 3)
        LOG_W("i2c%d clk_th5 write not reflected\n", i2c_num);
}

void i2c_write_slave_addr(i2c_device_number_t i2c_num, uint16 slave_address) {
    volatile i2c_t *i2c_adapter = i2c[i2c_num];

    /* current only support 7 bits address */
    if(slave_address == 10)
        i2c_adapter->tar = I2C_TAR_ADDRESS(slave_address) | I2C_TAR_10BITADDR_MASTER;
    else
        i2c_adapter->tar = I2C_TAR_ADDRESS(slave_address);
}


void i2c_dw_init(i2c_device_number_t i2c_num) {
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    struct i2c_controller *i2c_ctrl = i2c_ctrls[i2c_num];

    i2c_clk_init(i2c_num);
    /* calculation divider value */
    uint32_t v_i2c_freq = sysctl_clock_get_freq(SYSCTL_CLOCK_I2C0 + i2c_num);
    uint32_t i2c_clk = i2c_ctrl->i2c_data.speed_hz;
    uint16_t v_period_clk_cnt = v_i2c_freq / i2c_clk / 2;

    if(v_period_clk_cnt == 0)
        v_period_clk_cnt = 1;

    /* configurate control register */
    i2c_adapter->enable = 0;
    while(i2c_adapter->enable_status & I2C_ENABLE_STATUS_IC_ENABLE)
        ;
    i2c_adapter->con = I2C_CON_MASTER_MODE | I2C_CON_SLAVE_DISABLE | I2C_CON_RESTART_EN |
                       I2C_CON_SPEED(0);
    i2c_adapter->ss_scl_hcnt = I2C_SS_SCL_HCNT_COUNT(v_period_clk_cnt);  // scl high/low level count
    i2c_adapter->ss_scl_lcnt = I2C_SS_SCL_LCNT_COUNT(v_period_clk_cnt);
    i2c_adapter->intr_mask = 0;   // forbid all I2C interrupt
    /* Interrupt-mode FIFO thresholds: TX_EMPTY fires when the TX FIFO is empty
     * (top it up to 8 in one ISR), RX_FULL when the RX FIFO is completely full
     * (drain 8 at once); sub-threshold RX tails are drained at STOP_DET. */
    i2c_adapter->tx_tl = I2C_TX_TL_VALUE(0);
    i2c_adapter->rx_tl = I2C_RX_TL_VALUE(7);

    /* configurate DMA control */
    i2c_adapter->dma_cr = 0x3;    // enable rx and tx dma
    i2c_adapter->dma_rdlr = 0;    // set up rx adn tx burst size
    i2c_adapter->dma_tdlr = 4;
    i2c_adapter->enable = I2C_ENABLE_ENABLE;
    while(!(i2c_adapter->enable_status & I2C_ENABLE_STATUS_IC_ENABLE))
        ;
    i2c_adapter->sda_hold = I2C_SDA_HOLD_TX(v_period_clk_cnt / 4) |
            I2C_SDA_HOLD_RX(v_period_clk_cnt / 8);

    i2c_ctrl->bus_num = i2c_num;
    i2c_ctrl->i2c_data.index = i2c_num;
}

/* DMAC channel-completion IRQ: clear the channel interrupt and wake any
 * sleeper in dmac_wait_idle_timeout (same pattern as spi.c).  ctx = channel. */
static void
i2c_dma_irq(void *ctx)
{
  dmac_intr((dmac_channel_number_t)(uintptr_t)ctx);
}

void i2c_init(void) {
    static char names[I2C_DEVICE_MAX][10];

    for (int i = 0; i < I2C_DEVICE_MAX; i++) {
        if(i2c_ctrls[i] == 0)
            continue;

        i2c_dw_init(i);
        snprintf(names[i], sizeof(names[i]), "i2c_%d", i);
        initsleeplock(&i2c_ctrls[i]->lock, names[i]);
        initlock(&i2c_ctrls[i]->isr_lock, names[i]);
        irq_register(I2C0_IRQ + i, i2c_irq, i2c_ctrls[i]);
        if (i2c_ctrls[i]->i2c_data.dma_enable) {
            if (i2c_ctrls[i]->i2c_data.chan_tx < DMAC_CHANNEL_MAX)
                irq_register(27 + i2c_ctrls[i]->i2c_data.chan_tx, i2c_dma_irq,
                             (void *)(uintptr_t)i2c_ctrls[i]->i2c_data.chan_tx);
            if (i2c_ctrls[i]->i2c_data.chan_rx < DMAC_CHANNEL_MAX)
                irq_register(27 + i2c_ctrls[i]->i2c_data.chan_rx, i2c_dma_irq,
                             (void *)(uintptr_t)i2c_ctrls[i]->i2c_data.chan_rx);
        }
    }
}

static void
i2c_dw_xfer_msg(struct i2c_controller *i2c_ctrl)
{
  volatile i2c_t *i2c_adapter = i2c[i2c_ctrl->bus_num];
  struct i2c_xfer *xfer = &i2c_ctrl->xfer;

  while (xfer->tx_len && (8 - i2c_adapter->txflr) > 0) {
    uint32 cmd = xfer->tx_src ? I2C_DATA_CMD_DATA(*xfer->tx_src++) : I2C_DATA_CMD_CMD;
    if (xfer->need_restart) { cmd |= I2C_DATA_CMD_RESTART; xfer->need_restart = 0; }
    if (xfer->is_lastmsg && xfer->tx_len == 1) cmd |= I2C_DATA_CMD_STOP;
    i2c_adapter->data_cmd = cmd;
    xfer->tx_len--;
  }
  if (!xfer->tx_len)
    i2c_adapter->intr_mask &= ~I2C_INTR_MASK_TX_EMPTY;  /* sent it all: wait for STOP */
}

// RX_FULL / STOP_DET handler: drain the RX FIFO into the receive buffer.
static void
i2c_dw_read(struct i2c_controller *i2c_ctrl)
{
  volatile i2c_t *i2c_adapter = i2c[i2c_ctrl->bus_num];
  struct i2c_xfer *xfer = &i2c_ctrl->xfer;

  while (xfer->rx_len && i2c_adapter->rxflr)
    *xfer->rx_dst++ = (uint8)i2c_adapter->data_cmd, xfer->rx_len--;
}

// I2C ISR: drive the FIFOs toward completion, then signal the sleeping
// transfer function on STOP_DET / TX_ABRT.
void
i2c_irq(void *ctx)
{
  struct i2c_controller *i2c_ctrl = ctx;
  volatile i2c_t *i2c_adapter = i2c[i2c_ctrl->bus_num];
  uint32 stat = i2c_adapter->intr_stat;
  struct i2c_xfer *xfer = &i2c_ctrl->xfer;

  acquire(&i2c_ctrl->isr_lock);
  if (stat & I2C_INTR_STAT_TX_ABRT) {
    LOG_E("i2c_irq: tx abort bus=%d abrt=%x\n", i2c_ctrl->bus_num,
          i2c_adapter->tx_abrt_source & I2C_TX_ABRT_SOURCE_MASK);
    (void)i2c_adapter->clr_tx_abrt;
    xfer->err = -EIO;
    xfer->done = 1;
    i2c_adapter->intr_mask = 0;                   /* abort: silence everything */
  }
  if (stat & I2C_INTR_STAT_RX_FULL)
    i2c_dw_read(i2c_ctrl);
  if (stat & I2C_INTR_STAT_TX_EMPTY)
    i2c_dw_xfer_msg(i2c_ctrl);
  if (stat & I2C_INTR_STAT_STOP_DET) {
    (void)i2c_adapter->clr_stop_det;
    i2c_dw_read(i2c_ctrl);                        /* tail below the RX threshold, caught here */
    xfer->done = 1;
    i2c_adapter->intr_mask = 0;                   /* transaction over: silence everything */
  }
  if (xfer->done)
    wakeup(&ticks);                      /* fast path for the sleeping waiter */
  release(&i2c_ctrl->isr_lock);
}

// Wait for the in-flight transfer to finish.  Caller must hold isr_lock;
static int
i2c_wait_xfer(struct i2c_controller *i2c_ctrl, uint timeout_ticks)
{
  uint start = ticks;

  for (;;) {
    if (i2c_ctrl->xfer.done) {
      int err = i2c_ctrl->xfer.err;
      i2c_ctrl->xfer.done = 0;    /* consume: arm the next transfer */
      return err;
    }
    if ((uint)(ticks - start) > timeout_ticks) {
      LOG_E("i2c_wait_xfer: TIMEOUT bus=%d err=%d done=%d\n",
            i2c_ctrl->bus_num, i2c_ctrl->xfer.err, i2c_ctrl->xfer.done);
      return -ETIMEDOUT;
    }
    sleep(&ticks, &i2c_ctrl->isr_lock);
  }
}

// ---------- I2C send: interrupt and dma ----------

// Send one message via the interrupt path.  Caller holds isr_lock;
static int
i2c_send_data_int(struct i2c_controller *i2c_ctrl, const uint8 *buf, size_t len,
                  int need_restart, int is_lastmsg)
{
  volatile i2c_t *i2c_adapter = i2c[i2c_ctrl->bus_num];
  struct i2c_xfer *xfer = &i2c_ctrl->xfer;

  (void)i2c_adapter->clr_tx_abrt;
  (void)i2c_adapter->clr_stop_det;
  xfer->tx_src = buf;
  xfer->tx_len = len;
  xfer->rx_dst = 0;
  xfer->rx_len = 0;
  xfer->need_restart = need_restart;
  xfer->is_lastmsg = is_lastmsg;

  i2c_adapter->intr_mask = I2C_INTR_MASK_TX_EMPTY | I2C_INTR_MASK_STOP_DET | I2C_INTR_MASK_TX_ABRT;
  i2c_dw_xfer_msg(i2c_ctrl);   /* prefill the first batch so the bus does not wait on the first TX_EMPTY */
  int ret = i2c_wait_xfer(i2c_ctrl, I2C_INT_TIMEOUT_TICKS);
  i2c_adapter->intr_mask = 0;  
  return ret;
}

// Send one message via DMAC.  Caller holds isr_lock.
static int
i2c_send_data_dma(struct i2c_controller *i2c_ctrl, struct i2c_dw_data *i2c_data,
                  const uint8_t *send_buf, size_t send_buf_len,
                  bool need_restart, bool is_lastmsg)
{
    int ret = 0;
    i2c_device_number_t i2c_num = i2c_data->index;
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    struct i2c_xfer *xfer = &i2c_ctrl->xfer;
    int i;
    int dret;

    uint32_t *buf = kalloc_page();
    if(buf == 0)
        return -ENOMEM;

    /* repeat start */
    buf[0] = send_buf[0];
    if (need_restart) {
        buf[0] |= I2C_DATA_CMD_RESTART;
    }

    for(i = 1; i < send_buf_len; i++) {
        buf[i] = send_buf[i];
    }

    /* stop */
    if (is_lastmsg) {
        buf[send_buf_len - 1] |= I2C_DATA_CMD_STOP;
    }

    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;
    xfer->tx_src = 0; xfer->tx_len = 0;
    xfer->rx_dst = 0; xfer->rx_len = 0;
    xfer->need_restart = 0; xfer->is_lastmsg = 0;
    i2c_adapter->intr_mask = I2C_INTR_MASK_STOP_DET | I2C_INTR_MASK_TX_ABRT;

    /* select dma and send data by dma */
    sysctl_dma_select((sysctl_dma_channel_t)i2c_data->chan_tx, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    dmac_set_single_mode(i2c_data->chan_tx, buf, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, send_buf_len);

    release(&i2c_ctrl->isr_lock);
    dret = dmac_wait_idle_timeout(i2c_data->chan_tx, I2C_INT_TIMEOUT_TICKS);
    acquire(&i2c_ctrl->isr_lock);
    if(dret < 0) {
        LOG_E("i2c dma write tx timeout: bus=%d\n", i2c_num);
        ret = -ETIMEDOUT;
        goto done;
    }

    if (is_lastmsg)
        ret = i2c_wait_xfer(i2c_ctrl, I2C_INT_TIMEOUT_TICKS);

    if (ret == 0 && xfer->err)
        ret = xfer->err;

done:
    i2c_adapter->intr_mask = 0;

    if(ret == 0 && (i2c_adapter->tx_abrt_source & I2C_TX_ABRT_SOURCE_MASK) != 0) {
        LOG_E("i2c dma write abort done: bus=%d abrt=%x status=%x txflr=%d rxflr=%d\n",
                i2c_num, i2c_adapter->tx_abrt_source, i2c_adapter->status,
                i2c_adapter->txflr, i2c_adapter->rxflr);
        ret = -EIO;
    }

    /* clear hardware status */
    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;
    kfree_page((void *)buf);

    return ret;
}

// ---------- I2C recv: interrupt and dma ----------

// Receive one message via the interrupt path.  Caller holds isr_lock
static int
i2c_recv_data_int(struct i2c_controller *i2c_ctrl, uint8 *buf, size_t len,
                  int need_restart, int is_lastmsg)
{
  volatile i2c_t *i2c_adapter = i2c[i2c_ctrl->bus_num];
  struct i2c_xfer *xfer = &i2c_ctrl->xfer;

  (void)i2c_adapter->clr_tx_abrt;
  (void)i2c_adapter->clr_stop_det;

  xfer->tx_src = 0;      /* the ISR generates I2C_DATA_CMD_CMD */
  xfer->tx_len = len;
  xfer->rx_dst = buf;
  xfer->rx_len = len;
  xfer->need_restart = need_restart;
  xfer->is_lastmsg = is_lastmsg;

  i2c_adapter->intr_mask = I2C_INTR_MASK_TX_EMPTY | I2C_INTR_MASK_RX_FULL |
                  I2C_INTR_MASK_STOP_DET | I2C_INTR_MASK_TX_ABRT;
  i2c_dw_xfer_msg(i2c_ctrl);   /* prefill the read commands */
  int ret = i2c_wait_xfer(i2c_ctrl, I2C_INT_TIMEOUT_TICKS);
  i2c_adapter->intr_mask = 0;
  return ret;
}

// Receive one message via DMAC.  Caller holds isr_lock.
static int
i2c_recv_data_dma(struct i2c_controller *i2c_ctrl, struct i2c_dw_data *i2c_data,
                  uint8_t *receive_buf, size_t receive_buf_len,
                  bool need_restart, bool is_lastmsg)
{
    int ret = 0;
    i2c_device_number_t i2c_num = i2c_data->index;
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    struct i2c_xfer *xfer = &i2c_ctrl->xfer;
    size_t i;
    int dret;
    uint32_t *write_cmd = kalloc_page();
    if(write_cmd == 0)
        return -ENOMEM;

    /* repeat start */
    write_cmd[0] = I2C_DATA_CMD_CMD;
    if (need_restart) {
        write_cmd[0] |= I2C_DATA_CMD_RESTART;
    }

    for(i = 1; i < receive_buf_len; i+i2c_ctrl->xfer.done+)
        write_cmd[i] = I2C_DATA_CMD_CMD;

    /* stop */
    if (is_lastmsg)
        write_cmd[receive_buf_len - 1] = I2C_DATA_CMD_CMD | I2C_DATA_CMD_STOP;

    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;
    xfer->tx_src = 0; xfer->tx_len = 0;
    xfer->rx_dst = 0; xfer->rx_len = 0;      /* DMA receives into write_cmd, not xfer->rx_dst */
    xfer->need_restart = 0; xfer->is_lastmsg = 0;
    i2c_adapter->intr_mask = I2C_INTR_MASK_STOP_DET | I2C_INTR_MASK_TX_ABRT;

    /* set up dma rx and tx */
    sysctl_dma_select((sysctl_dma_channel_t)i2c_data->chan_tx, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    sysctl_dma_select((sysctl_dma_channel_t)i2c_data->chan_rx, SYSCTL_DMA_SELECT_I2C0_RX_REQ + i2c_num * 2);

    dmac_set_single_mode(i2c_data->chan_rx, (void *)(&i2c_adapter->data_cmd), write_cmd, DMAC_ADDR_NOCHANGE,
                         DMAC_ADDR_INCREMENT, DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, receive_buf_len);
    /* chan tx I2C_DATA_CMD_CMD --> rx */
    dmac_set_single_mode(i2c_data->chan_tx, write_cmd, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT,
                         DMAC_ADDR_NOCHANGE, DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, receive_buf_len);

    release(&i2c_ctrl->isr_lock);
    dret = dmac_wait_idle_timeout(i2c_data->chan_tx, I2C_INT_TIMEOUT_TICKS);
    acquire(&i2c_ctrl->isr_lock);
    if(dret < 0) {
        LOG_E("i2c dma read tx timeout: bus=%d\n", i2c_num);
        ret = -ETIMEDOUT;
        goto done;
    }

    release(&i2c_ctrl->isr_lock);
    dret = dmac_wait_idle_timeout(i2c_data->chan_rx, I2C_INT_TIMEOUT_TICKS);
    acquire(&i2c_ctrl->isr_lock);
    if(dret < 0) {
        LOG_E("i2c dma read rx timeout: bus=%d\n", i2c_num);
        ret = -ETIMEDOUT;
        goto done;
    }

    if (is_lastmsg)
        ret = i2c_wait_xfer(i2c_ctrl, I2C_INT_TIMEOUT_TICKS);

    if (ret == 0 && xfer->err)
        ret = xfer->err;

done:
    i2c_adapter->intr_mask = 0;

    /* write data to receive buf */
    for(i = 0; i < receive_buf_len; i++)
        receive_buf[i] = (uint8_t)write_cmd[i];

    if(ret == 0 && (i2c_adapter->tx_abrt_source & I2C_TX_ABRT_SOURCE_MASK) != 0) {
        LOG_E("i2c dma read abort: bus=%d abrt=%x status=%x txflr=%d rxflr=%d\n",
              i2c_num, i2c_adapter->tx_abrt_source, i2c_adapter->status,
              i2c_adapter->txflr, i2c_adapter->rxflr);
        ret = -EIO;
    }

    /* clear hardware status */
    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;
    kfree_page((void *)write_cmd);

    return ret;
}

// ---------- I2C: signal entry point: transfer ----------

int i2c_transfer(struct i2c_device *dev, struct i2c_msg *msgs, int num) {

    int ret = 0;
    int i = 0;

    /* Entry validation */
    if(dev == 0 || msgs == 0 || num <= 0)
        return -EINVAL;

    i2c_device_number_t bus_num = dev->bus_num;
    if(bus_num >= I2C_DEVICE_MAX)
        return -EINVAL;
    /* Only 7-bit addresses are currently supported */
    if(dev->address_width != 7)
        return -EINVAL;

    struct i2c_controller *i2c_ctrl = i2c_ctrls[bus_num];
    if(i2c_ctrl == 0)
        return -ENODEV;

    struct i2c_dw_data *i2c_data = &i2c_ctrl->i2c_data;
    volatile i2c_t *i2c_adapter = i2c[bus_num];

    acquiresleep(&i2c_ctrl->lock);
    acquire(&i2c_ctrl->isr_lock);
    for (i = 0; i < num; i ++) {
        i2c_ctrl->xfer.done = 0;
        i2c_ctrl->xfer.err = 0;
        bool is_lastmsg = (i == num - 1);
        bool need_restart = (i > 0);

        /* slave addr */
        i2c_write_slave_addr(bus_num, msgs[i].addr);

        /* read/write: large frames go through DMA, small ones through the
         * interrupt path */
        if (msgs[i].flags & I2C_M_RD) {
            if (i2c_data->dma_enable && msgs[i].len >= DMA_THRESHOLD &&
                msgs[i].len <= I2C_DMA_MAX_LEN)
                ret = i2c_recv_data_dma(i2c_ctrl, i2c_data, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
            else
                ret = i2c_recv_data_int(i2c_ctrl, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
        } else {
            if (i2c_data->dma_enable && msgs[i].len >= DMA_THRESHOLD &&
                msgs[i].len <= I2C_DMA_MAX_LEN)
                ret = i2c_send_data_dma(i2c_ctrl, i2c_data, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
            else
                ret = i2c_send_data_int(i2c_ctrl, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
        }
        if(ret != 0) {
            LOG_E("i2c_transfer: FAIL msg=%d ret=%d flags=%x len=%d restart=%d last=%d abrt=%x status=%x txflr=%d rxflr=%d\n",
                  i, ret, msgs[i].flags, msgs[i].len, need_restart, is_lastmsg,
                  i2c_adapter->tx_abrt_source, i2c_adapter->status,
                  i2c_adapter->txflr, i2c_adapter->rxflr);
            break;
        }
    }
    i2c_adapter->intr_mask = 0;
    release(&i2c_ctrl->isr_lock);
    releasesleep(&i2c_ctrl->lock);

    return ret;
}

#ifdef SW
/* --------------------------------------------------------------------- */
/*  Software I2C (bit-banging) via GPIOHS                               */
/*  Pins: GPIOHS0 → physical pin 30 (SCL), GPIOHS1 → physical pin 31   */
/*        (same pins as I2C0, but driven directly by GPIO)              */
/* --------------------------------------------------------------------- */
#define SW_I2C_SCL_GPIO   0
#define SW_I2C_SDA_GPIO   1

static void sw_i2c_delay(void)
{
    /* ~5 µs at 400 MHz */
    for (volatile int i = 0; i < 200; i++);
}

void sw_i2c_init(void)
{
    /* Re-map the physical pins from I2C0 function to GPIOHS */
    fpioa_set_function(30, FUNC_GPIOHS0);   /* SCL */
    fpioa_set_function(31, FUNC_GPIOHS1);   /* SDA */
    gpiohs_set_drive_mode(SW_I2C_SCL_GPIO, GPIO_DM_OUTPUT);
    gpiohs_set_drive_mode(SW_I2C_SDA_GPIO, GPIO_DM_OUTPUT);
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
}

void sw_i2c_start(void)
{
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_LOW);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_LOW);
    sw_i2c_delay();
}

void sw_i2c_stop(void)
{
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_LOW);
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
}

void sw_i2c_send_byte(uint8_t byte)
{
    /* 8 data bits, MSB first */
    for (int i = 0; i < 8; i++) {
        if (byte & 0x80)
            gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
        else
            gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_LOW);
        byte <<= 1;
        sw_i2c_delay();
        gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
        sw_i2c_delay();
        gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_LOW);
        sw_i2c_delay();
    }
    /* Release SDA, clock in ACK (ignored) */
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_LOW);
    sw_i2c_delay();
}

#endif
