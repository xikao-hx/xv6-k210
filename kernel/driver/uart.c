#include "dmac.h"
#include "fpioa.h"
#include "memlayout.h"
#include "printf.h"
#include "proc.h"
#include "ringbuffer.h"
#include "sysctl.h"
#include "uart.h"
#include "uart-dw.h"

static volatile uart_t *const uart = (volatile uart_t *)UART;

#define UART_LSR_DR      (1u << 0)
#define UART_LSR_TX_BUSY (1u << 5)
#define UART_LSR_TEMT    (1u << 6)

#define UART_IIR_THRE    0x02
#define UART_IIR_RDA     0x04
#define UART_IIR_LSERR   0x06
#define UART_IIR_TIMEOUT 0x0c

#define UART_IER_RX      0x01
#define UART_IER_TX      0x02

#define UART_BRATE_CONST 16

#define UART_RX_BUF_SIZE 32768
#define UART_TX_BUF_SIZE 4096

// DMA per-transfer chunk (32-bit words, 1 byte/word, as the SDK).  RX re-arms
// RXDMA_SIZE on each boundary; TX writes > TXDMA_SIZE split into blocks.
#define UART_RXDMA_SIZE 512
#define UART_TXDMA_SIZE 256
// dmac_wait_done(CH4) spin budget; ~1s at 400MHz, only guards a wedged line.
#define UART_DMA_TIMEOUT 100000000UL

// RX/TX state, split like uarths.c.  Each buf is size+1: the ring reserves one
// slot to tell "empty" from "full"; ringbuffer_capacity() is the usable size.
struct uart_rx {
  struct spinlock lock;
  char buf[UART_RX_BUF_SIZE + 1];
  struct ringbuffer ring;
  uint dropped;        // bytes discarded because the ring was full
  uint epoch;          // bumped by uart_flush_rx to wake blocked readers
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

// Active RX/TX path, defaulting to DMA.
static int rx_mode = UART_MODE_DMA;
static int tx_mode = UART_MODE_DMA;
static uint32 uart_rx_dma_buf[UART_RXDMA_SIZE] __attribute__((aligned(8)));
static uint32 uart_tx_dma_buf[UART_TXDMA_SIZE] __attribute__((aligned(8)));
static int rx_dma_active;    // CH5 is currently armed, feeding uart_rx_dma_buf

// ---------- Hardware Operation ----------

static int
uart_hw_getc(void)
{
  if (!(uart->LSR & UART_LSR_DR))
    return -1;
  return uart->RBR & 0xff;
}

static int
uart_hw_tx_ready(void)
{
  // K210 inverts LSR bit 5: 1 = busy, 0 = ready to accept a byte.
  return !(uart->LSR & UART_LSR_TX_BUSY);
}

static void
uart_hw_putc(int c)
{
  uart->THR = c;
}

// Drain the hardware RX FIFO (flush/baud switch); the soft ring is untouched.
static void
uart_hw_drain_fifo(void)
{
  while (uart_hw_getc() != -1)
    ;
}

// RX-enable is internal state, toggled only around baud/mode switches and at
// init.  TX-enable is owned by the tx service loop (on while the ring has data).
static void
uart_rxenable(int enabled)
{
  uint32 ier = uart->IER;

  if (enabled)
    ier |= UART_IER_RX;
  else
    ier &= ~UART_IER_RX;
  uart->IER = ier;
}

static void
uart_txenable(int enabled)
{
  uint32 ier = uart->IER;

  if (enabled)
    ier |= UART_IER_TX;
  else
    ier &= ~UART_IER_TX;
  uart->IER = ier;
}

// Program the 20-bit divisor (DLH:DLL:DLF): the APB0 clock is divided by
// (DLH<<12 | DLL<<4 | DLF), integer in DLH/DLL, fraction in the 4-bit DLF.
static void
uart_set_divisor(uint32 baud)
{
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor = freq / baud;
  uint8 dlh = divisor >> 12;
  uint8 dll = (divisor - (dlh << 12)) / UART_BRATE_CONST;
  uint8 dlf = divisor - (dlh << 12) - dll * UART_BRATE_CONST;

  uart->LCR |= (1u << 7);      // DLAB: latch DLL/DLH/DLF
  uart->DLH = dlh;
  uart->DLL = dll;
  uart->DLF = dlf;
  uart->LCR &= ~(1u << 7);     // clear DLAB, keep the current LCR format
}

// 8N1 + FIFO on + RX trigger at 1 byte (SDK defaults).  DLAB must be clear,
// so this must run after any uart_set_divisor() that uses the LCR register.
static void
uart_configure_line(void)
{
  uart->LCR = (8 - 5);         // 8N1: (data_width-5) | stop<<2 | parity<<3
  uart->IER |= 0x80;           // keep the SDK's THRE bit
  uart->FCR = (0 << 6) | (3 << 4) | (1 << 3) | 1;  // FIFO on, RX trig 1, TX trig 8
  uart->SRT = 0;               // receive FIFO trigger = 1 byte (UART_RECEIVE_FIFO_1)
  uart->STET = 0;
}

// ---------- UART Init ----------

void
uartinit(void)
{
  // Clock + reset, as the SDK.
  sysctl_clock_enable(SYSCTL_CLOCK_UART1);
  sysctl_reset(SYSCTL_RESET_UART1);

  // Route the FPIOA pins: IO7=TX, IO8=RX (confirmed by the uarttest loopback).
  fpioa_set_function(UART_TX_IO, FUNC_UART1_TX);
  fpioa_set_function(UART_RX_IO, FUNC_UART1_RX);

  // FUNC_UART1_RX is a floating input (pu=0); a low undriven line is framed as
  // an endless 0x00 stream.  Pull it up so idle reads as high.
  fpioa->io[UART_RX_IO].pu = 1;

  uart_set_divisor(115200);
  uart_configure_line();

  initlock(&uart_rx.lock, "uartrx");
  initlock(&uart_tx.lock, "uarttx");
  ringbuffer_init(&uart_rx.ring, (uint8 *)uart_rx.buf, UART_RX_BUF_SIZE);
  ringbuffer_init(&uart_tx.ring, (uint8 *)uart_tx.buf, UART_TX_BUF_SIZE);
  requested_baud = 115200;

  // Bring up the DMA default's RX hardware (8-byte trigger, arm CH5).  TX needs
  // nothing at init -- tx_mode already reads DMA and THRE is already off.
  uart_set_rx_mode(UART_MODE_DMA);
}

// ---------- UART RX ----------

// Drain the hardware FIFO into the rx ring, count drops, wake readers.
// Caller must hold uart_rx.lock.
static void
uart_rx_service(void)
{
  int received = 0;
  int c;

  while ((c = uart_hw_getc()) != -1) {
    if (ringbuffer_push(&uart_rx.ring, (uint8)c))
      received = 1;
    else
      uart_rx.dropped++;
  }

  if (received)
    wakeup_reason(&uart_rx.ring, WAKEUP_DEVICE);
}

// Block for rx data.  1 = data; 0 = flush bumped the epoch; -1 = interrupted.
// Caller must hold uart_rx.lock.
static int
uart_rx_wait_data(uint epoch)
{
  struct proc *p = myproc();

  for (;;) {
    if (!ringbuffer_empty(&uart_rx.ring))
      return 1;
    if (p && sleep_interruptible(&uart_rx.ring, &uart_rx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uart_rx.ring, &uart_rx.lock);
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

// ---- RX DMA (CH5, IRQ 32): boundary-event-driven frame harvesting ----
static void
uart_rx_dma_harvest(void)
{
  uint32 n = ((uint32)(uintptr_t)dmac->channel[DMAC_CHANNEL5].dar
              - (uint32)(uintptr_t)uart_rx_dma_buf) / 4;
  int received = 0;
  int c;

  if (n > UART_RXDMA_SIZE)
    n = UART_RXDMA_SIZE;       // clamp
  for (uint32 i = 0; i < n; i++) {
    if (ringbuffer_push(&uart_rx.ring, (uint8)(uart_rx_dma_buf[i] & 0xff)))
      received = 1;
    else
      uart_rx.dropped++;
  }
  while ((c = uart_hw_getc()) != -1) {
    if (ringbuffer_push(&uart_rx.ring, (uint8)c))
      received = 1;
    else
      uart_rx.dropped++;
  }

  if (received)
    wakeup_reason(&uart_rx.ring, WAKEUP_DEVICE);
}

// Arm a fresh RX block on CH5 (lock held).  Caller must have stopped the old
// transfer so dmac_set_single_mode's internal dmac_wait_idle is a no-op.
static void
uart_rx_dma_start(void)
{
  sysctl_dma_select(SYSCTL_DMA_CHANNEL_5, SYSCTL_DMA_SELECT_UART1_RX_REQ);
  dmac_set_single_mode(DMAC_CHANNEL5, (void *)&uart->RBR, uart_rx_dma_buf,
                       DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
                       DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, UART_RXDMA_SIZE);
  rx_dma_active = 1;
}

// RX boundary: harvest and re-arm.  Called from the PLIC CH5 ISR and from
// uartintr on RDA/CTI in DMA mode.  Clear the DMA-done flag first so a stale
// done from a transfer stopped mid-switch doesn't re-raise IRQ 32 in a no-op
// storm while RX is in interrupt mode.
void
uart_dma_rx_intr(void)
{
  dmac->channel[DMAC_CHANNEL5].intclear = 0xffffffff;
  acquire(&uart_rx.lock);
  if (rx_mode == UART_MODE_DMA && rx_dma_active) {
    dmac_channel_disable(DMAC_CHANNEL5);
    uart_rx_dma_harvest();
    uart_rx_dma_start();
  }
  release(&uart_rx.lock);
}

// Discard the FIFO (and in-flight DMA) and soft ring, reset rx state, then
// wake readers so uart_read returns 0.  In DMA mode the harvested buffer is
// dropped and CH5 re-armed to keep the stream continuous.
void
uart_flush_rx(void)
{
  acquire(&uart_rx.lock);
  if (rx_mode == UART_MODE_DMA && rx_dma_active) {
    dmac_channel_disable(DMAC_CHANNEL5);   // residual bytes are discarded
  } else {
    uart_hw_drain_fifo();
  }
  ringbuffer_reset(&uart_rx.ring);
  uart_rx.dropped = 0;
  uart_rx.epoch++;
  wakeup_reason(&uart_rx.ring, WAKEUP_DEVICE);
  if (rx_mode == UART_MODE_DMA && rx_dma_active)
    uart_rx_dma_start();
  release(&uart_rx.lock);
}

// info[0]=dropped, [1]=buffered, [2]=ring capacity, [3]=RX mode (INT/DMA).
void
uart_get_rx_stats(uint32 *info)
{
  acquire(&uart_rx.lock);
  info[0] = uart_rx.dropped;
  info[1] = ringbuffer_used(&uart_rx.ring);
  info[2] = ringbuffer_capacity(&uart_rx.ring);
  info[3] = (uint32)rx_mode;
  release(&uart_rx.lock);
}

// Switch the RX path, preserving in-flight bytes (stop the DMA and harvest
// first).  DMA mode uses an 8-byte trigger (SRT=2) so short bursts stay in the
// FIFO and close via CTI instead of raising an RDA every byte.
void
uart_set_rx_mode(int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == rx_mode && rx_dma_active == (mode == UART_MODE_DMA))
    return;

  uart_rxenable(0);   // quiet RDA/CTI while reconfiguring
  acquire(&uart_rx.lock);
  // Harvest only if armed: on a never-armed channel dar holds a stale address
  // and (stale - base)/4 would push hundreds of bogus zero words into the ring.
  if (rx_dma_active) {
    dmac_channel_disable(DMAC_CHANNEL5);
    uart_rx_dma_harvest();       // preserve bytes already in flight
  }
  if (mode == UART_MODE_DMA) {
    uart->SRT = 2;               // 8-byte trigger: short bursts -> CTI
    uart_rx_dma_start();
  } else {
    uart->SRT = 0;               // 1-byte trigger for the interrupt path
    rx_dma_active = 0;
  }
  rx_mode = mode;
  release(&uart_rx.lock);
  uart_rxenable(1);
}

// ---------- UART TX ----------

// Drain the tx ring into hardware, keep the TX interrupt on only while there
// is work, and wake writers.  Caller must hold uart_tx.lock.
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

// Block until the tx ring has room.  0 = a slot appeared; -1 = panicked or an
// interruptible sleep was interrupted, so the caller should stop writing.
// Caller must hold uart_tx.lock.
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

// TX via DMA: pack up to TXDMA_SIZE bytes into words, arm CH4, 
// block on dmac_wait_done. 
static int
uart_write_dma(const char *src, int n)
{
  int done = 0;

  while (done < n) {
    int chunk = n - done;
    int i;

    if (chunk > UART_TXDMA_SIZE)
      chunk = UART_TXDMA_SIZE;
    acquire(&uart_tx.lock);
    for (i = 0; i < chunk; i++)
      uart_tx_dma_buf[i] = (uint32)(uint8)src[done + i];
    sysctl_dma_select(SYSCTL_DMA_CHANNEL_4, SYSCTL_DMA_SELECT_UART1_TX_REQ);
    dmac_set_single_mode(DMAC_CHANNEL4, uart_tx_dma_buf, (void *)&uart->THR,
                         DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, (uint64)chunk);
    release(&uart_tx.lock);
    if (dmac_wait_done(DMAC_CHANNEL4, UART_DMA_TIMEOUT) < 0)
      return done > 0 ? done : -1;
    done += chunk;
  }
  return done;
}

int
uart_write(const char *src, int n)
{
  int i;

  if (tx_mode == UART_MODE_DMA)
    return uart_write_dma(src, n);

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

// Wait for the tx ring and hardware FIFO to drain; used by the baud switch and
// the switch into DMA TX so neither changes the line mid-frame.  In DMA mode
// the ring is bypassed, so this reduces to waiting TEMT.
static void
uart_flush_tx(void)
{
  acquire(&uart_tx.lock);
  while (!ringbuffer_empty(&uart_tx.ring)) {
    uart_tx_service();
    if (!ringbuffer_empty(&uart_tx.ring))
      sleep(&uart_tx.ring, &uart_tx.lock);
  }
  release(&uart_tx.lock);
  while (!(uart->LSR & UART_LSR_TEMT))
    ;
}

// Switch the TX path.  DMA mode flushes any bytes queued while still in INT
// mode, then turns the THRE interrupt off -- DMA owns the FIFO.  STET stays 1:
// it drives both THRE (INT) and DMATXREQ (DMA).
void
uart_set_tx_mode(int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == tx_mode)
    return;

  if (mode == UART_MODE_DMA)
    uart_flush_tx();
  acquire(&uart_tx.lock);
  uart_txenable(0);
  tx_mode = mode;
  release(&uart_tx.lock);
}

// ---------- baudrate setup ----------

void
uart_set_baud(int baud)
{
  if (baud <= 0)
    return;

  // Restore the prior RX interrupt state: forcing it on lets the ISR drain the
  // FIFO before the next read (the "loop reads back 0 bytes" bug).
  int rx_was_on = (uart->IER & UART_IER_RX) != 0;

  uart_flush_tx();
  uart_rxenable(0);
  acquire(&uart_rx.lock);
  if (rx_mode == UART_MODE_DMA && rx_dma_active) {
    dmac_channel_disable(DMAC_CHANNEL5);   // stop DMA, keep harvested bytes
    uart_rx_dma_harvest();
  } else {
    uart_hw_drain_fifo();                  // bytes in the soft ring are kept
  }
  uart_set_divisor((uint32)baud);
  requested_baud = (uint32)baud;
  if (rx_mode == UART_MODE_DMA && rx_dma_active)
    uart_rx_dma_start();   // re-arm on the new line rate
  release(&uart_rx.lock);
  if (rx_was_on)
    uart_rxenable(1);
}

void
uart_get_baud_info(uint32 *info)
{
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor;

  // DLH/DLL alias IER/RBR and only decode as the divisor latch while DLAB is
  // set -- without it the readback silently returns IER/RBR contents.
  uart->LCR |= (1u << 7);   // DLAB on: DLH/DLL/DLF visible at 0x04/0x00/0xc0
  divisor = ((uart->DLH & 0xff) << 12) | ((uart->DLL & 0xff) << 4) | (uart->DLF & 0xf);
  uart->LCR &= ~(1u << 7);  // DLAB off, line format preserved

  info[0] = requested_baud;
  info[1] = divisor ? freq / divisor : 0;
  info[2] = divisor;
  info[3] = freq;
}

// ---------- handler ----------

void
uartintr(void)
{
  uint8 status = uart->IIR & 0xf;

  switch (status) {
  case UART_IIR_THRE:
    if (tx_mode == UART_MODE_DMA)
      break;   // DMA TX owns the FIFO; a stale THRE is spurious
    acquire(&uart_tx.lock);
    uart_tx_service();
    release(&uart_tx.lock);
    break;
  case UART_IIR_RDA:
  case UART_IIR_TIMEOUT:
    if (rx_mode == UART_MODE_DMA) {
      // RDA fires on K210 even with FCR[3]=1; both are frame boundaries.
      uart_dma_rx_intr();
    } else {
      acquire(&uart_rx.lock);
      uart_rx_service();
      release(&uart_rx.lock);
    }
    break;
  case UART_IIR_LSERR:
    // Line-status error (overrun/parity/framing): reading LSR clears it.
    (void)uart->LSR;
    break;
  default:
    break;  // 0x01 = no interrupt pending
  }
}
