#ifndef __UART_H
#define __UART_H

#include "dmac.h"
#include "ringbuffer.h"
#include "spinlock.h"
#include "types.h"
#include "uart-dw.h"

// RX/TX operating modes, the arg to UART_IOCTL_SET_*_MODE and what
#define UART_MODE_INT 0
#define UART_MODE_DMA 1

// uart1-specific ioctl codes.  Numerically distinct from CONSOLE_IOCTL_*
// (0x01-0x08); uartdev.c routes them to uart_set_rx_mode/uart_set_tx_mode.
#define UART_IOCTL_SET_RX_MODE 0x21
#define UART_IOCTL_SET_TX_MODE 0x22

// Per-instance soft ring sizes.  Each buf is size+1: the ring reserves one
// slot to tell "empty" from "full"; ringbuffer_capacity() is the usable size.
#define UART_RX_BUF_SIZE 32768
#define UART_TX_BUF_SIZE 4096

// DMA per-transfer chunk (32-bit words, 1 byte/word, as the SDK).  RX re-arms
// RXDMA_SIZE on each boundary; TX writes > TXDMA_SIZE split into blocks.
// RXDMA_SIZE 1024 was tried: it made 1.5M RX errors worse.  A 527-byte burn
// frame never fills a 1024-byte block, so every frame was harvested by
// high-frequency RDA interrupts (dar mid-transfer, racing the DMA-done ISR)
// instead of one clean done.  Back at 512: one done (512 bytes) + one CTI
// (the 15-byte frame tail) per frame, and RDA harvesting is disabled in
// DMA mode (see uartintr) so RDA and done never race.
#define UART_RXDMA_SIZE 512
#define UART_TXDMA_SIZE 256

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

// One instance of the DW UART.  Hardware identity (base, fpioa pins, DMA
// channels, sysctl index) plus all runtime state live here, so a single
// driver serves UART1/2/3 with one struct per instance.  The board layer
// (kernel/board/uart_board.c) builds the instances; the driver is
// parameterized purely by `struct uart_controller *`.
struct uart_controller {
  volatile uart_t *hw;                     // instance register block base
  int index;                               // 0/1/2 -> UART1/2/3 (sysctl ids)
  int tx_io, rx_io;                        // fpioa pins
  dmac_channel_number_t chan_tx, chan_rx;  // DMA channels (UART1 only today)
  uint32 default_baud;
  uint32 rx_dma_buf[UART_RXDMA_SIZE] __attribute__((aligned(8)));
  uint32 tx_dma_buf[UART_TXDMA_SIZE] __attribute__((aligned(8)));
  struct uart_rx rx;
  struct uart_tx tx;
  uint32 requested_baud;
  int rx_mode, tx_mode;
  int rx_dma_active;                       // chan_rx currently armed
};

void uartinit(struct uart_controller *c);
void uartintr(void *ctx);                  // irq handler; ctx = uart_controller
void uart_dma_rx_intr(void *ctx);          // DMA RX completion: harvest + re-arm
int  uart_read(struct uart_controller *c, char *dst, int n);
int  uart_write(struct uart_controller *c, const char *src, int n);
void uart_set_baud(struct uart_controller *c, int baud);
void uart_get_baud_info(struct uart_controller *c, uint32 *info);
void uart_flush_rx(struct uart_controller *c);
void uart_get_rx_stats(struct uart_controller *c, uint32 *info);
void uart_set_rx_mode(struct uart_controller *c, int mode);
void uart_set_tx_mode(struct uart_controller *c, int mode);

#endif
