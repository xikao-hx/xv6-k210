#ifndef __UART_H
#define __UART_H

#include "dmac.h"
#include "ringbuffer.h"
#include "spinlock.h"
#include "types.h"
#include "uart-dw.h"

// Multi-instance UART driver for the K210 generic UARTs (DW APB 16550),
// independent from UARTHS (which drives the console).  Byte-stream interface
// with two switchable RX/TX paths each: interrupt-driven (UART_MODE_INT) and
// DMA (UART_MODE_DMA).  One struct uart_controller per UART (see
// kernel/board/uart_board.c), passed explicitly so a single copy of the
// driver code serves UART1/2/3.

// RX/TX operating modes, the arg to UART_IOCTL_SET_*_MODE and what
// uart_get_rx_stats() reports in info[3].
#define UART_MODE_INT 0
#define UART_MODE_DMA 1

// uart1-specific ioctl codes.  Numerically distinct from CONSOLE_IOCTL_*
// (0x01-0x08); uartdev.c routes them to uart_set_rx_mode/uart_set_tx_mode.
#define UART_IOCTL_SET_RX_MODE 0x21
#define UART_IOCTL_SET_TX_MODE 0x22

// RX/TX ring capacities (usable bytes; the backing array is +1 to keep
// "empty" apart from "full") and DMA per-transfer block sizes in 32-bit
// words, 1 byte/word (as the SDK).  RX re-arms RXDMA_SIZE on each boundary;
// TX writes > TXDMA_SIZE split into blocks.
#define UART_RX_BUF_SIZE 32768
#define UART_TX_BUF_SIZE 4096
#define UART_RXDMA_SIZE 512
#define UART_TXDMA_SIZE 256

// RX/TX state, split like uarths.c.  Each buf is size+1: the ring reserves one
// slot to tell "empty" from "full"; ringbuffer_capacity() is the usable size.
struct uart_rx {
  struct spinlock lock;
  char buf[UART_RX_BUF_SIZE + 1];
  struct ringbuffer ring;
  uint dropped;        // bytes discarded because the ring was full
  uint overrun;        // hardware FIFO overruns (LSR OE), counted on LSERR
  uint epoch;          // bumped by uart_flush_rx to wake blocked readers
};

struct uart_tx {
  struct spinlock lock;
  char buf[UART_TX_BUF_SIZE + 1];
  struct ringbuffer ring;
};

// Self-describing UART instance: index (derives the register block base from
// uart.c's static table, plus sysctl clock/reset/DMA-select), board pins, DMA
// channels and all runtime RX/TX state.  Filled in by the board layer
// (uart_board.c).
struct uart_controller {
  int index;                               /* 0/1/2 → UART1/2/3，推导基址表 + sysctl 时钟/复位/DMA-select */
  int tx_io, rx_io;                        /* fpioa 引脚号 */
  dmac_channel_number_t chan_tx, chan_rx;  /* DMA 通道（仅 UART1 用） */
  uint32 default_baud;
  uint32 rx_dma_buf[UART_RXDMA_SIZE] __attribute__((aligned(8)));
  uint32 tx_dma_buf[UART_TXDMA_SIZE] __attribute__((aligned(8)));
  struct uart_rx rx;                       /* 各自带独立锁 */
  struct uart_tx tx;
  uint32 requested_baud;
  int rx_mode, tx_mode;
  int rx_dma_active;
};

void uartinit(struct uart_controller *c);
void uartintr(void *ctx);                  // PLIC handler for the UART IRQ
int  uart_read(struct uart_controller *c, char *dst, int n);
int  uart_write(struct uart_controller *c, const char *src, int n);
void uart_set_baud(struct uart_controller *c, int baud);
void uart_get_baud_info(struct uart_controller *c, uint32 *info);
void uart_flush_rx(struct uart_controller *c);
void uart_get_rx_stats(struct uart_controller *c, uint32 *info);
void uart_set_rx_mode(struct uart_controller *c, int mode);
void uart_set_tx_mode(struct uart_controller *c, int mode);
void uart_dma_rx_intr(void *ctx);          // DMA RX completion: harvest + re-arm

#endif
