#ifndef __UARTDEV_H
#define __UARTDEV_H

#define UART_IOCTL_RAW_START  0x01  // route RX interrupts into the raw ring
#define UART_IOCTL_RAW_END    0x02  // restore RX interrupts to the console
#define UART_IOCTL_SET_BAUD   0x03  // set UART baud rate (arg = baud)
#define UART_IOCTL_GET_BAUD_INFO 0x04  // copy struct uart_baud_info to arg
#define UART_IOCTL_GET_RAW_STATS 0x05  // copy struct uart_raw_stats to arg

struct uart_baud_info {
  uint32 requested;
  uint32 actual;
  uint32 div;
  uint32 clock;
};

struct uart_raw_stats {
  uint32 dropped;
  uint32 buffered;
  uint32 capacity;
  uint32 mode;
};
#endif
