#ifndef __UART_H
#define __UART_H

#include "types.h"

// UART driver for the K210 generic UART (DW APB 16550), independent from
// UARTHS (which drives the console).  Byte-stream interface with two
// switchable RX/TX paths each: interrupt-driven (Step 1) and DMA (Step 2).

// Physical pins routed to the UART TX/RX functions.  Direction (which IO is
// TX vs RX) is pending real-hardware confirmation; swap these if the board
// behaves the other way round.  u1test.c documents IO22/IO23 as the wiring
// used in practice -- if the board still shows no signal, this is the first
// knob to check.
#define UART_TX_IO  7
#define UART_RX_IO  8

// RX/TX operating modes, the arg to UART_IOCTL_SET_*_MODE and what
// uart_get_rx_stats() reports in info[3].
#define UART_MODE_INT 0
#define UART_MODE_DMA 1

// uart1-specific ioctl codes.  Numerically distinct from CONSOLE_IOCTL_*
// (0x01-0x08); uartdev.c routes them to uart_set_rx_mode/uart_set_tx_mode.
#define UART_IOCTL_SET_RX_MODE 0x21
#define UART_IOCTL_SET_TX_MODE 0x22

void uartinit(void);
void uartintr(void);
int  uart_read(char *dst, int n);
int  uart_write(const char *src, int n);
void uart_set_baud(int baud);
void uart_get_baud_info(uint32 *info);
void uart_flush_rx(void);
void uart_get_rx_stats(uint32 *info);
void uart_set_rx_mode(int mode);
void uart_set_tx_mode(int mode);
void uart_dma_rx_intr(void);   // DMA CH5 completion: harvest + re-arm RX DMA

#endif