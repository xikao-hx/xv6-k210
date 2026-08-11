// 1bpp SSD1306 page-format renderer for the /dev/oledfb framebuffer.
// Layout: fb[page*128 + col], bit (row%8) = pixel (col,row).  All drawing
// is pure integer; no I2C, no device open, no float, no malloc - the only
// state is the caller-supplied fb pointer.
//
// Fonts OLED_F8x16 / OLED_F6x8 are defined in oled_font.h and compiled into
// user/libc/oled.c, which is part of ULIB on the K210 build.  They are
// referenced here by extern (same trick as user/test/oledfbtest.c) so the
// definition is never duplicated into dino programs.

#include "game_render.h"
#include "fcntl.h"
#include "user.h"
#include "game_data.h"

static unsigned char *fb = 0;
#define MAP_FAILED ((void *)-1)

int
OLED_init(void)
{
  int fd = open("/dev/oledfb", O_RDWR);
  if (fd < 0) {
    printf("OLED_init: open /dev/oledfb failed\n");
    exit(1);
  }

  fb = mmap(0, OLEDFB_FB_SIZE, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
  if (fb == MAP_FAILED || fb == 0) {
    printf("OLED_init: mmap failed\n");
    exit(1);
  }
  printf("OLED_init Success: fd=%d fb=%p\n", fd, fb);

  return fd;
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

// Draw a w x h bitmap at (X,Y), OR-ing into the fb (a frame is cleared
// first, then sprites are stacked, so OR == overwrite for a single frame).
// The bitmap is page-format: byte (row/8)*w + col carries the 8 vertical
// pixels of rows (row/8)*8 .. (row/8)*8+7 at column col, bit k = the k'th.
//
// Columns are clipped to the screen (clouds/ground scroll partially off the
// right edge).  Y/h in this game are never negative, but the clip keeps
// off-screen draws safe.  When the bitmap starts on a page boundary the
// bytes land 1:1 in the fb (fast byte-OR path); otherwise every row is
// shifted to its (row%8) bit (bit path).
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
oled_pow(unsigned int base, unsigned int exp)
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
    OLED_ShowChar(X + i * FontSize, Y, Number / oled_pow(10, Length - i - 1) % 10 + '0',
                  FontSize);
}

// Bresenham line, ported verbatim from game/OLED.c (handles the 8
// octants via the yflag/xyflag transforms; DrawPoint does the clipping).
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

// Midpoint circle (Bresenham), ported verbatim from game/OLED.c.
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
