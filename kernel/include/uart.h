#ifndef __UART_H
#define __UART_H

#include "dmac.h"
#include "ringbuffer.h"
#include "spinlock.h"
#include "types.h"
#include "uart-dw.h"

#define UART_MODE_INT 0
#define UART_MODE_DMA 1
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

struct uart_controller {
  int index;                               
  int tx_io, rx_io;                        
  dmac_channel_number_t chan_tx;  
  dmac_channel_number_t chan_rx;  
  uint32 default_baud;
  uint32 rx_dma_buf[UART_RXDMA_SIZE] __attribute__((aligned(8)));
  uint32 tx_dma_buf[UART_TXDMA_SIZE] __attribute__((aligned(8)));
  struct uart_rx rx;                       
  struct uart_tx tx;
  uint32 requested_baud;
  int rx_mode;
  int tx_mode;
  int rx_dma_active;
};

void uartinit(struct uart_controller *c);
int  uart_read(struct uart_controller *c, char *dst, int n);
int  uart_write(struct uart_controller *c, const char *src, int n);
void uart_set_baud(struct uart_controller *c, int baud);
void uart_get_baud_info(struct uart_controller *c, uint32 *info);
void uart_flush_rx(struct uart_controller *c);
void uart_get_rx_stats(struct uart_controller *c, uint32 *info);
void uart_set_rx_mode(struct uart_controller *c, int mode);
void uart_set_tx_mode(struct uart_controller *c, int mode);

#endif
