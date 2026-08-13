// UART byte-stream driver for QEMU 16550A and K210 UARTHS.

#include "memlayout.h"
#include "proc.h"
#include "ringbuffer.h"
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

// RX/TX state, split so rx control flags and tx state stay independent.
// Each backing array is one byte longer than its size because a ringbuffer
// reserves one slot to tell "empty" apart from "full"; ringbuffer_capacity()
// reports the real usable size (32768 / 4096).
struct uart_rx {
  struct spinlock lock;
  char buf[UART_RX_BUF_SIZE + 1];
  struct ringbuffer ring;
  uint dropped;        // bytes discarded because the ring was full
  uint epoch;          // bumped by uart_flush_rx to wake blocked readers
  int cancel_pending;  // observer (Ctrl-C) asked to abort the current read
  uart_rx_observer_t observer;
};

struct uart_tx {
  struct spinlock lock;
  char buf[UART_TX_BUF_SIZE + 1];
  struct ringbuffer ring;
};

static struct uart_rx uart_rx;
static struct uart_tx uart_tx;
static uint32 requested_baud;
extern volatile int panicked;

// ---------- Hardware Operation ----------
// Single-byte, register-level access.  RX and TX primitives stay grouped so
// the two directions read side by side.

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

// RX-enable is driver-internal state: it is toggled around a baud switch and
// at init, never by higher layers.
static void
uart_rxenable(int enabled)
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

// Discard whatever sits in the hardware RX FIFO.  Used on flush and baud
// switch; the soft ring is untouched here.
static void
uart_hw_drain_fifo(void)
{
  while (uart_hw_getc() != -1)
    ;
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
uart_txenable(int enabled)
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

// ---------- UART Init ----------

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

  initlock(&uart_rx.lock, "uartrx");
  initlock(&uart_tx.lock, "uarttx");
  ringbuffer_init(&uart_rx.ring, (uint8 *)uart_rx.buf, UART_RX_BUF_SIZE);
  ringbuffer_init(&uart_tx.ring, (uint8 *)uart_tx.buf, UART_TX_BUF_SIZE);
  requested_baud = 115200;
  uart_txenable(0);
  uart_rxenable(1);
}

// ---------- UART RX ----------

// Drain the hardware FIFO through the observer into the rx ring, count
// bytes dropped when the ring is full, and wake readers if anything landed.
// Caller must hold uart_rx.lock.
static void
uart_rx_service(void)
{
  int received = 0;
  int cancelled = 0;
  int c;

  while ((c = uart_hw_getc()) != -1) {
    if (uart_rx.observer) {
      int action = uart_rx.observer(c);

      if (action == UART_RX_CONSUME_CANCEL) {
        ringbuffer_reset(&uart_rx.ring);
        uart_rx.cancel_pending = 1;
        cancelled = 1;
        received = 0;
        continue;
      }
      if (action == UART_RX_CONSUME)
        continue;
    }

    if (ringbuffer_push(&uart_rx.ring, (uint8)c))
      received = 1;
    else
      uart_rx.dropped++;
  }

  if (received || cancelled)
    wakeup_reason(&uart_rx.ring, WAKEUP_DEVICE);
}

// Block until rx data is available.
static int
uart_rx_wait_data(uint epoch)
{
  struct proc *p = myproc();

  // A pending cancel wins over buffered data: the observer raises it by
  // emptying the ring, so any later byte only arrived before the flag.
  if (uart_rx.cancel_pending) {
    uart_rx.cancel_pending = 0;
    return -1;
  }
  for (;;) {
    if (!ringbuffer_empty(&uart_rx.ring))
      return 1;
    if (p && sleep_interruptible(&uart_rx.ring, &uart_rx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uart_rx.ring, &uart_rx.lock);
    if (uart_rx.cancel_pending) {
      uart_rx.cancel_pending = 0;
      return -1;
    }
    if (epoch != uart_rx.epoch)
      return 0;
  }
}

int
uart_read(char *dst, int n)
{
  int i = 0;
  int reason;
  uint epoch;

  if (n <= 0)
    return 0;

  acquire(&uart_rx.lock);
  epoch = uart_rx.epoch;
  reason = uart_rx_wait_data(epoch);
  if (reason != 1) {
    release(&uart_rx.lock);
    return reason;
  }
  while (i < n && ringbuffer_pop(&uart_rx.ring, (uint8 *)&dst[i]))
    i++;
  release(&uart_rx.lock);
  return i;
}

// Non-blocking poll read of one byte straight from the hardware, bypassing
// the ring.  Returns -1 when no byte is available.
int
uartgetc(void)
{
  return uart_hw_getc();
}

// Discard the hardware FIFO and the soft ring, reset the rx state, then
// wake blocked readers so uart_read returns 0 instead of waiting forever.
void
uart_flush_rx(void)
{
  acquire(&uart_rx.lock);
  uart_hw_drain_fifo();
  ringbuffer_reset(&uart_rx.ring);
  uart_rx.dropped = 0;
  uart_rx.cancel_pending = 0;
  uart_rx.epoch++;
  wakeup_reason(&uart_rx.ring, WAKEUP_DEVICE);
  release(&uart_rx.lock);
}

// info[0] bytes dropped, info[1] bytes buffered, info[2] ring capacity.
void
uart_get_rx_stats(uint32 *info)
{
  acquire(&uart_rx.lock);
  info[0] = uart_rx.dropped;
  info[1] = ringbuffer_used(&uart_rx.ring);
  info[2] = ringbuffer_capacity(&uart_rx.ring);
  release(&uart_rx.lock);
}

void
uart_set_rx_observer(uart_rx_observer_t observer)
{
  uart_rx.observer = observer;
}

// ---------- UART TX ----------

// Drain the tx ring into the hardware, leave the TX interrupt enabled only
// while there is still work to do, and wake writers.  Caller must hold
// uart_tx.lock.
static void
uart_tx_service(void)
{
  uint8 c;

  while (!ringbuffer_empty(&uart_tx.ring) && uart_hw_tx_ready()) {
    ringbuffer_pop(&uart_tx.ring, &c);
    uart_hw_putc(c);
  }

  wakeup_reason(&uart_tx.ring, WAKEUP_DEVICE);
  uart_txenable(!ringbuffer_empty(&uart_tx.ring));
}

// Block until the tx ring has room.  Returns 0 when a slot appeared; -1 if
// the kernel panicked or an interruptible sleep was interrupted, in which
// case the caller should stop writing.  Caller must hold uart_tx.lock.
static int
uart_tx_wait_room(void)
{
  struct proc *p = myproc();

  while (ringbuffer_full(&uart_tx.ring)) {
    if (panicked)
      return -1;
    if (p && sleep_interruptible(&uart_tx.ring, &uart_tx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uart_tx.ring, &uart_tx.lock);
  }
  return 0;
}

int
uart_write(const char *src, int n)
{
  int i;

  acquire(&uart_tx.lock);
  for (i = 0; i < n; i++) {
    if (uart_tx_wait_room() < 0) {
      release(&uart_tx.lock);
      return i > 0 ? i : -1;
    }
    ringbuffer_push(&uart_tx.ring, (uint8)src[i]);
    uart_tx_service();
  }
  release(&uart_tx.lock);
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
  acquire(&uart_tx.lock);
  while (!ringbuffer_empty(&uart_tx.ring)) {
    uart_tx_service();
    if (!ringbuffer_empty(&uart_tx.ring))
      sleep(&uart_tx.ring, &uart_tx.lock);
  }
  release(&uart_tx.lock);
  uart_wait_tx_idle();
}

// ---------- baudrate setup ----------
void
uart_set_baud(int baud)
{
#ifndef QEMU
  uint32 freq;
  uint32 div;

  if (baud <= 0)
    return;

  uart_flush_tx();
  uart_rxenable(0);
  acquire(&uart_rx.lock);
  // Only drain the hardware FIFO: bytes already read into the soft ring
  // belong to the application and must survive the baud switch.
  uart_hw_drain_fifo();
  release(&uart_rx.lock);
  freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  div = freq / (uint32)baud;
  if (div == 0)
    div = 1;
  uarths->div.div = div - 1;
  requested_baud = (uint32)baud;
  uart_rxenable(1);
#else
  (void)baud;
#endif
}

void
uart_get_baud_info(uint32 *info)
{
#ifdef QEMU
  info[0] = requested_baud;
  info[1] = requested_baud;
  info[2] = 0;
  info[3] = 0;
#else
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  uint32 div = uarths->div.div;

  info[0] = requested_baud;
  info[1] = freq / (div + 1);
  info[2] = div;
  info[3] = freq;
#endif
}

// ---------- handler ----------

void
uartintr(void)
{
  acquire(&uart_rx.lock);
  uart_rx_service();
  release(&uart_rx.lock);

  acquire(&uart_tx.lock);
  uart_tx_service();
  release(&uart_tx.lock);
}
