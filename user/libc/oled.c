#include "oled.h"
#include "fcntl.h"
#include "user.h"
#include "game_data.h"

#define MAP_FAILED ((void *)-1)
static unsigned char *fb = 0;
int oled_fd = 0; 

void
OLED_init(void)
{
  oled_fd = open("/dev/oledfb", O_RDWR);
  if (oled_fd < 0) {
    printf("OLED_init: open /dev/oledfb failed\n");
    exit(1);
  }

  fb = mmap(0, OLEDFB_FB_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, oled_fd, 0);
  if (fb == MAP_FAILED || fb == 0) {
    printf("OLED_init: mmap failed\n");
    exit(1);
  }
}

// Set one pixel; out-of-screen coordinates are ignored.
void
OLED_DrawPoint(int X, int Y)
{
  if (fb == 0 || X < 0 || X >= OLEDFB_W || Y < 0 || Y >= OLEDFB_H)
    return;
  fb[(Y >> 3) * OLEDFB_W + X] |= (unsigned char)(1u << (Y & 7));
}

void
OLED_Clear(void)
{
  int i;
  if (fb == 0)
    return;
  for (i = 0; i < OLEDFB_FB_SIZE; i++)
    fb[i] = 0;
}

// Zero the rectangular region [X, X+W) x [Y, Y+H), clipped to the screen.
// Needed because OLED_ShowImage / OLED_DrawPoint only set bits (OR); a
// per-row clear keeps stale glyphs from overlapping shorter strings.
void
OLED_ClearArea(int X, int Y, int W, int H)
{
  int col, page, bit;
  int x0, x1, p0, p1, b0, b1;
  uint8 mask;

  if (fb == 0 || W <= 0 || H <= 0)
    return;
  if (X >= OLEDFB_W || Y >= OLEDFB_H || X + W <= 0 || Y + H <= 0)
    return;

  x0 = X > 0 ? X : 0;
  x1 = X + W < OLEDFB_W ? X + W : OLEDFB_W;
  p0 = Y >> 3;
  p1 = (Y + H - 1) >> 3;
  if (p1 > OLEDFB_H / 8 - 1)
    p1 = OLEDFB_H / 8 - 1;

  for (page = p0; page <= p1; page++) {
    b0 = Y > page * 8 ? Y - page * 8 : 0;
    b1 = Y + H < (page + 1) * 8 ? Y + H - page * 8 : 8;
    mask = 0xFF;
    for (bit = b0; bit < b1; bit++)
      mask &= (uint8)~(1u << bit);
    for (col = x0; col < x1; col++)
      fb[page * OLEDFB_W + col] &= mask;
  }
}

void
OLED_ShowImage(int X, int Y, int w, int h, const unsigned char *Image)
{
  int col0, col1;    // [col0, col1): columns actually visible
  int row, page, bit;
  int sr;            // row relative to the image top

  if (fb == 0 || w <= 0 || h <= 0 || Image == 0)
    return;
  if (X >= OLEDFB_W || Y >= OLEDFB_H || X + w <= 0 || Y + h <= 0)
    return;

  col0 = X > 0 ? X : 0;
  col1 = X + w < OLEDFB_W ? X + w : OLEDFB_W;
  if (col1 <= col0)
    return;

  if (Y % 8 == 0 && h % 8 == 0) {
    // Page-aligned fast path: one bitmap byte per fb byte.
    int p = Y >> 3;
    int img_col = col0 - X;      // source column of the left clipped column
    while (p < 8 && h > 0) {
      int fb_off = p * OLEDFB_W + col0;
      int n = col1 - col0;
      int i;
      for (i = 0; i < n; i++)
        fb[fb_off + i] |= Image[img_col + i];
      Image += w;                // next 8-row page
      p++;
      h -= 8;
    }
  } else {
    // Non-page-aligned: walk each pixel row, place it on its (row%8) bit.
    for (row = Y; row < Y + h && row < OLEDFB_H; row++) {
      sr = row - Y;
      page = row >> 3;
      bit = row & 7;
      {
        int img_off = (sr >> 3) * w + (col0 - X);
        int col;
        for (col = col0; col < col1; col++) {
          if (Image[img_off + (col - col0)] & (1u << (sr & 7)))
            fb[page * OLEDFB_W + col] |= (unsigned char)(1u << bit);
        }
      }
    }
  }
}

void
OLED_ShowChar(int X, int Y, char Char, unsigned char FontSize)
{
  unsigned char idx = (unsigned char)Char - ' ';

  if (FontSize == OLED_8X16)        // 8 px wide, 16 px tall
    OLED_ShowImage(X, Y, 8, 16, OLED_F8x16[idx]);
  else if (FontSize == OLED_6X8)    // 6 px wide, 8 px tall
    OLED_ShowImage(X, Y, 6, 8, OLED_F6x8[idx]);
}

void
OLED_ShowString(int X, int Y, const char *String, unsigned char FontSize)
{
  int i;
  for (i = 0; String[i] != '\0'; i++)
    OLED_ShowChar(X + i * FontSize, Y, String[i], FontSize);
}

static unsigned int
OLED_Pow(unsigned int base, unsigned int exp)
{
  unsigned int result = 1;
  while (exp--)
    result *= base;
  return result;
}

void
OLED_ShowNum(int X, int Y, unsigned int Number, unsigned char Length, unsigned char FontSize)
{
  unsigned char i;
  for (i = 0; i < Length; i++)
    OLED_ShowChar(X + i * FontSize, Y, Number / OLED_Pow(10, Length - i - 1) % 10 + '0',
                  FontSize);
}

static const char oled_hexdigits[] = "0123456789ABCDEF";

void
OLED_ShowHexNum(int X, int Y, unsigned int Number, unsigned char Length,
                unsigned char FontSize)
{
  unsigned char i;

  if (Length > 8)
    Length = 8;
  for (i = 0; i < Length; i++)
    OLED_ShowChar(X + i * FontSize, Y,
                  oled_hexdigits[(Number >> ((Length - 1 - i) * 4)) & 0xF],
                  FontSize);
}

void
OLED_ShowHex32(int X, int Y, unsigned int val, unsigned char FontSize)
{
  OLED_ShowHexNum(X, Y, val, 8, FontSize);
}

void
OLED_ShowHexRow(int X, int Y, const char *label, const unsigned char *data,
                int n, unsigned char FontSize)
{
  char buf[64];
  int pos = 0;

  if (label)
    while (label[pos] && pos < (int)sizeof(buf) - 1) {
      buf[pos] = label[pos];
      pos++;
    }

  if (data && n > 0) {
    int j;
    for (j = 0; j < n && pos + 1 < (int)sizeof(buf); j++) {
      buf[pos++] = oled_hexdigits[(data[j] >> 4) & 0xF];
      buf[pos++] = oled_hexdigits[data[j] & 0xF];
    }
  }
  buf[pos] = 0;

  OLED_ShowString(X, Y, buf, FontSize);
}

void
OLED_DrawLine(int X0, int Y0, int X1, int Y1)
{
  int x, y, dx, dy, d, incrE, incrNE, temp;
  int x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
  unsigned char yflag = 0, xyflag = 0;

  if (y0 == y1) {       // horizontal
    if (x0 > x1) { temp = x0; x0 = x1; x1 = temp; }
    for (x = x0; x <= x1; x++)
      OLED_DrawPoint(x, y0);
  } else if (x0 == x1) {  // vertical
    if (y0 > y1) { temp = y0; y0 = y1; y1 = temp; }
    for (y = y0; y <= y1; y++)
      OLED_DrawPoint(x0, y);
  } else {              // diagonal, Bresenham
    if (x0 > x1) {      // start from the smaller x
      temp = x0; x0 = x1; x1 = temp;
      temp = y0; y0 = y1; y1 = temp;
    }
    if (y0 > y1) {      // reflect the negative slope to the first octant
      y0 = -y0;
      y1 = -y1;
      yflag = 1;
    }
    if (y1 - y0 > x1 - x0) {  // slope > 1: swap axes -> first octant
      temp = x0; x0 = y0; y0 = temp;
      temp = x1; x1 = y1; y1 = temp;
      xyflag = 1;
    }

    dx = x1 - x0;
    dy = y1 - y0;
    incrE = 2 * dy;
    incrNE = 2 * (dy - dx);
    d = 2 * dy - dx;
    x = x0;
    y = y0;

    if (yflag && xyflag) { OLED_DrawPoint(y, -x); }
    else if (yflag)      { OLED_DrawPoint(x, -y); }
    else if (xyflag)     { OLED_DrawPoint(y, x); }
    else                 { OLED_DrawPoint(x, y); }

    while (x < x1) {
      x++;
      if (d < 0) {
        d += incrE;
      } else {
        y++;
        d += incrNE;
      }
      if (yflag && xyflag) { OLED_DrawPoint(y, -x); }
      else if (yflag)      { OLED_DrawPoint(x, -y); }
      else if (xyflag)     { OLED_DrawPoint(y, x); }
      else                 { OLED_DrawPoint(x, y); }
    }
  }
}

void
OLED_DrawRectangle(int X, int Y, int Width, int Height, int IsFilled)
{
  int i, j;
  if (Width <= 0 || Height <= 0)
    return;

  if (!IsFilled) {
    for (i = X; i < X + Width; i++) {
      OLED_DrawPoint(i, Y);
      OLED_DrawPoint(i, Y + Height - 1);
    }
    for (i = Y; i < Y + Height; i++) {
      OLED_DrawPoint(X, i);
      OLED_DrawPoint(X + Width - 1, i);
    }
  } else {
    for (i = X; i < X + Width; i++)
      for (j = Y; j < Y + Height; j++)
        OLED_DrawPoint(i, j);
  }
}

void
OLED_DrawCircle(int X, int Y, int Radius, int IsFilled)
{
  int x, y, d, j;
  if (Radius <= 0)
    return;

  d = 1 - Radius;
  x = 0;
  y = Radius;

  OLED_DrawPoint(X + x, Y + y);
  OLED_DrawPoint(X - x, Y - y);
  OLED_DrawPoint(X + y, Y + x);
  OLED_DrawPoint(X - y, Y - x);

  if (IsFilled) {
    for (j = -y; j < y; j++)
      OLED_DrawPoint(X, Y + j);
  }

  while (x < y) {
    x++;
    if (d < 0) {
      d += 2 * x + 1;
    } else {
      y--;
      d += 2 * (x - y) + 1;
    }

    OLED_DrawPoint(X + x, Y + y);
    OLED_DrawPoint(X + y, Y + x);
    OLED_DrawPoint(X - x, Y - y);
    OLED_DrawPoint(X - y, Y - x);
    OLED_DrawPoint(X + x, Y - y);
    OLED_DrawPoint(X + y, Y - x);
    OLED_DrawPoint(X - x, Y + y);
    OLED_DrawPoint(X - y, Y + x);

    if (IsFilled) {
      for (j = -y; j < y; j++) {
        OLED_DrawPoint(X + x, Y + j);
        OLED_DrawPoint(X - x, Y + j);
      }
      for (j = -x; j < x; j++) {
        OLED_DrawPoint(X - y, Y + j);
        OLED_DrawPoint(X + y, Y + j);
      }
    }
  }
}

int
OLED_Flush(void)
{
  return ioctl(oled_fd, OLEDFB_IOCTL_FLUSH, (uint64)fb);
}
