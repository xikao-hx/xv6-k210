#ifndef __CONSOLE_H
#define __CONSOLE_H

#include "types.h"

#define CONSOLE_IOCTL_FLUSH_INPUT    0x01
#define CONSOLE_IOCTL_SET_MODE       0x02
#define CONSOLE_IOCTL_GET_MODE       0x03
#define CONSOLE_IOCTL_SET_BAUD       0x04
#define CONSOLE_IOCTL_GET_BAUD_INFO  0x05
#define CONSOLE_IOCTL_GET_RX_STATS   0x06

#define CONSOLE_MODE_TTY 0
#define CONSOLE_MODE_RAW 1

struct console_baud_info {
  uint32 requested;
  uint32 actual;
  uint32 div;
  uint32 clock;
};

struct console_rx_stats {
  uint32 dropped;
  uint32 buffered;
  uint32 capacity;
  uint32 mode;
};

void consoleinit(void);
void consputc(int);

#endif
