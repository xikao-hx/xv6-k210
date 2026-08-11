// Step 1 acceptance test for /dev/oledfb.
//
//   oledfbtest        open+mmap, render text into the fb, verify roundtrip
//   oledfbtest segv   open+mmap, then write past the VMA -> expect SEGV
//
// Text rendering: OLED_F6x8 (6x8, one byte per column) and OLED_F8x16 both
// use the SSD1306 page convention, bit k = page row k (bit0 = the page's
// top row), so glyph columns copy straight into a page row.  F6x8 fills one
// page per character; F8x16 spans two pages (first 8 bytes = upper 8 rows,
// second 8 bytes = lower 8 rows).  Neither needs a bit flip.
#include "oledfb.h"
#include "fcntl.h"
#include "user.h"

// Both fonts are defined in oled_font.h, which is only included by oled.c;
// referencing them here as externs links against the ulib copy.
extern const unsigned char OLED_F8x16[][16];
extern const unsigned char OLED_F6x8[][6];

#define MAP_FAILED ((void *)-1)

// 6x8 font, page-format copy: fb[page*128 + x + col] = glyph[col].
// OLED_F6x8 is already in the SSD1306 page convention (bit0 = top row of
// the page), so glyph columns go in as-is - no bit flip.
static void
draw_char6(char *fb, int page, int x, char c)
{
  const unsigned char *g = OLED_F6x8[(unsigned char)c - ' '];
  for(int i = 0; i < 6; i++)
    fb[page * OLEDFB_W + x + i] = g[i];
}

// 8x16 font: upper 8 rows into page, lower 8 rows into page+1.
static void
draw_char8(char *fb, int page, int x, char c)
{
  const unsigned char *g = OLED_F8x16[(unsigned char)c - ' '];
  for(int i = 0; i < 8; i++) {
    fb[page * OLEDFB_W + x + i] = g[i];
    fb[(page + 1) * OLEDFB_W + x + i] = g[8 + i];
  }
}

int
main(int argc, char *argv[])
{
  int fd = open("/dev/oledfb", O_RDWR);
  if(fd < 0) {
    printf("oledfbtest: open /dev/oledfb failed\n");
    exit(1);
  }

  char *fb = mmap(0, OLEDFB_FB_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if(fb == MAP_FAILED || fb == 0) {
    printf("oledfbtest: mmap failed\n");
    exit(1);
  }
  printf("oledfbtest: fd=%d fb=%p\n", fd, fb);

  if(argc > 1 && strcmp(argv[1], "segv") == 0) {
    printf("oledfbtest: about to write past the mapping (expect SEGV)\n");
    *(volatile char *)(fb + 4096) = 1;
    printf("oledfbtest: survived OOB write (UNEXPECTED)\n");
    exit(0);
  }

  /* minimal sentinel: a single byte write must read back identical */
  fb[0] = 0x5A;
  unsigned char sent = (unsigned char)fb[0];
  fb[0] = 0x00;
  printf("oledfbtest: sentinel fb[0]=%x (expect 5a) %s\n",
         sent, sent == 0x5A ? "OK" : "FAIL");

  /* clear the fb, then render two lines of text.
   * ORDER SWAPPED (I2C diagnostic): "xv6 DINO" (6x8) first at page 0,
   * "OLED" (8x16) second at pages 4-5.  If the SSD1306 corrupts later
   * bytes of the 1024-byte burst, the SECOND line garbles no matter which
   * text it holds; if the F6x8 rendering itself is wrong, the FIRST line
   * garbles. */
  for(int i = 0; i < OLEDFB_FB_SIZE; i++)
    fb[i] = 0x00;

  const char *line1 = "xv6 DINO";    /* 9 x 6x8 = 54 px wide, page 0 */
  const char *line2 = "OLED";        /* 4 x 8x16 = 32 px, pages 4-5 */
  int x1 = (OLEDFB_W - 9 * 6) / 2;   /* 37: centered */
  int x2 = (OLEDFB_W - 4 * 8) / 2;   /* 48: centered */
  for(int i = 0; line1[i]; i++)
    draw_char6(fb, 0, x1 + i * 6, line1[i]);
  for(int i = 0; line2[i]; i++)
    draw_char8(fb, 4, x2 + i * 8, line2[i]);

  /* roundtrip: the rendered bytes must read back identical */
  int nonzero = 0;
  for(int i = 0; i < OLEDFB_FB_SIZE; i++) {
    if(fb[i] != 0) {
      nonzero = 1;
      break;
    }
  }
  printf("oledfbtest: line1[pg0] 'x' cols=%x %x %x %x %x %x\n",
         (unsigned char)fb[0 * OLEDFB_W + x1],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 1],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 2],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 3],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 4],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 5]);
  printf("oledfbtest: line1[pg0] 'v' cols=%x %x %x %x %x %x\n",
         (unsigned char)fb[0 * OLEDFB_W + x1 + 6],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 7],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 8],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 9],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 10],
         (unsigned char)fb[0 * OLEDFB_W + x1 + 11]);
  printf("oledfbtest: line2[pg4] 'O' cols=%x %x %x %x %x %x %x %x\n",
         (unsigned char)fb[4 * OLEDFB_W + x2],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 1],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 2],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 3],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 4],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 5],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 6],
         (unsigned char)fb[4 * OLEDFB_W + x2 + 7]);
  if(nonzero)
    printf("oledfbtest: text written, readback OK\n");
  else {
    printf("oledfbtest: mmap readback MISMATCH (fb all zero)\n");
    for(int i = 0; i < 16; i++)
      printf(" %x", (unsigned char)fb[i]);
    printf("\n");
  }

  if(ioctl(fd, OLEDFB_IOCTL_FLUSH, 0) < 0)
    printf("oledfbtest: FLUSH failed (I2C?) - display may be blank\n");
  else
    printf("oledfbtest: FLUSH OK\n");

  exit(0);
}
