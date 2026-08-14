// DW APB UART1/2/3 byte-stream driver, multi-instance.
//
// Each UART instance is a struct uart_controller (kernel/include/uart.h) built
// by the board layer (kernel/board/uart_board.c).  Every driver function is
// parameterized by `struct uart_controller *c`; there are no file-static
// driver globals, so the same code serves UART1/2/3 with one struct per
// instance.  IRQ delivery goes through the irq registration table: uartinit()
// registers its device IRQ (UART0_IRQ + index, i.e. 11/12/13) with uartintr,
// and UART1 additionally owns the shared DMAC IRQ (32) for RX DMA.

#include "dmac.h"
#include "fpioa.h"
#include "irq.h"
#include "memlayout.h"
#include "printf.h"
#include "proc.h"
#include "ringbuffer.h"
#include "sysctl.h"
#include "uart.h"
#include "uart-dw.h"

#define UART_LSR_DR      (1u << 0)
#define UART_LSR_OE      (1u << 1)  // overrun error (FIFO overflowed)
#define UART_LSR_TX_BUSY (1u << 5)
#define UART_LSR_TEMT    (1u << 6)

#define UART_IIR_THRE    0x02
#define UART_IIR_RDA     0x04
#define UART_IIR_LSERR   0x06
#define UART_IIR_TIMEOUT 0x0c

#define UART_IER_RX      0x01
#define UART_IER_TX      0x02

#define UART_BRATE_CONST 16

// dmac_wait_done(CH4) spin budget; ~1s at 400MHz, only guards a wedged line.
#define UART_DMA_TIMEOUT 100000000UL

extern volatile int panicked;

// ---------- Hardware Operation ----------

static int
uart_hw_getc(struct uart_controller *c)
{
  if (!(c->hw->LSR & UART_LSR_DR))
    return -1;
  return c->hw->RBR & 0xff;
}

static int
uart_hw_tx_ready(struct uart_controller *c)
{
  // K210 inverts LSR bit 5: 1 = busy, 0 = ready to accept a byte.
  return !(c->hw->LSR & UART_LSR_TX_BUSY);
}

static void
uart_hw_putc(struct uart_controller *c, int ch)
{
  c->hw->THR = ch;
}

// Drain the hardware RX FIFO (flush/baud switch); the soft ring is untouched.
static void
uart_hw_drain_fifo(struct uart_controller *c)
{
  while (uart_hw_getc(c) != -1)
    ;
}

// RX-enable is internal state, toggled only around baud/mode switches and at
// init.  TX-enable is owned by the tx service loop (on while the ring has data).
static void
uart_rxenable(struct uart_controller *c, int enabled)
{
  uint32 ier = c->hw->IER;

  if (enabled)
    ier |= UART_IER_RX;
  else
    ier &= ~UART_IER_RX;
  c->hw->IER = ier;
}

static void
uart_txenable(struct uart_controller *c, int enabled)
{
  uint32 ier = c->hw->IER;

  if (enabled)
    ier |= UART_IER_TX;
  else
    ier &= ~UART_IER_TX;
  c->hw->IER = ier;
}

// Program the 20-bit divisor (DLH:DLL:DLF): the APB0 clock is divided by
// (DLH<<12 | DLL<<4 | DLF), integer in DLH/DLL, fraction in the 4-bit DLF.
static void
uart_set_divisor(struct uart_controller *c, uint32 baud)
{
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor = freq / baud;
  uint8 dlh = divisor >> 12;
  uint8 dll = (divisor - (dlh << 12)) / UART_BRATE_CONST;
  uint8 dlf = divisor - (dlh << 12) - dll * UART_BRATE_CONST;

  c->hw->LCR |= (1u << 7);      // DLAB: latch DLL/DLH/DLF
  c->hw->DLH = dlh;
  c->hw->DLL = dll;
  c->hw->DLF = dlf;
  c->hw->LCR &= ~(1u << 7);     // clear DLAB, keep the current LCR format
}

// 8N1 + FIFO on + RX trigger at 1 byte (SDK defaults).  DLAB must be clear,
// so this must run after any uart_set_divisor() that uses the LCR register.
static void
uart_configure_line(struct uart_controller *c)
{
  c->hw->LCR = (8 - 5);         // 8N1: (data_width-5) | stop<<2 | parity<<3
  c->hw->IER |= 0x80;           // keep the SDK's THRE bit
  c->hw->FCR = (0 << 6) | (3 << 4) | (1 << 3) | 1;  // FIFO on, RX trig 1, TX trig 8
  c->hw->SRT = 0;               // receive FIFO trigger = 1 byte (UART_RECEIVE_FIFO_1)
  c->hw->STET = 0;
}

// ---------- UART Init ----------

void
uartinit(struct uart_controller *c)
{
  // Clock + reset, as the SDK.  The sysctl clock/reset ids are consecutive per
  // UART (SYSCTL_CLOCK_UART1 + index), as are the DMA select ids below.
  sysctl_clock_enable(SYSCTL_CLOCK_UART1 + c->index);
  sysctl_reset(SYSCTL_RESET_UART1 + c->index);

  // Route the fpioa pins (UART1: IO7=TX, IO8=RX).  The fpioa function ids are
  // consecutive per UART too, RX then TX (FUNC_UART1_RX + index*2).
  fpioa_set_function(c->tx_io, FUNC_UART1_TX + c->index * 2);
  fpioa_set_function(c->rx_io, FUNC_UART1_RX + c->index * 2);

  // UART RX inputs are floating (pu=0); a low undriven line is framed as an
  // endless 0x00 stream.  Pull the RX pin up so idle reads as high.
  fpioa->io[c->rx_io].pu = 1;

  uart_set_divisor(c, c->default_baud);
  uart_configure_line(c);

  initlock(&c->rx.lock, "uartrx");
  initlock(&c->tx.lock, "uarttx");
  ringbuffer_init(&c->rx.ring, (uint8 *)c->rx.buf, UART_RX_BUF_SIZE);
  ringbuffer_init(&c->tx.ring, (uint8 *)c->tx.buf, UART_TX_BUF_SIZE);
  c->requested_baud = c->default_baud;
  c->rx_mode = UART_MODE_DMA;
  c->tx_mode = UART_MODE_DMA;
  c->rx_dma_active = 0;

  // Bring up the DMA default's RX hardware (8-byte trigger, arm chan_rx).  TX
  // needs nothing at init -- tx_mode already reads DMA and THRE is already off.
  uart_set_rx_mode(c, UART_MODE_DMA);

  // Register this instance's PLIC sources.  Device IRQs are consecutive per
  // UART (UART0_IRQ=11, then 12/13); the shared DMAC IRQ (32) stays UART1-only
  // because RX DMA is not enabled on UART2/3.
  irq_register(UART0_IRQ + c->index, uartintr, c);
  if (c->index == UART_DEVICE_1)
    irq_register(DMAC_CH5_IRQ, uart_dma_rx_intr, c);
}

// ---------- UART RX ----------

// Drain the hardware FIFO into the rx ring, count drops, wake readers.
// Caller must hold c->rx.lock.
static void
uart_rx_service(struct uart_controller *c)
{
  int received = 0;
  int ch;

  while ((ch = uart_hw_getc(c)) != -1) {
    if (ringbuffer_push(&c->rx.ring, (uint8)ch))
      received = 1;
    else
      c->rx.dropped++;
  }

  if (received)
    wakeup_reason(&c->rx.ring, WAKEUP_DEVICE);
}

// Block for rx data.  1 = data; 0 = flush bumped the epoch; -1 = interrupted.
// Caller must hold c->rx.lock.
static int
uart_rx_wait_data(struct uart_controller *c, uint epoch)
{
  struct proc *p = myproc();

  for (;;) {
    if (!ringbuffer_empty(&c->rx.ring))
      return 1;
    if (p && sleep_interruptible(&c->rx.ring, &c->rx.lock) < 0)
      return -1;
    if (!p)
      sleep(&c->rx.ring, &c->rx.lock);
    if (epoch != c->rx.epoch)
      return 0;
  }
}

int
uart_read(struct uart_controller *c, char *dst, int n)
{
  int i = 0;
  int reason;
  uint epoch;

  if (n <= 0)
    return 0;

  acquire(&c->rx.lock);
  epoch = c->rx.epoch;
  reason = uart_rx_wait_data(c, epoch);
  if (reason != 1) {
    release(&c->rx.lock);
    return reason;
  }
  while (i < n && ringbuffer_pop(&c->rx.ring, (uint8 *)&dst[i]))
    i++;
  release(&c->rx.lock);
  return i;
}

// ---- RX DMA (chan_rx, IRQ 32): boundary-event-driven frame harvesting ----
static void
uart_rx_dma_harvest(struct uart_controller *c)
{
  uint32 n = ((uint32)(uintptr_t)dmac->channel[c->chan_rx].dar
              - (uint32)(uintptr_t)c->rx_dma_buf) / 4;
  int received = 0;
  int ch;

  if (n > UART_RXDMA_SIZE)
    n = UART_RXDMA_SIZE;       // clamp
  for (uint32 i = 0; i < n; i++) {
    if (ringbuffer_push(&c->rx.ring, (uint8)(c->rx_dma_buf[i] & 0xff)))
      received = 1;
    else
      c->rx.dropped++;
  }
  while ((ch = uart_hw_getc(c)) != -1) {
    if (ringbuffer_push(&c->rx.ring, (uint8)ch))
      received = 1;
    else
      c->rx.dropped++;
  }

  if (received)
    wakeup_reason(&c->rx.ring, WAKEUP_DEVICE);
}

// Arm a fresh RX block on chan_rx (lock held).  Caller must have stopped the
// old transfer so dmac_set_single_mode's internal dmac_wait_idle is a no-op.
static void
uart_rx_dma_start(struct uart_controller *c)
{
  sysctl_dma_select(c->chan_rx, SYSCTL_DMA_SELECT_UART1_RX_REQ + c->index * 2);
  dmac_set_single_mode(c->chan_rx, (void *)&c->hw->RBR, c->rx_dma_buf,
                       DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
                       DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, UART_RXDMA_SIZE);
  c->rx_dma_active = 1;
}

// RX boundary: harvest and re-arm.  Called from the PLIC DMAC IRQ and from
// uartintr on RDA/CTI in DMA mode.  Clear the DMA-done flag first so a stale
// done from a transfer stopped mid-switch doesn't re-raise IRQ 32 in a no-op
// storm while RX is in interrupt mode.
void
uart_dma_rx_intr(void *ctx)
{
  struct uart_controller *c = ctx;

  dmac->channel[c->chan_rx].intclear = 0xffffffff;
  acquire(&c->rx.lock);
  if (c->rx_mode == UART_MODE_DMA && c->rx_dma_active) {
    dmac_channel_disable(c->chan_rx);
    uart_rx_dma_harvest(c);
    uart_rx_dma_start(c);
  }
  release(&c->rx.lock);
}

// Discard the FIFO (and in-flight DMA) and soft ring, reset rx state, then
// wake readers so uart_read returns 0.  In DMA mode the harvested buffer is
// dropped and the channel re-armed to keep the stream continuous.
void
uart_flush_rx(struct uart_controller *c)
{
  acquire(&c->rx.lock);
  if (c->rx_mode == UART_MODE_DMA && c->rx_dma_active) {
    dmac_channel_disable(c->chan_rx);   // residual bytes are discarded
  } else {
    uart_hw_drain_fifo(c);
  }
  ringbuffer_reset(&c->rx.ring);
  c->rx.dropped = 0;
  c->rx.overrun = 0;
  c->rx.epoch++;
  wakeup_reason(&c->rx.ring, WAKEUP_DEVICE);
  if (c->rx_mode == UART_MODE_DMA && c->rx_dma_active)
    uart_rx_dma_start(c);
  release(&c->rx.lock);
}

// info[0]=dropped, [1]=buffered, [2]=ring capacity, [3]=RX mode (INT/DMA),
// [4]=hardware overruns (LSR OE).
void
uart_get_rx_stats(struct uart_controller *c, uint32 *info)
{
  acquire(&c->rx.lock);
  info[0] = c->rx.dropped;
  info[1] = ringbuffer_used(&c->rx.ring);
  info[2] = ringbuffer_capacity(&c->rx.ring);
  info[3] = (uint32)c->rx_mode;
  info[4] = c->rx.overrun;
  release(&c->rx.lock);
}

// Switch the RX path, preserving in-flight bytes (stop the DMA and harvest
// first).  DMA mode uses an 8-byte trigger (SRT=2) so short bursts stay in the
// FIFO and close via CTI instead of raising an RDA every byte.
void
uart_set_rx_mode(struct uart_controller *c, int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == c->rx_mode && c->rx_dma_active == (mode == UART_MODE_DMA))
    return;

  uart_rxenable(c, 0);   // quiet RDA/CTI while reconfiguring
  acquire(&c->rx.lock);
  // Harvest only if armed: on a never-armed channel dar holds a stale address
  // and (stale - base)/4 would push hundreds of bogus zero words into the ring.
  if (c->rx_dma_active) {
    dmac_channel_disable(c->chan_rx);
    uart_rx_dma_harvest(c);       // preserve bytes already in flight
  }
  if (mode == UART_MODE_DMA) {
    c->hw->SRT = 2;               // 8-byte trigger: short bursts -> CTI
    uart_rx_dma_start(c);
  } else {
    c->hw->SRT = 0;               // 1-byte trigger for the interrupt path
    c->rx_dma_active = 0;
  }
  c->rx_mode = mode;
  release(&c->rx.lock);
  uart_rxenable(c, 1);
}

// ---------- UART TX ----------

// Drain the tx ring into hardware, keep the TX interrupt on only while there
// is work, and wake writers.  Caller must hold c->tx.lock.
static void
uart_tx_service(struct uart_controller *c)
{
  uint8 ch;

  while (!ringbuffer_empty(&c->tx.ring) && uart_hw_tx_ready(c)) {
    ringbuffer_pop(&c->tx.ring, &ch);
    uart_hw_putc(c, ch);
  }

  wakeup_reason(&c->tx.ring, WAKEUP_DEVICE);
  uart_txenable(c, !ringbuffer_empty(&c->tx.ring));
}

// Block until the tx ring has room.  0 = a slot appeared; -1 = panicked or an
// interruptible sleep was interrupted, so the caller should stop writing.
// Caller must hold c->tx.lock.
static int
uart_tx_wait_room(struct uart_controller *c)
{
  struct proc *p = myproc();

  while (ringbuffer_full(&c->tx.ring)) {
    if (panicked)
      return -1;
    if (p && sleep_interruptible(&c->tx.ring, &c->tx.lock) < 0)
      return -1;
    if (!p)
      sleep(&c->tx.ring, &c->tx.lock);
  }
  return 0;
}

// TX via DMA: pack up to TXDMA_SIZE bytes into words, arm chan_tx,
// block on dmac_wait_done.
static int
uart_write_dma(struct uart_controller *c, const char *src, int n)
{
  int done = 0;

  while (done < n) {
    int chunk = n - done;
    int i;

    if (chunk > UART_TXDMA_SIZE)
      chunk = UART_TXDMA_SIZE;
    acquire(&c->tx.lock);
    for (i = 0; i < chunk; i++)
      c->tx_dma_buf[i] = (uint32)(uint8)src[done + i];
    sysctl_dma_select(c->chan_tx, SYSCTL_DMA_SELECT_UART1_TX_REQ + c->index * 2);
    dmac_set_single_mode(c->chan_tx, c->tx_dma_buf, (void *)&c->hw->THR,
                         DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, (uint64)chunk);
    release(&c->tx.lock);
    if (dmac_wait_done(c->chan_tx, UART_DMA_TIMEOUT) < 0)
      return done > 0 ? done : -1;
    done += chunk;
  }
  return done;
}

int
uart_write(struct uart_controller *c, const char *src, int n)
{
  int i;

  if (c->tx_mode == UART_MODE_DMA)
    return uart_write_dma(c, src, n);

  acquire(&c->tx.lock);
  for (i = 0; i < n; i++) {
    if (uart_tx_wait_room(c) < 0) {
      release(&c->tx.lock);
      return i > 0 ? i : -1;
    }
    ringbuffer_push(&c->tx.ring, (uint8)src[i]);
    uart_tx_service(c);
  }
  release(&c->tx.lock);
  return i;
}

// Wait for the tx ring and hardware FIFO to drain; used by the baud switch and
// the switch into DMA TX so neither changes the line mid-frame.  In DMA mode
// the ring is bypassed, so this reduces to waiting TEMT.
static void
uart_flush_tx(struct uart_controller *c)
{
  acquire(&c->tx.lock);
  while (!ringbuffer_empty(&c->tx.ring)) {
    uart_tx_service(c);
    if (!ringbuffer_empty(&c->tx.ring))
      sleep(&c->tx.ring, &c->tx.lock);
  }
  release(&c->tx.lock);
  while (!(c->hw->LSR & UART_LSR_TEMT))
    ;
}

// Switch the TX path.  DMA mode flushes any bytes queued while still in INT
// mode, then turns the THRE interrupt off -- DMA owns the FIFO.  STET stays 1:
// it drives both THRE (INT) and DMATXREQ (DMA).
void
uart_set_tx_mode(struct uart_controller *c, int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == c->tx_mode)
    return;

  if (mode == UART_MODE_DMA)
    uart_flush_tx(c);
  acquire(&c->tx.lock);
  uart_txenable(c, 0);
  c->tx_mode = mode;
  release(&c->tx.lock);
}

// ---------- baudrate setup ----------

void
uart_set_baud(struct uart_controller *c, int baud)
{
  if (baud <= 0)
    return;

  // Restore the prior RX interrupt state: forcing it on lets the ISR drain the
  // FIFO before the next read (the "loop reads back 0 bytes" bug).
  int rx_was_on = (c->hw->IER & UART_IER_RX) != 0;

  uart_flush_tx(c);
  uart_rxenable(c, 0);
  acquire(&c->rx.lock);
  if (c->rx_mode == UART_MODE_DMA && c->rx_dma_active) {
    dmac_channel_disable(c->chan_rx);   // stop DMA, keep harvested bytes
    uart_rx_dma_harvest(c);
  } else {
    uart_hw_drain_fifo(c);              // bytes in the soft ring are kept
  }
  uart_set_divisor(c, (uint32)baud);
  c->requested_baud = (uint32)baud;
  if (c->rx_mode == UART_MODE_DMA && c->rx_dma_active)
    uart_rx_dma_start(c);   // re-arm on the new line rate
  release(&c->rx.lock);
  if (rx_was_on)
    uart_rxenable(c, 1);
}

void
uart_get_baud_info(struct uart_controller *c, uint32 *info)
{
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor;

  // DLH/DLL alias IER/RBR and only decode as the divisor latch while DLAB is
  // set -- without it the readback silently returns IER/RBR contents.
  c->hw->LCR |= (1u << 7);   // DLAB on: DLH/DLL/DLF visible at 0x04/0x00/0xc0
  divisor = ((c->hw->DLH & 0xff) << 12) | ((c->hw->DLL & 0xff) << 4) | (c->hw->DLF & 0xf);
  c->hw->LCR &= ~(1u << 7);  // DLAB off, line format preserved

  info[0] = c->requested_baud;
  info[1] = divisor ? freq / divisor : 0;
  info[2] = divisor;
  info[3] = freq;
}

// ---------- handler ----------

void
uartintr(void *ctx)
{
  struct uart_controller *c = ctx;
  uint8 status = c->hw->IIR & 0xf;

  switch (status) {
  case UART_IIR_THRE:
    if (c->tx_mode == UART_MODE_DMA)
      break;   // DMA TX owns the FIFO; a stale THRE is spurious
    acquire(&c->tx.lock);
    uart_tx_service(c);
    release(&c->tx.lock);
    break;
  case UART_IIR_RDA:
    if (c->rx_mode == UART_MODE_DMA)
      break;   // DMA drains the FIFO; done/CTI harvest.  Harvesting on RDA
               // disables the channel with dar mid-transfer and races the
               // DMA-done ISR, corrupting frames (seen as 1.5M burn CRC errors).
    acquire(&c->rx.lock);
    uart_rx_service(c);
    release(&c->rx.lock);
    break;
  case UART_IIR_TIMEOUT:   // CTI: FIFO idle with bytes left
    if (c->rx_mode == UART_MODE_DMA) {
      // A frame tail too short to fill a DMA block closes via CTI.
      uart_dma_rx_intr(c);
    } else {
      acquire(&c->rx.lock);
      uart_rx_service(c);
      release(&c->rx.lock);
    }
    break;
  case UART_IIR_LSERR:
    // Line-status error (overrun/parity/framing): reading LSR clears it.
    // Count FIFO overruns so a saturated RX path is observable instead of
    // silently dropping bytes (the RX ring's own dropped counter never sees
    // bytes the FIFO lost before the DMA/interrupt got to them).
    acquire(&c->rx.lock);
    if (c->hw->LSR & UART_LSR_OE)
      c->rx.overrun++;
    release(&c->rx.lock);
    break;
  default:
    break;  // 0x01 = no interrupt pending
  }
}
