#ifndef __UART_H
#define __UART_H

#include "types.h"

// UART driver for the K210 generic UART (DW APB 16550), independent from
// UARTHS (which drives the console).  Byte-stream interface: interrupt-driven
// RX/TX (Step 1), DMA-mode RX/TX planned (Step 2).

// Physical pins routed to the UART TX/RX functions.  Direction (which IO is
// TX vs RX) is pending real-hardware confirmation; swap these if the board
// behaves the other way round.  u1test.c documents IO22/IO23 as the wiring
// used in practice -- if the board still shows no signal, this is the first
// knob to check.
#define UART_TX_IO  7
#define UART_RX_IO  8

void uartinit(void);
void uartintr(void);
int  uart_read(char *dst, int n);
int  uart_write(const char *src, int n);
void uart_set_baud(int baud);
void uart_get_baud_info(uint32 *info);
void uart_flush_rx(void);
void uart_get_rx_stats(uint32 *info);

// Poll-mode primitives: bypass the soft ring and interrupts, poke the DW
// hardware directly.  Used to validate the TX/RX path independent of the
// interrupt machinery (dev mode CONSOLE_MODE_POLL).
int  uart_poll_putc(int c);   // blocking: waits LSR bit5 clear (TX not busy), writes THR
int  uart_poll_getc(void);    // non-blocking: -1 when no byte ready
void uart_set_poll(int on);   // on: disable RX/TX interrupts for pure polling

#endif
