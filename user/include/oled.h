#ifndef __USER_OLED_H
#define __USER_OLED_H

#include "types.h"

// OLED display functions for user space
// Uses I2C device interface (/dev/i2c-0)

int  oled_init(void);                           // init I2C & OLED, returns 0 or -1
int  oled_ready(void);
void oled_clear(void);
void oled_show_char(uint8 row, uint8 col, char ch);
void oled_show_string(uint8 row, uint8 col, const char *str);
void oled_show_num(uint8 row, uint8 col, uint32 num, uint8 len);
void oled_show_signed_num(uint8 row, uint8 col, int num, uint8 len);
void oled_show_hex_num(uint8 row, uint8 col, uint32 num, uint8 len);
void oled_show_bin_num(uint8 row, uint8 col, uint32 num, uint8 len);
void oled_show_hex32(uint8 row, uint8 col, uint32 val);
void oled_printf(uint8 row, uint8 col, const char *fmt, ...);

// Convenience: write + clear + set cursor
void oled_write_row(uint8 row, const char *str);
void oled_write_hexrow(uint8 row, const char *label, const uint8 *data, int n);

#endif
