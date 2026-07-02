#include "file.h"
#include "i2c.h"
#include "i2cdev.h"
#include "kalloc.h"
#include "proc.h"
#include "vm.h"

/* Last configured clock rate per I2C bus (saved from I2C_IOCTL_INIT) */
static uint32 saved_clk_rate[I2C_DEVICE_MAX];

static int
i2cdev_read(int user_dst, uint64 dst, int n)
{
  return -1;
}

static int
i2cdev_write(int user_src, uint64 src, int n)
{
  return -1;
}

static int
i2cdev_ioctl(int minor, uint64 cmd, uint64 arg)
{
  struct proc *p = myproc();
  i2c_device_number_t i2c_bus = I2C_BUS(minor);

  switch(cmd) {
  case I2C_IOCTL_INIT: {
    struct i2cdev_init cfg;
    if(copyin(p->pagetable, (char *)&cfg, arg, sizeof(cfg)) < 0)
      return -1;
    saved_clk_rate[i2c_bus] = cfg.clk_rate;
    i2c_init(i2c_bus, cfg.slave_addr, 7, cfg.clk_rate);
    return 0;
  }

  case I2C_IOCTL_TRANSFER: {
    struct i2c_rdwr_ioctl_data rdwr_arg;
    if(copyin(p->pagetable, (char *)&rdwr_arg, arg, sizeof(rdwr_arg)) < 0)
      return -1;
    if(rdwr_arg.nmsgs == 0 || rdwr_arg.nmsgs > I2C_MAX_MSGS)
      return -1;
    
    /* copy msgs */
    struct i2c_msg rdwr_pa[I2C_MAX_MSGS];
    for (int i = 0; i < rdwr_arg.nmsgs; i ++) {
      if (copyin(p->pagetable, (char *)&rdwr_pa[i], (uint64)(rdwr_arg.msgs + i), sizeof(struct i2c_msg)) < 0) 
        return -1;
    }

    /* Re-init with the target address (preserve clock rate from INIT) */
    uint32 clk = saved_clk_rate[i2c_bus];
    if(clk == 0) clk = 50000;
    i2c_init(i2c_bus, rdwr_pa[0].addr, 7, clk);

    if(rdwr_arg.nmsgs == 1) {
      /* Single message: pure read or pure write */
      struct i2c_msg *msg = &rdwr_pa[0];
      if(msg->flags & I2C_M_RD) {
        /* Pure read */
        if(msg->len == 0) return 0;
        uint32 len = msg->len;
        if(len > 4096) len = 4096;
        uint8 *buf = kalloc();
        if(buf == 0) return -1;
        int ret = i2c_recv_data(i2c_bus, 0, 0, buf, len);
        if(ret == 0 && msg->buf != 0) {
          if(copyout(p->pagetable, (uint64)msg->buf, (char *)buf, len) < 0)
            ret = -1;
        }
        kfree(buf);
        if(ret != 0) ret = -1;
        return ret;
      } else {
        /* Pure write */
        if(msg->len == 0) return 0;
        uint32 len = msg->len;
        if(len > 4096) len = 4096;
        uint8 *buf = kalloc();
        if(buf == 0) return -1;
        if(copyin(p->pagetable, (char *)buf, (uint64)msg->buf, len) < 0) {
          kfree(buf);
          return -1;
        }
        int ret = i2c_send_data(i2c_bus, buf, len);
        kfree(buf);
        if(ret != 0) ret = -1;
        return ret;
      }
    } else {
      /* Combined transaction: msg[0] = write, msg[1] = read with RESTART */
      struct i2c_msg *w = &rdwr_pa[0];
      struct i2c_msg *r = &rdwr_pa[1];
      if(w->flags & I2C_M_RD) return -1;  /* first must be write */
      if(!(r->flags & I2C_M_RD)) return -1; /* second must be read */
      uint32 wlen = w->len;
      if(wlen > 4096) wlen = 4096;
      uint32 rlen = r->len;
      if(rlen > 4096) rlen = 4096;

      uint8 *wbuf = kalloc();
      if(wbuf == 0) return -1;
      if(wlen > 0) {
        if(copyin(p->pagetable, (char *)wbuf, w->buf, wlen) < 0) {
          kfree(wbuf);
          return -1;
        }
      }

      uint8 *rbuf = kalloc();
      if(rbuf == 0) {
        kfree(wbuf);
        return -1;
      }

      int ret = i2c_recv_data(i2c_bus, wbuf, wlen, rbuf, rlen);
      if(ret == 0 && rlen > 0 && r->buf != 0) {
        if(copyout(p->pagetable, r->buf, (char *)rbuf, rlen) < 0)
          ret = -1;
      }
      kfree(wbuf);
      kfree(rbuf);
      if(ret != 0) ret = -1;
      return ret;
    }
  }

  default:
    return -1;
  }
}

void
i2cdev_init(void)
{
  devsw[DEV_I2C].read = i2cdev_read;
  devsw[DEV_I2C].write = i2cdev_write;
  devsw[DEV_I2C].ioctl = i2cdev_ioctl;
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
void test_mpu6050(void) {

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
/* OLED (128x64) test driver                                             */
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

void test_oled(void) {

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
    sw_oled_write_cmd(0xD5); 
    sw_oled_write_cmd(0x80);
    sw_oled_write_cmd(0xA8); 
    sw_oled_write_cmd(0x3F);
    sw_oled_write_cmd(0xD3); 
    sw_oled_write_cmd(0x00);
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

void test_sw_i2c(void) {

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
