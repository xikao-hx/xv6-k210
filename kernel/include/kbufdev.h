#ifndef __KBUFDEV_H
#define __KBUFDEV_H

#include "types.h"

#define KBUF_IOCTL_FILL   1
#define KBUF_IOCTL_CHECK  2

struct kbuf_ioctl_region {
  uint offset;
  uint length;
  uchar value;
};

void kbufdev_init(void);

#endif
