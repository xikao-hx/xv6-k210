#ifndef __UART_BOARD_H
#define __UART_BOARD_H

#include "uart.h"

// Board-level UART instances.  The generic DW UART driver (kernel/driver/uart.c)
// operates purely on a struct uart_controller *; the board layer
// (kernel/board/uart_board.c) builds each instance's hardware identity (base,
// fpioa pins, DMA channels) plus its embedded runtime state, exposed via the
// uart_ctrls[] table.  Table index == devsw minor (/dev/uart1 = minor 0).
//
// Device indexes are the SDK's uart_device_number_t enum (uart-dw.h, pulled in
// through uart.h): UART_DEVICE_1=0, UART_DEVICE_2=1, UART_DEVICE_3=2,
// UART_DEVICE_MAX=3.

extern struct uart_controller *uart_ctrls[UART_DEVICE_MAX];

#endif
