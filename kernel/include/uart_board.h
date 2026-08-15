#ifndef __UART_BOARD_H
#define __UART_BOARD_H

#include "uart.h"
#include "uart-dw.h"

// Board-level UART instances.  The generic driver (kernel/driver/uart.c)
// accesses controllers via this extern table; UART_DEVICE_1/2/3 and
// UART_DEVICE_MAX come from the SDK enum in uart-dw.h.  Unused slots (UART2/3)
// are NULL -- opening /dev/uart2|3 then fails, and uartdev_init skips them.

extern struct uart_controller *uart_ctrls[UART_DEVICE_MAX];

#endif
