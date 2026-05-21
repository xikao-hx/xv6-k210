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
#include "types.h"
#include "riscv.h"
#include "param.h"
#include "memlayout.h"
#include "fpioa.h"
#include "i2c.h"
#include "sysctl.h"
#include "kalloc.h"
#include "string.h"
#include "defs.h"
#include "oled_font.h"

volatile i2c_t *const i2c[3] =
    {
        (volatile i2c_t *)I2C0_V,
        (volatile i2c_t *)I2C1_V,
        (volatile i2c_t *)I2C2_V};

static void i2c_clk_init(i2c_device_number_t i2c_num)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    sysctl_clock_enable(SYSCTL_CLOCK_I2C0 + i2c_num);
    sysctl_clock_set_threshold(SYSCTL_THRESHOLD_I2C0 + i2c_num, 3);  // I2C_clk = PLL0 / 8 ≈ 100MHz
}

void i2c_init(i2c_device_number_t i2c_num, uint32_t slave_address, uint32_t address_width,
              uint32_t i2c_clk)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    // configASSERT(address_width == 7 || address_width == 10);

    volatile i2c_t *i2c_adapter = i2c[i2c_num];

    /* clock init */
    i2c_clk_init(i2c_num);

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
}

/* polling */
int i2c_send_data(i2c_device_number_t i2c_num, const uint8_t *send_buf, size_t send_buf_len)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    size_t fifo_len, index;

    /* clear send abort interrupt */
    i2c_adapter->clr_tx_abrt = i2c_adapter->clr_tx_abrt;

    /* send circle */
    while(send_buf_len)
    {
        fifo_len = 8 - i2c_adapter->txflr;
        /* write data to fifo */
        fifo_len = send_buf_len < fifo_len ? send_buf_len : fifo_len;
        for(index = 0; index < fifo_len; index++)
            i2c_adapter->data_cmd = I2C_DATA_CMD_DATA(*send_buf++);
       
        /* check transfer error */
        if(i2c_adapter->tx_abrt_source != 0)
            return 1;
        send_buf_len -= fifo_len;
    }
    /* waitting for transport finish */
    while((i2c_adapter->status & I2C_STATUS_ACTIVITY) || !(i2c_adapter->status & I2C_STATUS_TFE))
        ;

    /* last error check */
    if(i2c_adapter->tx_abrt_source != 0)
        return 1;

    /* Clear STOP_DET and provide bus-free time */
    i2c_adapter->clr_stop_det = i2c_adapter->clr_stop_det;
    for (volatile int i = 0; i < 200; i++);  // ~5µs delay

    return 0;
}

void i2c_send_data_dma(dmac_channel_number_t dma_channel_num, i2c_device_number_t i2c_num, const uint8_t *send_buf,
                       size_t send_buf_len)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);
    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    i2c_adapter->clr_tx_abrt = i2c_adapter->clr_tx_abrt;

    // uint32_t *buf = malloc(send_buf_len * sizeof(uint32_t));
    uint32_t *buf = kalloc();

    /* deal with addr aligen */
    int i;
    for(i = 0; i < send_buf_len; i++)
    {
        buf[i] = send_buf[i];
    }

    /* select dma and send date by dma */
    sysctl_dma_select((sysctl_dma_channel_t)dma_channel_num, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    dmac_set_single_mode(dma_channel_num, buf, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, send_buf_len);

    /* waiting for dma send done */
    dmac_wait_done(dma_channel_num);

    // free((void *)buf);
    kfree((void *)buf);

    /* waiting for i2c send done and check error */
    while((i2c_adapter->status & I2C_STATUS_ACTIVITY) || !(i2c_adapter->status & I2C_STATUS_TFE))
    {
        if(i2c_adapter->tx_abrt_source != 0)
            return;
    }
}

int i2c_recv_data(i2c_device_number_t i2c_num, const uint8_t *send_buf, size_t send_buf_len, uint8_t *receive_buf,
                  size_t receive_buf_len)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);

    size_t fifo_len, index;
    size_t rx_len = receive_buf_len;
    volatile i2c_t *i2c_adapter = i2c[i2c_num];

    /* send data */
    while(send_buf_len)
    {
        fifo_len = 8 - i2c_adapter->txflr;
        fifo_len = send_buf_len < fifo_len ? send_buf_len : fifo_len;
        for(index = 0; index < fifo_len; index++)
            i2c_adapter->data_cmd = I2C_DATA_CMD_DATA(*send_buf++);
        if(i2c_adapter->tx_abrt_source != 0)
            return 1;
        send_buf_len -= fifo_len;
    }

    /* receive data */
    while(receive_buf_len || rx_len)
    {
        fifo_len = i2c_adapter->rxflr;
        fifo_len = rx_len < fifo_len ? rx_len : fifo_len;
        for(index = 0; index < fifo_len; index++)
            *receive_buf++ = (uint8_t)i2c_adapter->data_cmd;
        rx_len -= fifo_len;
        fifo_len = 8 - i2c_adapter->txflr;
        fifo_len = receive_buf_len < fifo_len ? receive_buf_len : fifo_len;
        for(index = 0; index < fifo_len; index++)
            i2c_adapter->data_cmd = I2C_DATA_CMD_CMD;
        if(i2c_adapter->tx_abrt_source != 0)
            return 1;
        receive_buf_len -= fifo_len;
    }
    return 0;
}

void i2c_recv_data_dma(dmac_channel_number_t dma_send_channel_num, dmac_channel_number_t dma_receive_channel_num,
                       i2c_device_number_t i2c_num, const uint8_t *send_buf, size_t send_buf_len,
                       uint8_t *receive_buf, size_t receive_buf_len)
{
    // configASSERT(i2c_num < I2C_MAX_NUM);

    volatile i2c_t *i2c_adapter = i2c[i2c_num];
    // uint32_t *write_cmd = malloc(sizeof(uint32_t) * (send_buf_len + receive_buf_len));
    uint32_t *write_cmd = kalloc();

    /* addr aligen and fill data */
    size_t i;
    for(i = 0; i < send_buf_len; i++)
        write_cmd[i] = *send_buf++;
    for(i = 0; i < receive_buf_len; i++)
        write_cmd[i + send_buf_len] = I2C_DATA_CMD_CMD;

    /* set up dma rx and tx */
    sysctl_dma_select((sysctl_dma_channel_t)dma_send_channel_num, SYSCTL_DMA_SELECT_I2C0_TX_REQ + i2c_num * 2);
    sysctl_dma_select((sysctl_dma_channel_t)dma_receive_channel_num, SYSCTL_DMA_SELECT_I2C0_RX_REQ + i2c_num * 2);

    dmac_set_single_mode(dma_receive_channel_num, (void *)(&i2c_adapter->data_cmd), write_cmd, DMAC_ADDR_NOCHANGE,
                         DMAC_ADDR_INCREMENT, DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, receive_buf_len);

    dmac_set_single_mode(dma_send_channel_num, write_cmd, (void *)(&i2c_adapter->data_cmd), DMAC_ADDR_INCREMENT,
                         DMAC_ADDR_NOCHANGE, DMAC_MSIZE_4, DMAC_TRANS_WIDTH_32, receive_buf_len + send_buf_len);

    /* waiting for dma rx and tx done */
    dmac_wait_done(dma_send_channel_num);
    dmac_wait_done(dma_receive_channel_num);

    /* write data to receive buf */
    for(i = 0; i < receive_buf_len; i++)
    {
        receive_buf[i] = (uint8_t)write_cmd[i];
    }
    
    // free(write_cmd);
    kfree((void *)write_cmd);
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
