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
#include "gpiohs.h"
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

    /* Clear any pending abort from previous transaction */
    i2c_adapter->clr_tx_abrt = i2c_adapter->clr_tx_abrt;

    /* Disable controller and wait for confirmation (DW APB I2C handshake) */
    i2c_adapter->enable = 0;
    for (int i = 0; i < 1000; i++) {
        if ((i2c_adapter->enable_status & I2C_ENABLE_STATUS_IC_ENABLE) == 0)
            break;
    }

    i2c_adapter->con = I2C_CON_MASTER_MODE | I2C_CON_SLAVE_DISABLE | I2C_CON_RESTART_EN |
                       (address_width == 10 ? I2C_CON_10BITADDR_SLAVE : 0) | I2C_CON_SPEED(0);
    i2c_adapter->ss_scl_hcnt = I2C_SS_SCL_HCNT_COUNT(v_period_clk_cnt);  // scl high/low level count
    i2c_adapter->ss_scl_lcnt = I2C_SS_SCL_LCNT_COUNT(v_period_clk_cnt);

    i2c_adapter->tar = I2C_TAR_ADDRESS(slave_address);
    i2c_adapter->intr_mask = 0;   // forbid all I2C interrupt

    /* enable DMA handshake (required for data_cmd FIFO access on K210) */
    i2c_adapter->dma_cr = 0x3;
    i2c_adapter->dma_rdlr = 0;
    i2c_adapter->dma_tdlr = 4;
    i2c_adapter->enable = I2C_ENABLE_ENABLE;
    for (int i = 0; i < 1000; i++) {
        if ((i2c_adapter->enable_status & I2C_ENABLE_STATUS_IC_ENABLE) != 0)
            break;
    }

    /* Set SDA hold time to prevent bit errors */
    i2c_adapter->sda_hold = I2C_SDA_HOLD_TX(v_period_clk_cnt / 4) | I2C_SDA_HOLD_RX(v_period_clk_cnt / 8);
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
        {
            /* Wait for bus idle before returning so i2c_init can disable cleanly */
            while((i2c_adapter->status & I2C_STATUS_ACTIVITY) || !(i2c_adapter->status & I2C_STATUS_TFE))
                ;
            return 1;
        }
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

    /* clear any pending abort from a previous transaction */
    i2c_adapter->clr_tx_abrt = i2c_adapter->clr_tx_abrt;

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

    /* If there are no bytes to receive, we are done */
    if(receive_buf_len == 0)
        return 0;

    /* Wait for the TX FIFO to empty and then start reading.
     * For combined transactions (write + read with RESTART), the read
     * command must be queued while the write is still in-flight so the
     * controller does not issue STOP prematurely.  When called with
     * send_buf_len == 0 (separate read transaction), the TX FIFO is
     * already empty and a new START is generated. */
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

#ifdef TEST
/* busy-wait delay using the MTIME counter */
void Delay_ms(uint32_t ms)
{
    uint64_t target = r_time() + (uint64_t)ms * (INTERVAL / 5);
    while(r_time() < target);
}

/* --------------------------------------------------------------------- */
/* MPU6050 test driver            */
/* --------------------------------------------------------------------- */
#define MPU6050_ADDRESS         0x68
#define MPU6050_SMPLRT_DIV      0x19
#define MPU6050_CONFIG          0x1A
#define MPU6050_GYRO_CONFIG     0x1B
#define MPU6050_ACCEL_CONFIG    0x1C

#define MPU6050_ACCEL_XOUT_H    0x3B
#define MPU6050_ACCEL_XOUT_L    0x3C
#define MPU6050_ACCEL_YOUT_H    0x3D
#define MPU6050_ACCEL_YOUT_L    0x3E
#define MPU6050_ACCEL_ZOUT_H    0x3F
#define MPU6050_ACCEL_ZOUT_L    0x40
#define MPU6050_TEMP_OUT_H      0x41
#define MPU6050_TEMP_OUT_L      0x42
#define MPU6050_GYRO_XOUT_H     0x43
#define MPU6050_GYRO_XOUT_L     0x44
#define MPU6050_GYRO_YOUT_H     0x45
#define MPU6050_GYRO_YOUT_L     0x46
#define MPU6050_GYRO_ZOUT_H     0x47
#define MPU6050_GYRO_ZOUT_L     0x48

#define MPU6050_PWR_MGMT_1      0x6B
#define MPU6050_PWR_MGMT_2      0x6C
#define MPU6050_WHO_AM_I        0x75

void MPU6050_WriteReg(uint8 RegAddress, uint8 Data) {
    uint8 tx_buf[2] = {RegAddress, Data};

    i2c_send_data(I2C_DEVICE_0, tx_buf, sizeof(tx_buf));
}

uint8 MPU6050_ReadReg(uint8_t RegAddress) {
    uint8 tx_buf[1] = {RegAddress};
    uint8 rx_buf[1];

    i2c_recv_data(I2C_DEVICE_0, tx_buf, sizeof(tx_buf), rx_buf, sizeof(rx_buf));

    return rx_buf[0];
}

// A simple test for i2c read/write test
void test_i2c(void) {

    i2c_init(I2C_DEVICE_0, MPU6050_ADDRESS, 7, 50000);

    MPU6050_WriteReg(MPU6050_PWR_MGMT_1, 0x01);            
    Delay_ms(100);
    MPU6050_WriteReg(MPU6050_PWR_MGMT_2, 0x00);            
    MPU6050_WriteReg(MPU6050_SMPLRT_DIV, 0x09);            
    MPU6050_WriteReg(MPU6050_CONFIG, 0x06);                
    MPU6050_WriteReg(MPU6050_GYRO_CONFIG, 0x18);            
    MPU6050_WriteReg(MPU6050_ACCEL_CONFIG, 0x18);          
   
    uint8 data = MPU6050_ReadReg(MPU6050_WHO_AM_I);
    printf("ID: %x\n", data);
}

/* --------------------------------------------------------------------- */
/* OLED (128×64) test driver
/* --------------------------------------------------------------------- */
#define OLED_ADDR       0x3C
#define OLED_CTRL_CMD   0x00
#define OLED_CTRL_DAT   0x40

static void oled_write_cmd(uint8_t cmd) {
    uint8_t buf[2] = {OLED_CTRL_CMD, cmd};
    i2c_send_data(I2C_DEVICE_0, buf, 2);
}

static void oled_write_data(uint8_t dat) {
    uint8_t buf[2] = {OLED_CTRL_DAT, dat};
    i2c_send_data(I2C_DEVICE_0, buf, 2);
}

static void oled_init(void) {
    Delay_ms(100);                     /* power-up delay */

    oled_write_cmd(0xAE);              /* display off */

    oled_write_cmd(0xD5);              /* clock div / oscillator freq */
    oled_write_cmd(0x80);
    oled_write_cmd(0xA8);              /* multiplex ratio */
    oled_write_cmd(0x3F);              /* 64 lines */
    oled_write_cmd(0xD3);              /* display offset */
    oled_write_cmd(0x00);
    oled_write_cmd(0x40);              /* start line = 0 */

    oled_write_cmd(0xA1);              /* segment remap (column 127 = SEG0) */
    oled_write_cmd(0xC8);              /* COM scan direction (remapped) */

    oled_write_cmd(0xDA);              /* COM pins hw config */
    oled_write_cmd(0x12);
    oled_write_cmd(0x81);              /* contrast */
    oled_write_cmd(0xCF);
    oled_write_cmd(0xD9);              /* pre-charge */
    oled_write_cmd(0xF1);
    oled_write_cmd(0xDB);              /* VCOMH deselect level */
    oled_write_cmd(0x30);

    oled_write_cmd(0xA4);              /* entire display on (follow RAM) */
    oled_write_cmd(0xA6);              /* normal (non-inverted) display */

    oled_write_cmd(0x8D);              /* charge pump */
    oled_write_cmd(0x14);              /* enable */

    oled_write_cmd(0xAF);              /* display on */
}

static void oled_set_cursor(uint8_t row, uint8_t col) {
    /* row = 0..7 (8 pages × 8 pixels), col = 0..127 */
    oled_write_cmd(0xB0 | row);
    oled_write_cmd(0x10 | ((col >> 4) & 0x0F));
    oled_write_cmd(0x00 | (col & 0x0F));
}

static void oled_clear(void) {
    for (uint8_t page = 0; page < 8; page++) {
        oled_set_cursor(page, 0);
        for (uint16_t i = 0; i < 128; i++)
            oled_write_data(0x00);
    }
}

static void oled_show_char(uint8_t row, uint8_t col, char ch) {
    if (ch < ' ' || ch > '~')
        ch = ' ';
    uint8_t idx = ch - ' ';
    /* upper half (8 bytes) */
    oled_set_cursor(row * 2, col * 8);
    for (uint8_t i = 0; i < 8; i++)
        oled_write_data(OLED_F8x16[idx][i]);
    /* lower half (8 bytes) */
    oled_set_cursor(row * 2 + 1, col * 8);
    for (uint8_t i = 0; i < 8; i++)
        oled_write_data(OLED_F8x16[idx][i + 8]);
}

static void oled_show_string(uint8_t row, uint8_t col, const char *str) {
    for (uint8_t i = 0; str[i] != '\0' && col + i < 16; i++)
        oled_show_char(row, col + i, str[i]);
}

void test_i2c(void) {

    /* OLED init */
    i2c_init(I2C_DEVICE_0, OLED_ADDR, 7, 100000);
    oled_init();
    oled_clear();
    printf("OLED: displaying text...\n");

    oled_show_string(0, 0, "Hello xv6!");
    oled_show_string(1, 0, "I2C on K210");
    oled_show_string(2, 0, "SSD1306 OLED");
    oled_show_string(3, 0, "OK! 0123456789");

    printf("OLED test done\n");
}
#endif

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

static void sw_i2c_init(void)
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

static void sw_i2c_start(void)
{
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_LOW);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_LOW);
    sw_i2c_delay();
}

static void sw_i2c_stop(void)
{
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_LOW);
    gpiohs_set_pin(SW_I2C_SCL_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
    gpiohs_set_pin(SW_I2C_SDA_GPIO, GPIO_PV_HIGH);
    sw_i2c_delay();
}

static void sw_i2c_send_byte(uint8_t byte)
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

/* Software I2C OLED helpers */
static void sw_oled_write_cmd(uint8_t cmd)
{
    sw_i2c_start();
    sw_i2c_send_byte(OLED_ADDR << 1);   /* 0x78 = write */
    sw_i2c_send_byte(OLED_CTRL_CMD);    /* 0x00 = command */
    sw_i2c_send_byte(cmd);
    sw_i2c_stop();
}

static void sw_oled_write_data(uint8_t dat)
{
    sw_i2c_start();
    sw_i2c_send_byte(OLED_ADDR << 1);   /* 0x78 = write */
    sw_i2c_send_byte(OLED_CTRL_DAT);    /* 0x40 = data */
    sw_i2c_send_byte(dat);
    sw_i2c_stop();
}

static void sw_oled_set_cursor(uint8_t row, uint8_t col) {
    sw_oled_write_cmd(0xB0 | row);
    sw_oled_write_cmd(0x10 | ((col >> 4) & 0x0F));
    sw_oled_write_cmd(0x00 | (col & 0x0F));
}

static void sw_oled_clear(void) {
    for (uint8_t page = 0; page < 8; page++) {
        sw_oled_set_cursor(page, 0);
        for (uint16_t i = 0; i < 128; i++)
            sw_oled_write_data(0x00);
    }
}

static void sw_oled_show_char(uint8_t row, uint8_t col, char ch) {
    if (ch < ' ' || ch > '~') ch = ' ';
    uint8_t idx = ch - ' ';
    sw_oled_set_cursor(row * 2, col * 8);
    for (uint8_t i = 0; i < 8; i++)
        sw_oled_write_data(OLED_F8x16[idx][i]);
    sw_oled_set_cursor(row * 2 + 1, col * 8);
    for (uint8_t i = 0; i < 8; i++)
        sw_oled_write_data(OLED_F8x16[idx][i + 8]);
}

static void sw_oled_show_string(uint8_t row, uint8_t col, const char *str) {
    for (uint8_t i = 0; str[i] != '\0' && col + i < 16; i++)
        sw_oled_show_char(row, col + i, str[i]);
}

static void sw_oled_init(void) {
    Delay_ms(100);
    sw_oled_write_cmd(0xAE);
    sw_oled_write_cmd(0xD5); sw_oled_write_cmd(0x80);
    sw_oled_write_cmd(0xA8); sw_oled_write_cmd(0x3F);
    sw_oled_write_cmd(0xD3); sw_oled_write_cmd(0x00);
    sw_oled_write_cmd(0x40);
    sw_oled_write_cmd(0xA1);
    sw_oled_write_cmd(0xC8);
    sw_oled_write_cmd(0xDA); 
    sw_oled_write_cmd(0x12);
    sw_oled_write_cmd(0x81); 
    sw_oled_write_cmd(0xCF);
    sw_oled_write_cmd(0xD9); 
    sw_oled_write_cmd(0xF1);
    sw_oled_write_cmd(0xDB); 
    sw_oled_write_cmd(0x30);
    sw_oled_write_cmd(0xA4);
    sw_oled_write_cmd(0xA6);
    sw_oled_write_cmd(0x8D); 
    sw_oled_write_cmd(0x14);
    sw_oled_write_cmd(0xAF);
}

void test_i2c(void) {

    /* ---- Software I2C OLED test ---- */
    printf("\n--- SW I2C OLED test ---\n");
    sw_i2c_init();
    Delay_ms(10);
    sw_oled_init();
    sw_oled_clear();
    printf("SW OLED: displaying text...\n");
    sw_oled_show_string(0, 0, "Hello sw!");
    sw_oled_show_string(2, 0, "SW I2C on K210");
    sw_oled_show_string(4, 0, "SSD1306 OLED");
    sw_oled_show_string(6, 0, "SW I2C OK!");
    printf("SW OLED test done\n");
}

#endif
