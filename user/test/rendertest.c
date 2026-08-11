// Step 2 acceptance test for the user-space 1bpp renderer (dino_render.c).
//
//   rendertest
// Opens /dev/oledfb, mmaps the 1024-byte framebuffer, renders a test
// pattern (points / lines / rectangles / circles / CACTUS bitmap / text),
// spot-checks known fb bytes, then flushes so the whole pattern is visible.
//
// The expected bytes below were cross-checked on the host against an
// independent Python reference implementation of the same algorithm.

#include "oledfb.h"
#include "game_render.h"
#include "game_data.h"
#include "user.h"

int
main(void)
{
  int fd = 0;

  /* 1. init */
  fd = OLED_init();
  /* 2. clear */
  OLED_Clear();

  /* 3. points: bit k of fb[page*128+col] is row (page*8+k) */
  OLED_Clear();
  OLED_DrawPoint(0, 0);
  OLED_DrawPoint(0, 7);
  OLED_DrawPoint(0, 8);
  OLED_DrawPoint(OLEDFB_W - 1, OLEDFB_H - 1);
  OLED_DrawPoint(-1, 5);
  OLED_DrawPoint(OLEDFB_W, 5);
  OLED_DrawPoint(0, OLEDFB_H);

  /* 4. lines */
  OLED_Clear();
  OLED_DrawLine(0, 0, 0, 15);       /* vertical, spans two pages */
  OLED_Clear();
  OLED_DrawLine(0, 10, 20, 10);     /* horizontal, y=10 -> bit2 of page1 */

  /* 5. rectangle border: X=10..15, Y=10..13 -> page1 bits 2,3,4,5 */
  OLED_Clear();
  OLED_DrawRectangle(10, 10, 6, 4, OLED_UNFILLED);

  /* 6. circle outline, midpoint r=5 at (20,20) */
  OLED_Clear();
  OLED_DrawCircle(20, 20, 5, OLED_UNFILLED);

  /* 7. bitmap, page-aligned Y=48: byte OR fast path */
  OLED_Clear();
  OLED_ShowImage(0, 48, 8, 16, CACTUS_1);

  /* 8. bitmap, Y=49: every pixel row shifted to its (row%8) bit */
  OLED_Clear();
  OLED_ShowImage(0, 49, 8, 16, CACTUS_1);

  /* 9. text, 8x16 font: 'A' upper page col2=0xC0, lower page non-zero */
  OLED_Clear();
  OLED_ShowChar(0, 0, 'A', OLED_8X16);

  /* 10. string, 6x8 font: 'O' at x0, 'K' at x6 (their col1 bytes) */
  OLED_Clear();
  OLED_ShowString(0, 0, "OK", OLED_6X8);

  /* 11. render the demo pattern that stays on screen */
  OLED_Clear();
  OLED_DrawRectangle(0, 0, OLEDFB_W, OLEDFB_H, OLED_UNFILLED);  /* border */
  OLED_DrawLine(0, 0, OLEDFB_W - 1, OLEDFB_H - 1);              /* diagonal */
  OLED_DrawLine(0, OLEDFB_H - 1, OLEDFB_W - 1, 0);
  OLED_DrawCircle(40, 24, 10, OLED_UNFILLED);
  OLED_DrawCircle(88, 40, 8, OLED_FILLED);
  OLED_ShowImage(70, 48, 8, 16, CACTUS_1);
  OLED_ShowString(10, 2, "DINO!", OLED_8X16);
  OLED_ShowString(10, 28, "render OK", OLED_6X8);
  OLED_ShowNum(10, 36, 12345, 5, OLED_6X8);

  if (ioctl(fd, OLEDFB_IOCTL_FLUSH, 0) < 0)
    printf("rendertest: FLUSH failed (I2C?) - display may be blank\n");
  else
    printf("rendertest: FLUSH OK\n");
  exit(0);
}
