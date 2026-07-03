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
#include "memlayout.h"
#include "i2c.h"
#include "kalloc.h"
#include "log.h"
#include "sysctl.h"
#include "stdbool.h"

#define I2C_WAIT_TIMEOUT  1000000UL
#define DMAC_WAIT_TIMEOUT 10000UL
#define DMA_THRESHOLD 16

volatile i2c_t *const i2c[3] = {
    (volatile i2c_t *)I2C0_V,
    (volatile i2c_t *)I2C1_V,
    (volatile i2c_t *)I2C2_V
};

bool dma_enable = false;

static int
i2c_wait_done(i2c_device_number_t i2c_num, volatile i2c_t *i2c_adapter)
{
    int timeout = I2C_WAIT_TIMEOUT;

    while(1) {
        if(i2c_adapter->tx_abrt_source != 0) {
            LOG_D("i2c abort: bus=%d abrt=%x status=%x raw=%x txflr=%d rxflr=%d\n",
                  i2c_num, i2c_adapter->tx_abrt_source, i2c_adapter->status,
                  i2c_adapter->raw_intr_stat, i2c_adapter->txflr, i2c_adapter->rxflr);
            (void)i2c_adapter->clr_tx_abrt;
            (void)i2c_adapter->clr_stop_det;
            return -1;
        }

        if(!(i2c_adapter->status & I2C_STATUS_ACTIVITY) &&
           (i2c_adapter->status & I2C_STATUS_TFE)) {
            (void)i2c_adapter->clr_stop_det;
            for (volatile int i = 0; i < 200; i++);
            return 0;
        }

        if(--timeout == 0) {
            LOG_E("i2c wait timeout: bus=%d status=%x raw=%x abrt=%x txflr=%d rxflr=%d\n",
                  i2c_num, i2c_adapter->status, i2c_adapter->raw_intr_stat,
                  i2c_adapter->tx_abrt_source, i2c_adapter->txflr, i2c_adapter->rxflr);
            return -1;
        }
    }
}

// static void i2c_clk_init(i2c_device_number_t i2c_num)
// {
//     // configASSERT(i2c_num < I2C_MAX_NUM);
//     sysctl_clock_enable(SYSCTL_CLOCK_I2C0 + i2c_num);
//     sysctl_clock_set_threshold(SYSCTL_THRESHOLD_I2C0 + i2c_num, 3);  // I2C_clk = PLL0 / 8 ≈ 100MHz
// }

void i2c_init(i2c_device_number_t i2c_num, uint32_t slave_address, uint32_t address_width,
              uint32_t i2c_clk)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    // configASSERT(address_width == 7 || address_width == 10);

    volatile i2c_t *i2c_adapter = i2c[i2c_num];

    /* clock init */
    // i2c_clk_init(i2c_num);

    /* calculation divider value */
    // NOTE: Both sysctl_clock_get_freq(I2C0) and sysctl_clock_get_threshold()
    // return wrong values on real K210 (bitfield register reads are broken).
    // Derive I2C clock from CPU frequency with the known threshold we set above:
    uint32_t v_i2c_freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU) / 4;
    uint16_t v_period_clk_cnt = v_i2c_freq / i2c_clk / 2;

    if(v_period_clk_cnt == 0)
        v_period_clk_cnt = 1;

    /* configurate control register */
    i2c_adapter->enable = 0;
    i2c_adapter->con = I2C_CON_MASTER_MODE | I2C_CON_SLAVE_DISABLE | I2C_CON_RESTART_EN |
                       (address_width == 10 ? I2C_CON_10BITADDR_SLAVE : 0) | I2C_CON_SPEED(0);
    i2c_adapter->ss_scl_hcnt = I2C_SS_SCL_HCNT_COUNT(v_period_clk_cnt);  // scl high/low level count
    i2c_adapter->ss_scl_lcnt = I2C_SS_SCL_LCNT_COUNT(v_period_clk_cnt);

    i2c_adapter->tar = I2C_TAR_ADDRESS(slave_address);
    i2c_adapter->intr_mask = 0;   // forbid all I2C interrupt

    /* configurate DMA control */
    i2c_adapter->dma_cr = 0x3;    // enable rx and tx dma
    i2c_adapter->dma_rdlr = 0;    // set up rx adn tx burst size
    i2c_adapter->dma_tdlr = 4;
    i2c_adapter->enable = I2C_ENABLE_ENABLE;
    dma_enable = true;
}

/* polling */
int i2c_send_data(i2c_device_number_t i2c_num, const uint8_t *send_buf, 
                    size_t send_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    size_t fifo_len, index;

    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;

    /* send circle */
    int first_cmd = 1;
    while(send_buf_len)
    {
        fifo_len = 8 - i2c_adapter->txflr;
        /* write data to fifo */
        fifo_len = send_buf_len < fifo_len ? send_buf_len : fifo_len;
        for(index = 0; index < fifo_len; index++) {
            uint32_t cmd = I2C_DATA_CMD_DATA(*send_buf++);
            if (first_cmd && need_restart) {
                cmd |= I2C_DATA_CMD_RESTART;
            }

            if (is_lastmsg && send_buf_len == 1) {
                cmd |= I2C_DATA_CMD_STOP;
            }
            i2c_adapter->data_cmd = cmd;

            first_cmd = 0;
            send_buf_len--;
        }
       
        /* check transfer error */
        if(i2c_adapter->tx_abrt_source != 0)
            return -1;
    }

    if (is_lastmsg && i2c_wait_done(i2c_num, i2c_adapter) < 0)
        return -1;
    
    LOG_D("i2c dma write ok: bus=%d len=%d restart=%d last=%d status=%x txflr=%d rxflr=%d\n",
          i2c_num, send_buf_len, need_restart, is_lastmsg, i2c_adapter->status,
          i2c_adapter->txflr, i2c_adapter->rxflr);

    return 0;
}

int i2c_send_data_dma(i2c_device_number_t i2c_num, dmac_channel_number_t chan_tx, 
                        const uint8_t *send_buf, size_t send_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    int i;

    uint32_t *buf = kalloc_page();
    if(buf == 0)
        return -1;

    /* clear hardware status */
    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;

    /* deal with addr aligen */
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

    /* select dma and send date by dma */
    sysctl_dma_select((sysctl_dma_channel_t)chan_tx, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    dmac_set_single_mode(chan_tx, buf, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, send_buf_len);

    /* waiting for dma send done */
    dmac_wait_done(chan_tx, DMAC_WAIT_TIMEOUT);
    LOG_D("i2c dma write tx done: status=%x txflr=%d rxflr=%d abrt=%x\n",
          i2c_adapter->status, i2c_adapter->txflr, i2c_adapter->rxflr,
          i2c_adapter->tx_abrt_source);

    kfree_page((void *)buf);

    if (is_lastmsg && i2c_wait_done(i2c_num, i2c_adapter) < 0)
        return -1;

    if(i2c_adapter->tx_abrt_source != 0) {
        LOG_E("i2c dma write abort done: bus=%d abrt=%x status=%x txflr=%d rxflr=%d\n",
              i2c_num, i2c_adapter->tx_abrt_source, i2c_adapter->status,
              i2c_adapter->txflr, i2c_adapter->rxflr);

        /* clear hardware status */
        (void)i2c_adapter->clr_tx_abrt;
        (void)i2c_adapter->clr_stop_det;
        return -1;
    }

    LOG_D("i2c dma write ok: bus=%d len=%d restart=%d last=%d status=%x txflr=%d rxflr=%d\n",
          i2c_num, send_buf_len, need_restart, is_lastmsg, i2c_adapter->status,
          i2c_adapter->txflr, i2c_adapter->rxflr);
    return 0;
}

int i2c_recv_data(i2c_device_number_t i2c_num, uint8_t *receive_buf, 
                    size_t receive_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);

    size_t fifo_len, index;
    size_t rx_len = receive_buf_len;
    volatile i2c_t *i2c_adapter = i2c[i2c_num];

    /* receive data */
    int first_cmd = 1;
    while(receive_buf_len || rx_len) {
        fifo_len = i2c_adapter->rxflr;
        fifo_len = rx_len < fifo_len ? rx_len : fifo_len;

        for(index = 0; index < fifo_len; index++)
            *receive_buf++ = (uint8_t)i2c_adapter->data_cmd;

        rx_len -= fifo_len;

        fifo_len = 8 - i2c_adapter->txflr;
        fifo_len = receive_buf_len < fifo_len ? receive_buf_len : fifo_len;
        for(index = 0; index < fifo_len; index++) {
            uint32_t cmd = I2C_DATA_CMD_CMD;

            if(first_cmd && need_restart)
                cmd |= I2C_DATA_CMD_RESTART;

            if(is_lastmsg && receive_buf_len == 1)
                cmd |= I2C_DATA_CMD_STOP;

            i2c_adapter->data_cmd = cmd;

            first_cmd = 0;
            receive_buf_len--;
        }

        if(i2c_adapter->tx_abrt_source != 0)
            return -1;
    }

    LOG_D("i2c read ok: bus=%d len=%d restart=%d last=%d status=%x txflr=%d rxflr=%d\n",
          i2c_num, receive_buf_len, need_restart, is_lastmsg, i2c_adapter->status,
          i2c_adapter->txflr, i2c_adapter->rxflr);

    return 0;
}

int i2c_recv_data_dma(i2c_device_number_t i2c_num, dmac_channel_number_t chan_tx, dmac_channel_number_t chan_rx,
                       uint8_t *receive_buf, size_t receive_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);

    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    size_t i;
    uint32_t *write_cmd = kalloc_page();
    if(write_cmd == 0)
        return -1;

    /* clear hardware status */
    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;

    /* repeat start */
    write_cmd[0] = I2C_DATA_CMD_CMD;
    if (need_restart) {
        write_cmd[0] |= I2C_DATA_CMD_RESTART;
    }

    /* addr aligen and fill data */
    for(i = 1; i < receive_buf_len; i++)
        write_cmd[i] = I2C_DATA_CMD_CMD;

    /* stop */
    if (is_lastmsg)
        write_cmd[receive_buf_len - 1] = I2C_DATA_CMD_CMD | I2C_DATA_CMD_STOP;

    /* set up dma rx and tx */
    sysctl_dma_select((sysctl_dma_channel_t)chan_tx, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    sysctl_dma_select((sysctl_dma_channel_t)chan_rx, SYSCTL_DMA_SELECT_I2C0_RX_REQ + i2c_num * 2);

    dmac_set_single_mode(chan_rx, (void *)(&i2c_adapter->data_cmd), write_cmd, DMAC_ADDR_NOCHANGE,
                         DMAC_ADDR_INCREMENT, DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, receive_buf_len);
    /* chan tx I2C_DATA_CMD_CMD --> rx */
    dmac_set_single_mode(chan_tx, write_cmd, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT,
                         DMAC_ADDR_NOCHANGE, DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, receive_buf_len);

    /* waiting for dma rx and tx done */
    dmac_wait_done(chan_tx, DMAC_WAIT_TIMEOUT);
    LOG_D("i2c dma read tx done: status=%x txflr=%d rxflr=%d abrt=%x\n",
          i2c_adapter->status, i2c_adapter->txflr, i2c_adapter->rxflr,
          i2c_adapter->tx_abrt_source);

    dmac_wait_done(chan_rx, DMAC_WAIT_TIMEOUT);
    LOG_D("i2c dma read rx done: status=%x txflr=%d rxflr=%d abrt=%x\n",
          i2c_adapter->status, i2c_adapter->txflr, i2c_adapter->rxflr,
          i2c_adapter->tx_abrt_source);

    if (is_lastmsg && i2c_wait_done(i2c_num, i2c_adapter) < 0) {
        kfree_page((void *)write_cmd);
        return -1;
    }

    /* write data to receive buf */
    for(i = 0; i < receive_buf_len; i++)
    {
        receive_buf[i] = (uint8_t)write_cmd[i];
    }
    
    kfree_page((void *)write_cmd);

    if(i2c_adapter->tx_abrt_source != 0) {
        LOG_E("i2c dma read abort: bus=%d abrt=%x status=%x txflr=%d rxflr=%d\n",
              i2c_num, i2c_adapter->tx_abrt_source, i2c_adapter->status,
              i2c_adapter->txflr, i2c_adapter->rxflr);

        /* clear hardware status */
        (void)i2c_adapter->clr_tx_abrt;
        (void)i2c_adapter->clr_stop_det;
        return -1;
    }

    LOG_D("i2c dma read ok: bus=%d len=%d restart=%d last=%d status=%x txflr=%d rxflr=%d\n",
          i2c_num, receive_buf_len, need_restart, is_lastmsg, i2c_adapter->status,
          i2c_adapter->txflr, i2c_adapter->rxflr);
    return 0;
}

int i2c_transfer(i2c_device_number_t i2c_num, dmac_channel_number_t chan_tx, dmac_channel_number_t chan_rx, 
                    struct i2c_msg *msgs, int num) {
    int ret = 0;
    int i = 0;
    bool is_lastmsg = false;
    bool need_restart = false;

    for (i = 0; i < num; i ++) {
        /* last byte: stop */
        if (i == num - 1)
            is_lastmsg = true;
        
        /* repeat restart */
        if (i > 0) {
            need_restart = true;
        }

        /* read/write */
        if (msgs[i].flags & I2C_M_RD) {
            if (dma_enable && msgs[i].len >= DMA_THRESHOLD)
                ret = i2c_recv_data_dma(i2c_num, chan_tx, chan_rx, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
            else
                ret = i2c_recv_data(i2c_num, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
        } else {
            if (dma_enable && msgs[i].len >= DMA_THRESHOLD)
                ret = i2c_send_data_dma(i2c_num, chan_tx, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
            else
                ret = i2c_send_data(i2c_num, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
        }

        LOG_D("i2c_transfer: msg=%d ret=%d flags=%x restart=%d last=%d\n",
              i, ret, msgs[i].flags, need_restart, is_lastmsg);
        if(ret != 0)
            return ret;
    }

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
