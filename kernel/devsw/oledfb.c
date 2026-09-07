#include "oledfb.h"

#include "dev.h"
#include "fcntl.h"
#include "file.h"
#include "kalloc.h"
#include "mmap.h"
#include "param.h"
#include "printf.h"
#include "proc.h"
#include "riscv.h"
#include "sleeplock.h"
#include "string.h"
#include "types.h"

#include "i2c.h"
#include "i2c_board.h"

// SSD1306 over I2C in horizontal addressing mode: after the display RAM is
// full (128 cols) the page pointer auto-increments, so the whole 1024-byte
// framebuffer can be pushed as a single data burst.
#define OLED_ADDR       0x3C
#define OLED_CTRL_CMD   0x00
#define OLED_CTRL_DAT   0x40

struct oledfb {
  struct sleeplock lock;
  uint8 flush_buf[OLEDFB_FB_SIZE];  // staging for the 1024-byte DMA msg: {0x40, fb[0..1022]}
};

static void
oledfb_delay_ms(uint32 ms)
{
  uint64 target = r_time() + (uint64)ms * (INTERVAL / 5);
  while(r_time() < target);
}

static int
oled_send_cmd(struct i2c_device *dev, uint8 cmd)
{
  uint8 buf[2] = {OLED_CTRL_CMD, cmd};
  struct i2c_msg msg;

  msg.addr = OLED_ADDR;
  msg.flags = 0;
  msg.len = 2;
  msg.buf = buf;
  return i2c_transfer(dev, &msg, 1);
}

static void
oledfb_init(void)
{
  struct i2c_device *dev = i2c_device_get(I2C_DEV_OLED);
  uint8 seq[] = {
    0xAE,             /* display off */
    0xD5, 0x80,       /* clock div / oscillator freq */
    0xA8, 0x3F,       /* multiplex ratio: 64 lines */
    0xD3, 0x00,       /* display offset */
    0x40,             /* start line = 0 */
    0x20, 0x00,       /* memory addressing mode: horizontal */
    0xA1,             /* segment remap */
    0xC8,             /* COM scan direction */
    0xDA, 0x12,       /* COM pins hw config */
    0x81, 0xCF,       /* contrast */
    0xD9, 0xF1,       /* pre-charge */
    0xDB, 0x30,       /* VCOMH deselect level */
    0xA4,             /* entire display on (follow RAM) */
    0xA6,             /* normal (non-inverted) display */
    0x8D, 0x14,       /* charge pump on */
    0xAF,             /* display on */
  };
  int i;

  if(dev == 0)
    return;
  oledfb_delay_ms(5);
  for(i = 0; i < (int)sizeof(seq); i++)
    oled_send_cmd(dev, seq[i]);
}

// Push the whole framebuffer to the panel.  In horizontal addressing mode the
// display RAM address auto-increments across page boundaries and across a
// repeated START, so a set-column/set-page command followed by data bursts
// covers all 8 pages.  The data is split into a 1024-byte DMA message
// ({0x40, fb[0..1022]}) plus a 2-byte polling tail ({0x40, fb[1023]})
static int
oledfb_flush(struct oledfb *of, uint8 *fb)
{
  struct i2c_device *dev = i2c_device_get(I2C_DEV_OLED);
  struct i2c_msg msgs[3];

  // {ctrl, set-col-addr, start, end, set-page-addr, start, end}
  uint8 cursor[7] = {OLED_CTRL_CMD, 0x21, 0x00, 0x7F, 0x22, 0x00, 0x07};
  uint8 tail[2] = {OLED_CTRL_DAT, 0};   // {ctrl-data, last fb byte}
  int ret;

  if(dev == 0)
    return -1;
  if(fb == 0)
    return -1;

  of->flush_buf[0] = OLED_CTRL_DAT;
  memmove(of->flush_buf + 1, fb, OLEDFB_FB_SIZE - 1);   // fb[0..1022]
  tail[1] = fb[OLEDFB_FB_SIZE - 1];                     // fb[1023]

  msgs[0].addr = OLED_ADDR;
  msgs[0].flags = 0;
  msgs[0].len = sizeof(cursor);
  msgs[0].buf = cursor;

  msgs[1].addr = OLED_ADDR;
  msgs[1].flags = 0;
  msgs[1].len = OLEDFB_FB_SIZE;       // {0x40, fb[0..1022]} - DMA, fits one page
  msgs[1].buf = of->flush_buf;

  msgs[2].addr = OLED_ADDR;
  msgs[2].flags = 0;
  msgs[2].len = sizeof(tail);         // {0x40, fb[1023]} - polling tail
  msgs[2].buf = tail;

  ret = i2c_transfer(dev, msgs, 3);

  return ret;
}

static int
oledfb_open(struct file *file)
{
  struct oledfb *of;

  if(file->minor != 0)
    return -1;

  of = kmalloc(sizeof(*of));
  if(of == 0)
    return -1;
  memset(of, 0, sizeof(*of));
  initsleeplock(&of->lock, "oledfb");

  oledfb_init();

  file->private_data = of;
  return 0;
}

static int
oledfb_close(struct file *file)
{
  struct oledfb *of = file->private_data;

  if(of) {
    kfree(of);
    file->private_data = 0;
  }
  return 0;
}

static int
oledfb_mmap(struct file *file, struct vma_area *vma, uint64 offset)
{
  uint64 length = vma->valid_end - vma->start;

  (void)file;
  if(offset != 0 || length != OLEDFB_FB_SIZE ||
     vma->flags != MAP_SHARED || (vma->prot & PROT_EXEC))
    return -1;
  vma->populate = 1;
  return 0;
}

static int
oledfb_ioctl(struct file *file, uint64 cmd, uint64 arg)
{
  struct oledfb *of = file->private_data;
  uint8 *fb;
  int ret;

  if(of == 0)
    return -1;
  switch(cmd) {
  case OLEDFB_IOCTL_FLUSH:
    fb = vma_device_page_address(myproc(), file, arg, 0);
    if(fb == 0)
      return -1;
    acquiresleep(&of->lock);
    ret = oledfb_flush(of, fb);
    releasesleep(&of->lock);
    return ret;
  default:
    return -1;
  }
  
}

static const struct file_operations oledfb_ops = {
  .open = oledfb_open,
  .close = oledfb_close,
  .mmap = oledfb_mmap,
  .ioctl = oledfb_ioctl,
};

void
oledfbdev_init(void)
{
  if(device_register(DEV_OLEDFB, "oledfb", &oledfb_ops) < 0)
    panic("oledfb device register");
}
