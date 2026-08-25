// UART byte-stream driver for QEMU 16550A and K210 UARTHS.

#include "irq.h"
#include "memlayout.h"
#include "plic.h"
#include "proc.h"
#include "ringbuffer.h"
#include "uarths.h"
#include "uarths-dw.h"

#ifdef QEMU

#define Reg(reg)     ((volatile unsigned char *)(UARTHS_V + reg))
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

volatile uarths_t *const uarths = (volatile uarths_t *)UARTHS_V;

#endif

#define UARTHS_RX_BUF_SIZE 32768
#define UARTHS_TX_BUF_SIZE 4096

struct uarths_rx {
  struct spinlock lock;
  char buf[UARTHS_RX_BUF_SIZE + 1];
  struct ringbuffer ring;
  uint dropped;        // bytes discarded because the ring was full
  uint epoch;          // bumped by uarths_flush_rx to wake blocked readers
  uarths_rx_observer_t observer;
};

struct uarths_tx {
  struct spinlock lock;
  char buf[UARTHS_TX_BUF_SIZE + 1];
  struct ringbuffer ring;
};

static struct uarths_rx uarths_rx;
static struct uarths_tx uarths_tx;
static uint32 requested_baud;
extern volatile int panicked;

// ---------- Hardware Operation ----------
// Single-byte, register-level access.  RX and TX primitives stay grouped so
// the two directions read side by side.

static int
uarths_hw_getc(void)
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
uarths_rxenable(int enabled)
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
uarths_hw_drain_fifo(void)
{
  while (uarths_hw_getc() != -1)
    ;
}

static int
uarths_hw_tx_ready(void)
{
#ifdef QEMU
  return (ReadReg(LSR) & LSR_TX_IDLE) != 0;
#else
  return !uarths->txdata.full;
#endif
}

static void
uarths_hw_putc(int c)
{
#ifdef QEMU
  WriteReg(THR, c);
#else
  uarths->txdata.data = (uint8)c;
#endif
}

static void
uarths_txenable(int enabled)
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

void uarths_dw_isr(void *data);
void
uarthsinit(void)
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

  initlock(&uarths_rx.lock, "uarthsrx");
  initlock(&uarths_tx.lock, "uarthstx");
  ringbuffer_init(&uarths_rx.ring, (uint8 *)uarths_rx.buf, UARTHS_RX_BUF_SIZE);
  ringbuffer_init(&uarths_tx.ring, (uint8 *)uarths_tx.buf, UARTHS_TX_BUF_SIZE);
  requested_baud = 115200;
  uarths_txenable(0);
  uarths_rxenable(1);
  irq_register(UARTHS_IRQ, uarths_dw_isr, 0);
}

// ---------- UART RX ----------

// Drain the hardware FIFO through the observer into the rx ring, count
// bytes dropped when the ring is full, and wake readers if anything landed.
// Caller must hold uarths_rx.lock.
static void
uarths_rx_service(void)
{
  int received = 0;
  int c;

  while ((c = uarths_hw_getc()) != -1) {
    if (uarths_rx.observer) {
      int action = uarths_rx.observer(c);

      if (action == UARTHS_RX_CONSUME)
        continue;
    }

    if (ringbuffer_push(&uarths_rx.ring, (uint8)c))
      received = 1;
    else
      uarths_rx.dropped++;
  }

  if (received)
    wakeup_reason(&uarths_rx.ring, WAKEUP_DEVICE);
}

// Block until rx data is available.
static int
uarths_rx_wait_data(uint epoch)
{
  struct proc *p = myproc();

  for (;;) {
    if (!ringbuffer_empty(&uarths_rx.ring))
      return 1;
    if (p && sleep_interruptible(&uarths_rx.ring, &uarths_rx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uarths_rx.ring, &uarths_rx.lock);
    if (epoch != uarths_rx.epoch)
      return 0;
  }
}

int
uarths_read(char *dst, int n)
{
  int i = 0;
  int reason;
  uint epoch;

  if (n <= 0)
    return 0;

  acquire(&uarths_rx.lock);
  epoch = uarths_rx.epoch;
  reason = uarths_rx_wait_data(epoch);
  if (reason != 1) {
    release(&uarths_rx.lock);
    return reason;
  }
  while (i < n && ringbuffer_pop(&uarths_rx.ring, (uint8 *)&dst[i]))
    i++;
  release(&uarths_rx.lock);
  return i;
}

// Non-blocking poll read of one byte straight from the hardware, bypassing
// the ring.  Returns -1 when no byte is available.
int
uarthsgetc(void)
{
  return uarths_hw_getc();
}

// Discard the hardware FIFO and the soft ring, reset the rx state, then
// wake blocked readers so uarths_read returns 0 instead of waiting forever.
void
uarths_flush_rx(void)
{
  acquire(&uarths_rx.lock);
  uarths_hw_drain_fifo();
  ringbuffer_reset(&uarths_rx.ring);
  uarths_rx.dropped = 0;
  uarths_rx.epoch++;
  wakeup_reason(&uarths_rx.ring, WAKEUP_DEVICE);
  release(&uarths_rx.lock);
}

// info[0] bytes dropped, info[1] bytes buffered, info[2] ring capacity.
void
uarths_get_rx_stats(uint32 *info)
{
  acquire(&uarths_rx.lock);
  info[0] = uarths_rx.dropped;
  info[1] = ringbuffer_used(&uarths_rx.ring);
  info[2] = ringbuffer_capacity(&uarths_rx.ring);
  release(&uarths_rx.lock);
}

void
uarths_set_rx_observer(uarths_rx_observer_t observer)
{
  uarths_rx.observer = observer;
}

// ---------- UART TX ----------

// Drain the tx ring into the hardware, leave the TX interrupt enabled only
// while there is still work to do, and wake writers.  Caller must hold
// uarths_tx.lock.
static void
uarths_tx_service(void)
{
  uint8 c;

  while (!ringbuffer_empty(&uarths_tx.ring) && uarths_hw_tx_ready()) {
    ringbuffer_pop(&uarths_tx.ring, &c);
    uarths_hw_putc(c);
  }

  wakeup_reason(&uarths_tx.ring, WAKEUP_DEVICE);
  uarths_txenable(!ringbuffer_empty(&uarths_tx.ring));
}

// Block until the tx ring has room.  Returns 0 when a slot appeared; -1 if
// the kernel panicked or an interruptible sleep was interrupted, in which
// case the caller should stop writing.  Caller must hold uarths_tx.lock.
static int
uarths_tx_wait_room(void)
{
  struct proc *p = myproc();

  while (ringbuffer_full(&uarths_tx.ring)) {
    if (panicked)
      return -1;
    if (p && sleep_interruptible(&uarths_tx.ring, &uarths_tx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uarths_tx.ring, &uarths_tx.lock);
  }
  return 0;
}

int
uarths_write(const char *src, int n)
{
  int i;

  acquire(&uarths_tx.lock);
  for (i = 0; i < n; i++) {
    if (uarths_tx_wait_room() < 0) {
      release(&uarths_tx.lock);
      return i > 0 ? i : -1;
    }
    ringbuffer_push(&uarths_tx.ring, (uint8)src[i]);
    uarths_tx_service();
  }
  release(&uarths_tx.lock);
  return i;
}

void
uarthsputc(int c)
{
  char ch = c;
  uarths_write(&ch, 1);
}

void
uarthsputc_sync(int c)
{
  push_off();
  if (panicked)
    for (;;)
      ;
  while (!uarths_hw_tx_ready())
    ;
  uarths_hw_putc(c);
  pop_off();
}

void
uarths_wait_tx_idle(void)
{
#ifdef QEMU
  while (!uarths_hw_tx_ready())
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
uarths_flush_tx(void)
{
  acquire(&uarths_tx.lock);
  while (!ringbuffer_empty(&uarths_tx.ring)) {
    uarths_tx_service();
    if (!ringbuffer_empty(&uarths_tx.ring))
      sleep(&uarths_tx.ring, &uarths_tx.lock);
  }
  release(&uarths_tx.lock);
  uarths_wait_tx_idle();
}

// ---------- baudrate setup ----------
void
uarths_set_baud(int baud)
{
#ifndef QEMU
  uint32 freq;
  uint32 div;

  if (baud <= 0)
    return;

  uarths_flush_tx();
  uarths_rxenable(0);
  acquire(&uarths_rx.lock);
  // Only drain the hardware FIFO: bytes already read into the soft ring
  // belong to the application and must survive the baud switch.
  uarths_hw_drain_fifo();
  release(&uarths_rx.lock);
  freq = sysctl_clock_get_freq(SYSCTL_CLOCK_CPU);
  div = freq / (uint32)baud;
  if (div == 0)
    div = 1;
  uarths->div.div = div - 1;
  requested_baud = (uint32)baud;
  uarths_rxenable(1);
#else
  (void)baud;
#endif
}

void
uarths_get_baud_info(uint32 *info)
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
uarths_dw_isr(void *data)
{
  (void)data;

  acquire(&uarths_rx.lock);
  uarths_rx_service();
  release(&uarths_rx.lock);

  acquire(&uarths_tx.lock);
  uarths_tx_service();
  release(&uarths_tx.lock);
}
