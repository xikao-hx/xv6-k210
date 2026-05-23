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

// Raw receive buffer used by /dev/uart while binary transfers are active.
struct spinlock uart_raw_lock;
#define UART_RAW_RX_BUF_SIZE 32768
static char uart_raw_buf[UART_RAW_RX_BUF_SIZE];
static uint uart_raw_r;
static uint uart_raw_w;
static uint uart_raw_dropped;
static int uart_raw_mode;
static uint32 uart_requested_baud = 115200;

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
  initlock(&uart_raw_lock, "uartraw");
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
// Disable UART receive interrupts while changing raw RX state.
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

void
uart_wait_tx_idle(void)
{
#ifndef QEMU
    uint32 old_txcnt = uarths->txctrl.txcnt;
    uarths->txctrl.txcnt = 1;

    while (!uarths->ip.txwm)
        ;

    uarths->txctrl.txcnt = old_txcnt;

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

void
uart_set_baud(int baud)
{
#ifndef QEMU
    if (baud <= 0)
        return;

    uart_wait_tx_idle();
    uarths->ie.rxwm = 0;
    while (uartgetc() != -1)
        ;

    uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
    uint32 div = freq / (uint32)baud;
    if (div == 0)
        div = 1;
    uarths->div.div = div - 1;
    uart_requested_baud = (uint32)baud;
    uarths->ie.rxwm = 1;
#endif
}

void
uart_get_baud_info(uint32 *info)
{
#ifdef QEMU
  info[0] = uart_requested_baud;
  info[1] = uart_requested_baud;
  info[2] = 0;
  info[3] = 0;
#else
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  uint32 div = uarths->div.div;
  info[0] = uart_requested_baud;
  info[1] = freq / (div + 1);
  info[2] = div;
  info[3] = freq;
#endif
}

static int
uart_raw_input(int c)
{
  acquire(&uart_raw_lock);

  if (!uart_raw_mode) {
    release(&uart_raw_lock);
    return 0;
  }

  uint next = (uart_raw_w + 1) % UART_RAW_RX_BUF_SIZE;
  if (next == uart_raw_r) {
    uart_raw_dropped++;
  } else {
    uart_raw_buf[uart_raw_w] = (char)c;
    uart_raw_w = next;
    wakeup(&uart_raw_r);
  }

  release(&uart_raw_lock);
  return 1;
}

static int
uart_raw_drain_fifo(void)
{
  int did_raw = 0;
  int did_wakeup = 0;

  acquire(&uart_raw_lock);
  if (uart_raw_mode) {
    did_raw = 1;
    while (1) {
      int c = uartgetc();
      if (c == -1)
        break;

      uint next = (uart_raw_w + 1) % UART_RAW_RX_BUF_SIZE;
      if (next == uart_raw_r) {
        uart_raw_dropped++;
      } else {
        uart_raw_buf[uart_raw_w] = (char)c;
        uart_raw_w = next;
        did_wakeup = 1;
      }
    }
  }
  if (did_wakeup)
    wakeup(&uart_raw_r);
  release(&uart_raw_lock);

  return did_raw;
}

void
uart_raw_start(void)
{
  acquire(&uart_raw_lock);
  uart_raw_r = 0;
  uart_raw_w = 0;
  uart_raw_dropped = 0;
  uart_raw_mode = 1;
  release(&uart_raw_lock);
}

void
uart_raw_end(void)
{
  acquire(&uart_raw_lock);
  uart_raw_mode = 0;
  wakeup(&uart_raw_r);
  release(&uart_raw_lock);
}

void
uart_raw_flush(void)
{
  acquire(&uart_raw_lock);
  uart_raw_r = uart_raw_w;
  uart_raw_dropped = 0;
  release(&uart_raw_lock);
}

int
uart_raw_read(char *dst, int n)
{
  int i;

  acquire(&uart_raw_lock);
  for (i = 0; i < n; i++) {
    while (uart_raw_r == uart_raw_w) {
      if (!uart_raw_mode) {
        release(&uart_raw_lock);
        return i > 0 ? i : -1;
      }
      if (myproc() && myproc()->killed) {
        release(&uart_raw_lock);
        return -1;
      }
      sleep(&uart_raw_r, &uart_raw_lock);
    }

    dst[i] = uart_raw_buf[uart_raw_r];
    uart_raw_r = (uart_raw_r + 1) % UART_RAW_RX_BUF_SIZE;
  }
  release(&uart_raw_lock);

  return i;
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
  if (!uart_raw_drain_fifo()) {
    while(1){
      int c = uartgetc();
      if(c == -1)
        break;
      if(!uart_raw_input(c))
        consoleintr(c);
    }
  }

  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);
}
