#ifndef __OLEDFB_H
#define __OLEDFB_H

// Framebuffer device for the SSD1306 OLED (128x64, 1bpp page format).
// The user maps the 1024-byte framebuffer via mmap, draws into it, then
// issues OLEDFB_IOCTL_FLUSH to push the whole buffer over I2C.
#define OLEDFB_W        128
#define OLEDFB_H        64
#define OLEDFB_FB_SIZE  (OLEDFB_W * OLEDFB_H / 8)   // 1024

#define OLEDFB_IOCTL_FLUSH  1

void oledfbdev_init(void);

#endif
