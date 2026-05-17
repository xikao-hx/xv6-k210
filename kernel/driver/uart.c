//
// Console I/O via SBI ecalls. OpenSBI (M-mode) manages the UART hardware.
// The K210 uses a SiFive UART which is incompatible with the 16550a register
// interface, so all direct MMIO UART access is replaced with SBI calls.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sbi.h"

// the transmit output buffer.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
int uart_tx_w; // write next to uart_tx_buf[uart_tx_w++]
int uart_tx_r; // read next from uart_tx_buf[uar_tx_r++]

extern volatile int panicked; // from printf.c

void uartstart();

void
uartinit(void)
{
  // OpenSBI already initialized the UART (baud rate, tx/rx enable, FIFO).
  // However, OpenSBI disables all UART interrupts (IER=0).
  // Re-enable receive interrupts so uartintr() gets called on input.
  uart_tx_w = 0;
  uart_tx_r = 0;
  initlock(&uart_tx_lock, "uart");

#ifdef QEMU
  // ns16550a: IER offset 1, bit 0 = Received Data Available interrupt
  *(volatile uint8*)(UART0 + 1) = 0x01;
#else
  // SiFive UART: IE register at offset 0x10, bit 1 = RXWM (receive watermark)
  *(volatile uint32*)(UART0 + 0x10) = 0x02;
#endif
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void
uartputc(int c)
{
  acquire(&uart_tx_lock);

  if(panicked){
    for(;;)
      ;
  }

  while(1){
    if(((uart_tx_w + 1) % UART_TX_BUF_SIZE) == uart_tx_r){
      // buffer is full.
      // wait for uartstart() to open up space in the buffer.
      sleep(&uart_tx_r, &uart_tx_lock);
    } else {
      uart_tx_buf[uart_tx_w] = c;
      uart_tx_w = (uart_tx_w + 1) % UART_TX_BUF_SIZE;
      uartstart();
      release(&uart_tx_lock);
      return;
    }
  }
}

// alternate version of uartputc() that doesn't
// use interrupts, for use by kernel printf() and
// to echo characters. Uses SBI ecall instead of
// direct MMIO to avoid SiFive/16550a incompatibility.
void
uartputc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  sbi_console_putchar(c);

  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it via SBI.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uartstart()
{
  while(1){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      return;
    }

    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;

    // maybe uartputc() is waiting for space in the buffer.
    wakeup(&uart_tx_r);

    sbi_console_putchar(c);
  }
}

// read one input character from the UART via SBI.
// return -1 if none is waiting.
int
uartgetc(void)
{
  int c = sbi_console_getchar();
  return c;
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from trap.c.
void
uartintr(void)
{
  // read and process incoming characters.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }

  // send buffered characters.
  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);
}
