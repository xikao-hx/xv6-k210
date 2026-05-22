#ifndef __UARTDEV_H
#define __UARTDEV_H

#define UART_IOCTL_RAW_START  0x01  // disable UART RX interrupt for raw polling
#define UART_IOCTL_RAW_END    0x02  // re-enable UART RX interrupt
#define UART_IOCTL_SET_BAUD   0x03  // set UART baud rate (arg = baud)
#define UART_IOCTL_FLUSH_RX   0x04  // drain UART RX FIFO
#define UART_IOCTL_GETC       0x05  // read one byte (bypasses kalloc/copyout)

#endif
