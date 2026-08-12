#ifndef __OLED_H
#define __OLED_H

#include "oledfb.h"

#define OLED_8X16   8
#define OLED_6X8    6

// Fill selectors for DrawRectangle / DrawCircle.
#define OLED_UNFILLED  0
#define OLED_FILLED    1

void OLED_init(void);
void OLED_Clear(void);
void OLED_ClearArea(int X, int Y, int W, int H);
void OLED_ShowImage(int X, int Y, int w, int h, const unsigned char *Image);
void OLED_ShowChar(int X, int Y, char Char, unsigned char FontSize);
void OLED_ShowString(int X, int Y, const char *String, unsigned char FontSize);
void OLED_ShowNum(int X, int Y, unsigned int Number, unsigned char Length, unsigned char FontSize);
void OLED_ShowHexNum(int X, int Y, unsigned int Number, unsigned char Length, unsigned char FontSize);
void OLED_ShowHex32(int X, int Y, unsigned int val, unsigned char FontSize);
void OLED_ShowHexRow(int X, int Y, const char *label, const unsigned char *data, int n, unsigned char FontSize);
void OLED_DrawPoint(int X, int Y);
void OLED_DrawLine(int X0, int Y0, int X1, int Y1);
void OLED_DrawRectangle(int X, int Y, int Width, int Height, int IsFilled);
void OLED_DrawCircle(int X, int Y, int Radius, int IsFilled);
int OLED_Flush(void);

#endif /* __OLED_H */
