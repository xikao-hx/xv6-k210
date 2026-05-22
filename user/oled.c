// OLED (SSD1306 128x64) user-space driver via I2C device.

#include "types.h"
#include "i2cdev.h"
#include "oled.h"
#include "oled_font.h"
#include "user/user.h"
#include <stdarg.h>

int vsnprintf(char *buf, int n, const char *fmt, va_list ap);

#define OLED_ADDR       0x3C
#define OLED_CTRL_CMD   0x00
#define OLED_CTRL_DAT   0x40

static int oled_fd = -1;

static void
oled_write_cmd(uint8 cmd)
{
  uint8 buf[2] = {OLED_CTRL_CMD, cmd};
  struct i2c_msg msg;
  struct i2c_transfer xfer;
  msg.addr = OLED_ADDR;
  msg.flags = 0;
  msg.len = 2;
  msg.buf = (uint64)buf;
  xfer.nmsgs = 1;
  xfer.msgs[0] = msg;
  ioctl(oled_fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

static void
oled_write_data(uint8 dat)
{
  uint8 buf[2] = {OLED_CTRL_DAT, dat};
  struct i2c_msg msg;
  struct i2c_transfer xfer;
  msg.addr = OLED_ADDR;
  msg.flags = 0;
  msg.len = 2;
  msg.buf = (uint64)buf;
  xfer.nmsgs = 1;
  xfer.msgs[0] = msg;
  ioctl(oled_fd, I2C_IOCTL_TRANSFER, (uint64)&xfer);
}

static void
oled_set_cursor(uint8 page, uint8 col)
{
  oled_write_cmd(0xB0 | page);
  oled_write_cmd(0x10 | ((col >> 4) & 0x0F));
  oled_write_cmd(0x00 | (col & 0x0F));
}

int
oled_init(void)
{
  oled_fd = dev(0, I2C_DEV_MAJOR, 0);   // I2C0
  if (oled_fd < 0)
    return -1;

  struct i2cdev_init cfg;
  cfg.clk_rate = 100000;
  cfg.slave_addr = OLED_ADDR;
  if (ioctl(oled_fd, I2C_IOCTL_INIT, (uint64)&cfg) < 0) {
    oled_fd = -1;
    return -1;
  }

  // Init sequence
  sleep(1);                       // power-up delay
  oled_write_cmd(0xAE);       // display off

  oled_write_cmd(0xD5); oled_write_cmd(0x80);  // clock div
  oled_write_cmd(0xA8); oled_write_cmd(0x3F);  // mux ratio
  oled_write_cmd(0xD3); oled_write_cmd(0x00);  // display offset
  oled_write_cmd(0x40);       // start line = 0

  oled_write_cmd(0xA1);       // segment remap
  oled_write_cmd(0xC8);       // COM scan remap

  oled_write_cmd(0xDA); oled_write_cmd(0x12);  // COM pins
  oled_write_cmd(0x81); oled_write_cmd(0xCF);  // contrast
  oled_write_cmd(0xD9); oled_write_cmd(0xF1);  // pre-charge
  oled_write_cmd(0xDB); oled_write_cmd(0x30);  // VCOMH

  oled_write_cmd(0xA4);       // entire display follow RAM
  oled_write_cmd(0xA6);       // normal display

  oled_write_cmd(0x8D); oled_write_cmd(0x14);  // charge pump enable
  oled_write_cmd(0xAF);       // display on

  oled_clear();
  return 0;
}

int
oled_ready(void)
{
  return oled_fd >= 0;
}

void
oled_clear(void)
{
  if (!oled_ready())
    return;

  for (uint8 page = 0; page < 8; page++) {
    oled_set_cursor(page, 0);
    for (uint16 i = 0; i < 128; i++)
      oled_write_data(0x00);
  }
}

void
oled_show_char(uint8 row, uint8 col, char ch)
{
  if (!oled_ready() || row >= 4 || col >= 16)
    return;

  if (ch < ' ' || ch > '~')
    ch = ' ';
  uint8 idx = ch - ' ';

  oled_set_cursor(row * 2, col * 8);
  for (uint8 i = 0; i < 8; i++)
    oled_write_data(OLED_F8x16[idx][i]);

  oled_set_cursor(row * 2 + 1, col * 8);
  for (uint8 i = 0; i < 8; i++)
    oled_write_data(OLED_F8x16[idx][i + 8]);
}

void
oled_show_string(uint8 row, uint8 col, const char *str)
{
  for (uint8 i = 0; str[i] != '\0' && col + i < 16; i++)
    oled_show_char(row, col + i, str[i]);
}

static uint32
oled_pow(uint32 x, uint8 y)
{
  uint32 result = 1;
  while (y--)
    result *= x;
  return result;
}

void
oled_show_num(uint8 row, uint8 col, uint32 num, uint8 len)
{
  if (len > 10)
    len = 10;
  for (uint8 i = 0; i < len && col + i < 16; i++) {
    uint8 digit = (num / oled_pow(10, len - i - 1)) % 10;
    oled_show_char(row, col + i, digit + '0');
  }
}

void
oled_show_signed_num(uint8 row, uint8 col, int num, uint8 len)
{
  uint32 mag;

  if (num >= 0) {
    oled_show_char(row, col, '+');
    mag = num;
  } else {
    oled_show_char(row, col, '-');
    mag = (uint32)(-(num + 1)) + 1;
  }

  oled_show_num(row, col + 1, mag, len);
}

void
oled_show_hex_num(uint8 row, uint8 col, uint32 num, uint8 len)
{
  static const char hex[] = "0123456789ABCDEF";

  if (len > 8)
    len = 8;
  for (uint8 i = 0; i < len && col + i < 16; i++) {
    uint8 digit = (num >> ((len - i - 1) * 4)) & 0xF;
    oled_show_char(row, col + i, hex[digit]);
  }
}

void
oled_show_bin_num(uint8 row, uint8 col, uint32 num, uint8 len)
{
  if (len > 32)
    len = 32;
  for (uint8 i = 0; i < len && col + i < 16; i++) {
    uint8 bit = (num >> (len - i - 1)) & 1;
    oled_show_char(row, col + i, bit + '0');
  }
}

void
oled_show_hex32(uint8 row, uint8 col, uint32 val)
{
  oled_show_hex_num(row, col, val, 8);
}

void
oled_printf(uint8 row, uint8 col, const char *fmt, ...)
{
  char buf[17];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  oled_show_string(row, col, buf);
}

void
oled_write_row(uint8 row, const char *str)
{
  if (!oled_ready() || row >= 4)
    return;

  // Clear the row first
  for (uint8 page = row * 2; page < row * 2 + 2; page++) {
    oled_set_cursor(page, 0);
    for (uint16 i = 0; i < 128; i++)
      oled_write_data(0x00);
  }
  oled_show_string(row, 0, str);
}

void
oled_write_hexrow(uint8 row, const char *label, const uint8 *data, int n)
{
  char buf[17];
  int i = 0;
  int pos = 0;

  while (label && label[i] && pos < 16)
    buf[pos++] = label[i++];

  for (int j = 0; data && j < n && pos + 1 < 16; j++) {
    static const char hex[] = "0123456789ABCDEF";
    buf[pos++] = hex[(data[j] >> 4) & 0xF];
    buf[pos++] = hex[data[j] & 0xF];
  }

  while (pos < 16)
    buf[pos++] = ' ';
  buf[16] = 0;

  oled_write_row(row, buf);
}
