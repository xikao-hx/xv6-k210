// UART byte-stream driver for QEMU 16550A and K210 UARTHS.

#include "memlayout.h"
#include "proc.h"
#include "signal.h"
#include "uarths.h"

#ifdef QEMU

#define Reg(reg)     ((volatile unsigned char *)(UART0 + reg))
#define RHR          0
#define THR          0
#define IER          1
#define IER_RX_ENABLE  (1 << 0)
#define IER_TX_ENABLE  (1 << 1)
#define FCR          2
#define FCR_FIFO_ENABLE (1 << 0)
#define FCR_FIFO_CLEAR  (3 << 1)
#define LCR          3
#define LCR_EIGHT_BITS  (3 << 0)
#define LCR_BAUD_LATCH  (1 << 7)
#define LSR          5
#define LSR_RX_READY (1 << 0)
#define LSR_TX_IDLE  (1 << 5)

#define ReadReg(reg)     (*(Reg(reg)))
#define WriteReg(reg, v) (*(Reg(reg)) = (v))

#else

#include "sysctl.h"

volatile uarths_t *const uarths = (volatile uarths_t *)UART0_V;

#endif

#define UART_RX_BUF_SIZE 32768
#define UART_TX_BUF_SIZE 4096

struct uart_state {
  struct spinlock rx_lock;
  struct spinlock tx_lock;
  char rx_buf[UART_RX_BUF_SIZE];
  char tx_buf[UART_TX_BUF_SIZE];
  uint rx_r;
  uint rx_w;
  uint tx_r;
  uint tx_w;
  uint rx_dropped;
  uint rx_epoch;
  int rx_cancel_pending;
  uint32 requested_baud;
  uart_rx_observer_t rx_observer;
};

static struct uart_state uart;

extern volatile int panicked;

void
uart_set_rx_observer(uart_rx_observer_t observer)
{
  uart.rx_observer = observer;
}

static int
uart_hw_getc(void)
{
#ifdef QEMU
  if ((ReadReg(LSR) & LSR_RX_READY) == 0)
    return -1;
  return ReadReg(RHR);
#else
  uarths_rxdata_t recv = uarths->rxdata;

  if (recv.empty)
    return -1;
  return recv.data & 0xff;
#endif
}

static int
uart_hw_tx_ready(void)
{
#ifdef QEMU
  return (ReadReg(LSR) & LSR_TX_IDLE) != 0;
#else
  return !uarths->txdata.full;
#endif
}

static void
uart_hw_putc(int c)
{
#ifdef QEMU
  WriteReg(THR, c);
#else
  uarths->txdata.data = (uint8)c;
#endif
}

static void
uart_hw_set_rx_interrupt(int enabled)
{
#ifdef QEMU
  unsigned char ier = ReadReg(IER);

  if (enabled)
    ier |= IER_RX_ENABLE;
  else
    ier &= ~IER_RX_ENABLE;
  WriteReg(IER, ier);
#else
  uarths->ie.rxwm = enabled;
#endif
}

static void
uart_hw_set_tx_interrupt(int enabled)
{
#ifdef QEMU
  unsigned char ier = ReadReg(IER);

  if (enabled)
    ier |= IER_TX_ENABLE;
  else
    ier &= ~IER_TX_ENABLE;
  WriteReg(IER, ier);
#else
  uarths->ie.txwm = enabled;
#endif
}

static void
uartstart(void)
{
  while (uart.tx_r != uart.tx_w && uart_hw_tx_ready()) {
    int c = uart.tx_buf[uart.tx_r];
    uart.tx_r = (uart.tx_r + 1) % UART_TX_BUF_SIZE;
    uart_hw_putc(c);
  }

  wakeup_reason(&uart.tx_r, WAKEUP_DEVICE);
  uart_hw_set_tx_interrupt(uart.tx_r != uart.tx_w);
}

void
uartinit(void)
{
#ifdef QEMU
  WriteReg(IER, 0);
  WriteReg(LCR, LCR_BAUD_LATCH);
  WriteReg(0, 3);
  WriteReg(1, 0);
  WriteReg(LCR, LCR_EIGHT_BITS);
  WriteReg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);
#else
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  uint16 div = freq / 115200 - 1;

  uarths->div.div = div;
  uarths->txctrl.txen = 1;
  uarths->rxctrl.rxen = 1;
  // txwm is asserted when the FIFO count is less than txcnt. A threshold
  // of zero can never fire, leaving buffered output stalled until an RX IRQ.
  uarths->txctrl.txcnt = 1;
  uarths->rxctrl.rxcnt = 0;
  uarths->ip.txwm = 1;
  uarths->ip.rxwm = 1;
#endif

  initlock(&uart.rx_lock, "uartrx");
  initlock(&uart.tx_lock, "uarttx");
  uart.requested_baud = 115200;
  uart_hw_set_tx_interrupt(0);
  uart_hw_set_rx_interrupt(1);
}

int
uart_write(const char *src, int n)
{
  int i;
  struct proc *p = myproc();

  acquire(&uart.tx_lock);
  for (i = 0; i < n; i++) {
    while (((uart.tx_w + 1) % UART_TX_BUF_SIZE) == uart.tx_r) {
      if (panicked) {
        release(&uart.tx_lock);
        return i > 0 ? i : -1;
      }
      if (p && sleep_interruptible(&uart.tx_r, &uart.tx_lock) < 0) {
        release(&uart.tx_lock);
        return i > 0 ? i : -1;
      }
      if (!p)
        sleep(&uart.tx_r, &uart.tx_lock);
    }

    uart.tx_buf[uart.tx_w] = src[i];
    uart.tx_w = (uart.tx_w + 1) % UART_TX_BUF_SIZE;
    uartstart();
  }
  release(&uart.tx_lock);
  return i;
}

void
uartputc(int c)
{
  char ch = c;
  uart_write(&ch, 1);
}

void
uartputc_sync(int c)
{
  push_off();
  if (panicked)
    for (;;)
      ;
  while (!uart_hw_tx_ready())
    ;
  uart_hw_putc(c);
  pop_off();
}

void
uart_wait_tx_idle(void)
{
#ifdef QEMU
  while (!uart_hw_tx_ready())
    ;
#else
  uint32 old_txcnt = uarths->txctrl.txcnt;
  uint32 freq;
  uint32 baud;
  uint32 ncycles;

  uarths->txctrl.txcnt = 1;
  while (!uarths->ip.txwm)
    ;
  uarths->txctrl.txcnt = old_txcnt;

  freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  baud = freq / (uarths->div.div + 1);
  ncycles = (20UL * freq) / baud + 1000;
  if (ncycles > 500000)
    ncycles = 500000;
  for (volatile uint32 i = 0; i < ncycles; i++)
    ;
#endif
}

void
uart_flush_tx(void)
{
  acquire(&uart.tx_lock);
  while (uart.tx_r != uart.tx_w) {
    uartstart();
    if (uart.tx_r != uart.tx_w)
      sleep(&uart.tx_r, &uart.tx_lock);
  }
  release(&uart.tx_lock);
  uart_wait_tx_idle();
}

int
uart_read(char *dst, int n)
{
  int i = 0;
  uint epoch;
  struct proc *p = myproc();

  if (n <= 0)
    return 0;

  acquire(&uart.rx_lock);
  epoch = uart.rx_epoch;
  if(uart.rx_cancel_pending) {
    uart.rx_cancel_pending = 0;
    release(&uart.rx_lock);
    return -1;
  }
  while (uart.rx_r == uart.rx_w) {
    if (p && signal_pending(p)) {
      release(&uart.rx_lock);
      return -1;
    }
    if (p && sleep_interruptible(&uart.rx_r, &uart.rx_lock) < 0) {
      release(&uart.rx_lock);
      return -1;
    }
    if (!p)
      sleep(&uart.rx_r, &uart.rx_lock);
    if(uart.rx_cancel_pending) {
      uart.rx_cancel_pending = 0;
      release(&uart.rx_lock);
      return -1;
    }
    if (epoch != uart.rx_epoch) {
      release(&uart.rx_lock);
      return 0;
    }
  }

  while (i < n && uart.rx_r != uart.rx_w) {
    dst[i++] = uart.rx_buf[uart.rx_r];
    uart.rx_r = (uart.rx_r + 1) % UART_RX_BUF_SIZE;
  }
  release(&uart.rx_lock);
  return i;
}

int
uart_try_read(char *dst, int n)
{
  int i = 0;

  acquire(&uart.rx_lock);
  while (i < n && uart.rx_r != uart.rx_w) {
    dst[i++] = uart.rx_buf[uart.rx_r];
    uart.rx_r = (uart.rx_r + 1) % UART_RX_BUF_SIZE;
  }
  release(&uart.rx_lock);
  return i;
}

void
uartrx_disable(void)
{
  uart_hw_set_rx_interrupt(0);
}

void
uartrx_enable(void)
{
  uart_hw_set_rx_interrupt(1);
}

void
uart_flush_rx(void)
{
  acquire(&uart.rx_lock);
  while (uart_hw_getc() != -1)
    ;
  uart.rx_r = uart.rx_w = 0;
  uart.rx_dropped = 0;
  uart.rx_cancel_pending = 0;
  uart.rx_epoch++;
  wakeup_reason(&uart.rx_r, WAKEUP_DEVICE);
  release(&uart.rx_lock);
}

void
uart_get_rx_stats(uint32 *info)
{
  acquire(&uart.rx_lock);
  info[0] = uart.rx_dropped;
  if (uart.rx_w >= uart.rx_r)
    info[1] = uart.rx_w - uart.rx_r;
  else
    info[1] = UART_RX_BUF_SIZE - uart.rx_r + uart.rx_w;
  info[2] = UART_RX_BUF_SIZE - 1;
  release(&uart.rx_lock);
}

void
uart_set_baud(int baud)
{
#ifndef QEMU
  uint32 freq;
  uint32 div;

  if (baud <= 0)
    return;

  uart_flush_tx();
  uartrx_disable();
  acquire(&uart.rx_lock);
  while (uart_hw_getc() != -1)
    ;
  release(&uart.rx_lock);
  freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  div = freq / (uint32)baud;
  if (div == 0)
    div = 1;
  uarths->div.div = div - 1;
  uart.requested_baud = (uint32)baud;
  uartrx_enable();
#else
  (void)baud;
#endif
}

void
uart_get_baud_info(uint32 *info)
{
#ifdef QEMU
  info[0] = uart.requested_baud;
  info[1] = uart.requested_baud;
  info[2] = 0;
  info[3] = 0;
#else
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  uint32 div = uarths->div.div;

  info[0] = uart.requested_baud;
  info[1] = freq / (div + 1);
  info[2] = div;
  info[3] = freq;
#endif
}

int
uartgetc(void)
{
  return uart_hw_getc();
}

void
uartintr(void)
{
  int cancelled = 0;
  int received = 0;
  int c;

  acquire(&uart.rx_lock);
  while ((c = uart_hw_getc()) != -1) {
    if (uart.rx_observer) {
      int action = uart.rx_observer(c);

      if (action == UART_RX_CONSUME_CANCEL) {
        uart.rx_r = uart.rx_w;
        uart.rx_cancel_pending = 1;
        cancelled = 1;
        received = 0;
        continue;
      }
      if (action == UART_RX_CONSUME)
        continue;
    }
    uint next = (uart.rx_w + 1) % UART_RX_BUF_SIZE;
    if (next == uart.rx_r)
      uart.rx_dropped++;
    else {
      uart.rx_buf[uart.rx_w] = c;
      uart.rx_w = next;
      received = 1;
    }
  }
  if (received || cancelled)
    wakeup_reason(&uart.rx_r, WAKEUP_DEVICE);
  release(&uart.rx_lock);

  acquire(&uart.tx_lock);
  uartstart();
  release(&uart.tx_lock);
}
