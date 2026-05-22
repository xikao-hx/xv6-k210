#ifndef __UARTDEV_H
#define __UARTDEV_H

#define UART_IOCTL_RAW_START  0x01  // route RX interrupts into the raw ring
#define UART_IOCTL_RAW_END    0x02  // restore RX interrupts to the console

#endif
