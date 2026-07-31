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
#include "kalloc.h"
#include "log.h"
#include "sysctl.h"
#include "stdbool.h"
#include "i2c_board.h"

#define I2C_WAIT_TIMEOUT  1000000UL
#define DMAC_WAIT_TIMEOUT 10000UL
#define DMA_THRESHOLD 16

volatile i2c_t *const i2c[3] = {
    (volatile i2c_t *)I2C0_V,
    (volatile i2c_t *)I2C1_V,
    (volatile i2c_t *)I2C2_V
};

static int i2c_wait_done(i2c_device_number_t i2c_num, volatile i2c_t *i2c_adapter) {
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

    /* calculation divider value */
    // NOTE: Both sysctl_clock_get_freq(I2C0) and sysctl_clock_get_threshold()
    // return wrong values on real K210 (bitfield register reads are broken).
    // Derive I2C clock from CPU frequency with the known threshold we set above:
    uint32_t v_i2c_freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU) / 4;
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
    i2c_ctrl->i2c_data.dma_enable = true;
    i2c_ctrl->i2c_data.index = i2c_num;
}

void i2c_init(void) {

    for (int i = 0; i < I2C_DEVICE_MAX; i++) {
        if(i2c_ctrls[i] == 0)
            continue;
            
        i2c_dw_init(i);
        char name[10];
        snprintf(name, sizeof(name), "i2c_%d", i);
        initsleeplock(&i2c_ctrls[i]->lock, name);
    }
}

/* polling */
int i2c_send_data(struct i2c_dw_data *i2c_data, const uint8_t *send_buf, 
                    size_t send_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    int ret = 0;
    i2c_device_number_t i2c_num = i2c_data->index;
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
       
        ret = i2c_wait_done(i2c_num, i2c_adapter);
        if (ret) {
            break;
        }
    }

    return ret;
}

int i2c_send_data_dma(struct i2c_dw_data *i2c_data, const uint8_t *send_buf, 
                    size_t send_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    int ret = 0;
    i2c_device_number_t i2c_num = i2c_data->index;
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    int i;

    uint32_t *buf = kalloc_page();
    if(buf == 0)
        return -1;

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
    sysctl_dma_select((sysctl_dma_channel_t)i2c_data->chan_tx, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    dmac_set_single_mode(i2c_data->chan_tx, buf, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, send_buf_len);

    /* waiting for dma send done */
    if(dmac_wait_done(i2c_data->chan_tx, DMAC_WAIT_TIMEOUT) < 0) {
        LOG_E("i2c dma write tx timeout: bus=%d\n", i2c_num);
        kfree_page((void *)buf);
        ret = -1;
        goto error;
    }

    kfree_page((void *)buf);

    if (i2c_wait_done(i2c_num, i2c_adapter) < 0)
        return -1;

    if(i2c_adapter->tx_abrt_source != 0) {
        LOG_E("i2c dma write abort done: bus=%d abrt=%x status=%x txflr=%d rxflr=%d\n",
                i2c_num, i2c_adapter->tx_abrt_source, i2c_adapter->status,
                i2c_adapter->txflr, i2c_adapter->rxflr);
        ret = -1;
    }

error:    
    /* clear hardware status */
    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;

    return ret;
}

int i2c_recv_data(struct i2c_dw_data *i2c_data, uint8_t *receive_buf, 
                    size_t receive_buf_len, bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    int ret = 0;
    uint32_t i2c_num = i2c_data->index;
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

        ret = i2c_wait_done(i2c_num, i2c_adapter);
        if (ret)
            break;
    }

    return 0;
}

int i2c_recv_data_dma(struct i2c_dw_data *i2c_data, uint8_t *receive_buf, size_t receive_buf_len, 
                    bool need_restart, bool is_lastmsg)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    int ret = 0;
    i2c_device_number_t i2c_num = i2c_data->index;
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    size_t i;
    uint32_t *write_cmd = kalloc_page();
    if(write_cmd == 0)
        return -1;

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
    sysctl_dma_select((sysctl_dma_channel_t)i2c_data->chan_tx, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    sysctl_dma_select((sysctl_dma_channel_t)i2c_data->chan_rx, SYSCTL_DMA_SELECT_I2C0_RX_REQ + i2c_num * 2);

    dmac_set_single_mode(i2c_data->chan_rx, (void *)(&i2c_adapter->data_cmd), write_cmd, DMAC_ADDR_NOCHANGE,
                         DMAC_ADDR_INCREMENT, DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, receive_buf_len);
    /* chan tx I2C_DATA_CMD_CMD --> rx */
    dmac_set_single_mode(i2c_data->chan_tx, write_cmd, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT,
                         DMAC_ADDR_NOCHANGE, DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, receive_buf_len);

    /* waiting for dma rx and tx done */
    if(dmac_wait_done(i2c_data->chan_tx, DMAC_WAIT_TIMEOUT) < 0) {
        LOG_E("i2c dma read tx timeout: bus=%d\n", i2c_num);
        kfree_page((void *)write_cmd);
        ret = -1;
        goto error;
    }

    if(dmac_wait_done(i2c_data->chan_rx, DMAC_WAIT_TIMEOUT) < 0) {
        LOG_E("i2c dma read rx timeout: bus=%d\n", i2c_num);
        kfree_page((void *)write_cmd);
        ret = -1;
        goto error;
    }

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
        ret = -1;
    }

error:
    /* clear hardware status */
    (void)i2c_adapter->clr_tx_abrt;
    (void)i2c_adapter->clr_stop_det;

    return ret;
}

int i2c_transfer(struct i2c_device *dev, struct i2c_msg *msgs, int num) {

    int ret = 0;
    int i = 0;
    bool is_lastmsg = false;
    bool need_restart = false;

    /* Entry validation */
    if(dev == 0 || msgs == 0 || num <= 0)
        return -1;

    i2c_device_number_t bus_num = dev->bus_num;
    if(bus_num >= I2C_DEVICE_MAX)
        return -1;
    /* Only 7-bit addresses are currently supported */
    if(dev->address_width != 7)
        return -1;

    struct i2c_controller *i2c_ctrl = i2c_ctrls[bus_num];
    if(i2c_ctrl == 0)
        return -1;

    struct i2c_dw_data *i2c_data = &i2c_ctrl->i2c_data;

    acquiresleep(&i2c_ctrl->lock);
    for (i = 0; i < num; i ++) {

        /* slave addr */
        i2c_write_slave_addr(bus_num, msgs[i].addr);
        /* last byte: stop */
        if (i == num - 1) is_lastmsg = true;
        /* repeat restart */
        if (i > 0) need_restart = true;

        /* read/write */
        if (msgs[i].flags & I2C_M_RD) {
            if (i2c_data->dma_enable && msgs[i].len >= DMA_THRESHOLD)
                ret = i2c_recv_data_dma(i2c_data, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
            else
                ret = i2c_recv_data(i2c_data, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
        } else {
            if (i2c_data->dma_enable && msgs[i].len >= DMA_THRESHOLD)
                ret = i2c_send_data_dma(i2c_data, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
            else
                ret = i2c_send_data(i2c_data, msgs[i].buf, msgs[i].len, need_restart, is_lastmsg);
        }

        LOG_D("i2c_transfer: msg=%d ret=%d flags=%x restart=%d last=%d\n",
              i, ret, msgs[i].flags, need_restart, is_lastmsg);
        if(ret != 0)
            break;
    }
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
