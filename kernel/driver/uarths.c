//
// Unified UART driver
//
// Supports both:
//   - QEMU virt machine (16550A UART at 0x10000000)
//   - K210 hardware    (UART HS at 0x38000000)
//
// Provides the standard xv6 uart*() API declared in defs.h.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#ifdef QEMU

// 16550A UART registers (memory-mapped)
#define Reg(reg)     ((volatile unsigned char *)(UART0 + reg))

#define RHR          0  // receive holding register
#define THR          0  // transmit holding register
#define IER          1  // interrupt enable register
#define IER_TX_ENABLE  (1<<0)
#define IER_RX_ENABLE  (1<<1)
#define FCR          2  // FIFO control register
#define FCR_FIFO_ENABLE (1<<0)
#define FCR_FIFO_CLEAR  (3<<1)
#define ISR          2  // interrupt status register
#define LCR          3  // line control register
#define LCR_EIGHT_BITS  (3<<0)
#define LCR_BAUD_LATCH  (1<<7)
#define LSR          5  // line status register
#define LSR_RX_READY (1<<0)
#define LSR_TX_IDLE  (1<<5)

#define ReadReg(reg)     (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

#else /* K210 */

#include "sysctl.h"
#include "uarths.h"

volatile uarths_t *const uarths = (volatile uarths_t *)UART0_V;

#endif

// Transmit output buffer with interrupt-driven flow control.
struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
int uart_tx_w; // write next to uart_tx_buf[uart_tx_w++]
int uart_tx_r; // read next from uart_tx_buf[uart_tx_r++]

extern volatile int panicked; // from printf.c

void uartstart();

void
uartinit(void)
{
#ifdef QEMU
  // Disable interrupts.
  WriteReg(IER, 0x00);

  // Set baud-rate-division mode.
  WriteReg(LCR, LCR_BAUD_LATCH);

  // LSB for baud rate of 38.4K.
  WriteReg(0, 0x03);

  // MSB for baud rate of 38.4K.
  WriteReg(1, 0x00);

  // Leave set-baud mode; 8-bit word, no parity.
  WriteReg(LCR, LCR_EIGHT_BITS);

  // Reset and enable FIFOs.
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);

  // Enable transmit and receive interrupts.
  WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
#else
    uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    uint16 div = freq / 115200 - 1;

    /* Set UART registers */
    uarths->div.div = div;
    uarths->txctrl.txen = 1;
    uarths->rxctrl.rxen = 1;
    uarths->txctrl.txcnt = 0;
    uarths->rxctrl.rxcnt = 0;
    uarths->ip.txwm = 1;
    uarths->ip.rxwm = 1;
    uarths->ie.txwm = 0;
    uarths->ie.rxwm = 1;
#endif

  initlock(&uart_tx_lock, "uart");
}

//
// Buffered putc (for write() syscall — may sleep).
//
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
      // Buffer full — wait for uartstart() to drain.
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

//
// Synchronous putc (for printf / consputc — spins until sent).
// Does NOT use interrupts, so it's safe to call from contexts
// where sleeping is not allowed.
//
void
uartputc_sync(int c)
{
    push_off();

    if(panicked){
    for(;;)
        ;
    }

#ifdef QEMU
    while((ReadReg(LSR) & LSR_TX_IDLE) == 0)
    ;
    WriteReg(THR, c);
#else
    while(uarths->txdata.full)
    ;
    uarths->txdata.data = (uint8)c;
#endif

  pop_off();
}

//
// Start transmission of buffered characters.
// Caller must hold uart_tx_lock.
//
void
uartstart()
{
  while(1){
    if(uart_tx_w == uart_tx_r){
      // Transmit buffer empty.
      return;
    }

#ifdef QEMU
    if((ReadReg(LSR) & LSR_TX_IDLE) == 0){
      // UART not ready for another byte; it will interrupt.
      return;
    }
    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;
    wakeup(&uart_tx_r);
    WriteReg(THR, c);
#else
    if (uarths->txdata.full) {
        return;
    }
    int c = uart_tx_buf[uart_tx_r];
    uart_tx_r = (uart_tx_r + 1) % UART_TX_BUF_SIZE;
    wakeup(&uart_tx_r);
    uarths->txdata.data = (uint8)c;
#endif
  }
}

//
// Disable UART receive interrupts (for raw polling mode).
//
void
uartrx_disable(void)
{
#ifdef QEMU
    WriteReg(IER, IER_TX_ENABLE);
#else
    uarths->ie.rxwm = 0;
#endif
}

//
// Re-enable UART receive interrupts after uartrx_disable().
//
void
uartrx_enable(void)
{
#ifdef QEMU
    WriteReg(IER, IER_TX_ENABLE | IER_RX_ENABLE);
#else
    uarths->ie.rxwm = 1;
#endif
}

//
// Wait for the UART TX to finish transmitting all buffered data.
// Necessary before changing baud rate to avoid corrupting the byte
// currently being shifted out.
//
void
uart_wait_tx_idle(void)
{
#ifndef QEMU
    // Set TX watermark to 1 so ip.txwm fires when FIFO becomes empty
    uint32 old_txcnt = uarths->txctrl.txcnt;
    uarths->txctrl.txcnt = 1;

    // Spin until the TX FIFO drains (all bytes moved to shift register)
    while (!uarths->ip.txwm)
        ;

    uarths->txctrl.txcnt = old_txcnt;

    // The shift register may still be transmitting the last byte.
    // Calculate wait time for 2 byte-times (20 bits) at the current baud rate.
    uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    uint32 cur_div = uarths->div.div;
    uint32 baud = freq / (cur_div + 1);
    uint32 ncycles = (20UL * freq) / baud + 1000;

    if (ncycles > 500000)
        ncycles = 500000;

    for (volatile uint32 i = 0; i < ncycles; i++)
        ;
#endif
}

//
// Set UART baud rate.
//
void
uart_set_baud(int baud)
{
#ifndef QEMU
    // Complete any in-flight transmission before changing divisor
    uart_wait_tx_idle();

    uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    uint16 div = freq / baud - 1;
    uarths->div.div = div;
#endif
}

//
// Read one input character.
// Return -1 if none is waiting.
//
int
uartgetc(void)
{
#ifdef QEMU
    if(ReadReg(LSR) & 0x01)
        return ReadReg(RHR);
    else
        return -1;
#else
    uarths_rxdata_t recv = uarths->rxdata;

    if(recv.empty)
        return -1;
    else
        return (recv.data & 0xff);
#endif
}

//
// UART interrupt handler.
// Called from trap.c on UART0_IRQ.
//
void
uartintr(void)
{
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }

  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);
}
