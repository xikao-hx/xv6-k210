#ifndef __GAME_RENDER_H
#define __GAME_RENDER_H

// 1bpp SSD1306 page-format renderer for the /dev/oledfb framebuffer.
//
// Framebuffer layout: 1024 bytes, fb[page*128 + col]; bit (row%8) of the
// byte at (col,row) is pixel (col,row).  The image/sprite arrays (GROUND,
// DINO_*, CACTUS_*, fonts, ...) use the same convention: each byte holds
// 8 vertically stacked pixels, bit k = the (row/8)*8+k'th row.
//
// Pure integer drawing only - no I2C, no device-open logic, no float, no
// malloc.  The caller maps /dev/oledfb, passes the address to render_init(),
// draws, then flushes with ioctl(fd, OLEDFB_IOCTL_FLUSH, 0).

#include "oledfb.h"

// Font-size selectors.  Same names/values as game/OLED_Data.h so the
// ported DinoGame logic compiles unchanged.  Value = glyph width in px.
#define OLED_8X16   8
#define OLED_6X8    6

// Fill selectors for DrawRectangle / DrawCircle.
#define OLED_UNFILLED  0
#define OLED_FILLED    1


int OLED_init(void);
void OLED_Clear(void);
void OLED_ShowImage(int X, int Y, int w, int h, const unsigned char *Image);
void OLED_ShowChar(int X, int Y, char Char, unsigned char FontSize);
void OLED_ShowString(int X, int Y, const char *String, unsigned char FontSize);
void OLED_ShowNum(int X, int Y, unsigned int Number, unsigned char Length, unsigned char FontSize);
void OLED_DrawPoint(int X, int Y);
void OLED_DrawLine(int X0, int Y0, int X1, int Y1);
void OLED_DrawRectangle(int X, int Y, int Width, int Height, int IsFilled);
void OLED_DrawCircle(int X, int Y, int Radius, int IsFilled);

#endif /* __GAME_RENDER_H */
