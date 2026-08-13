// UART byte-stream driver for the K210 generic UART (DW APB 16550), the
// UART1/2/3 block distinct from UARTHS (which drives the console).
//
// Register-level port of kendryte-standalone-sdk/lib/drivers/uart.c, wrapped
// in the xv6 ring-buffer model used by uarths.c.  RX/TX each have two
// switchable paths, selected via UART_IOCTL_SET_RX_MODE / SET_TX_MODE:
//   - UART_MODE_INT (default): RX FIFO interrupt (IIR RDA / char-timeout)
//     drains into the rx ring; TX pumps the ring via the THRE interrupt.
//   - UART_MODE_DMA: RX uses DMA CH5 (boundary-event-driven, see below);
//     TX uses a blocking DMA CH4 transfer per write.
// This is Step 2 of the uart.h plan.  Poll mode (Step 1's diagnostic
// primitive) has been removed: DMA replaced it as the raw-path alternative.
//
// DMA channel allocation (fixed): TX = CH4, RX = CH5, shared with SPI1/I2C1,
// which are inactive during burn.  SPI0 keeps CH0/CH1.  The DMAC and its
// sysctl_dma_select() routing are the same ones the SPI driver uses.

#include "dmac.h"
#include "fpioa.h"
#include "memlayout.h"
#include "printf.h"
#include "proc.h"
#include "ringbuffer.h"
#include "sysctl.h"
#include "uart.h"

// ---- Register layout (DW APB UART, same offsets as the standalone SDK) ----
// 0x00 RBR/DLL/THR, 0x04 IER/DLH, 0x08 FCR/IIR, 0x0c LCR, 0x10 MCR,
// 0x14 LSR, 0x18 MSR, 0x1c SCR ... 0x30 SRBR/STHR[16] ... 0x9c SRT,
// 0xa0 STET, 0xc0 DLF.
typedef struct _uart {
  union { volatile uint32 RBR; volatile uint32 DLL; volatile uint32 THR; };
  union { volatile uint32 DLH; volatile uint32 IER; };
  union { volatile uint32 FCR; volatile uint32 IIR; };
  volatile uint32 LCR;
  volatile uint32 MCR;
  volatile uint32 LSR;
  volatile uint32 MSR;
  volatile uint32 SCR;
  volatile uint32 LPDLL;
  volatile uint32 LPDLH;
  volatile uint32 reserved1[2];
  volatile uint32 SRBR[16];
  volatile uint32 FAR;
  volatile uint32 TFR;
  volatile uint32 RFW;
  volatile uint32 USR;
  volatile uint32 TFL;
  volatile uint32 RFL;
  volatile uint32 SRR;
  volatile uint32 SRTS;
  volatile uint32 SBCR;
  volatile uint32 SDMAM;
  volatile uint32 SFE;
  volatile uint32 SRT;
  volatile uint32 STET;
  volatile uint32 HTX;
  volatile uint32 DMASA;
  volatile uint32 TCR;
  volatile uint32 DE_EN;
  volatile uint32 RE_EN;
  volatile uint32 DET;
  volatile uint32 TAT;
  volatile uint32 DLF;
  volatile uint32 RAR;
  volatile uint32 TAR;
  volatile uint32 LCR_EXT;
} uart_t;

// memlayout.h defines UART as the UART1 physical base (0x50210000).
static volatile uart_t *const uart = (volatile uart_t *)UART;

// LSR bits used here: 0 = DR (data ready), 5 = TX busy, 6 = TEMT (transmitter
// empty).  IMPORTANT: the K210's DW APB 16550 inverts bit 5 versus the
// textbook THRE -- it reads 1 while the transmitter is BUSY (can't accept a
// byte) and 0 when ready.  The standalone SDK's uart_channel_putc polls it
// exactly this way:
//     while (uart->LSR & (1u << 5)) continue;   // wait until NOT busy
//     uart->THR = c;
// A standard `while (!(LSR & THRE))` poll therefore hangs forever.  Bit 6
// (TEMT) is normal active-high.  IIR[3:0]: 2 = TX empty, 4 = RDA,
// 6 = line-status error, 0xc = character timeout, 1 = none pending.
#define UART_LSR_DR      (1u << 0)
#define UART_LSR_TX_BUSY (1u << 5)
#define UART_LSR_TEMT    (1u << 6)

#define UART_IIR_THRE    0x02
#define UART_IIR_RDA     0x04
#define UART_IIR_LSERR   0x06
#define UART_IIR_TIMEOUT 0x0c

#define UART_IER_RX      0x01
#define UART_IER_TX      0x02

// __UART_BRATE_CONST from the SDK: the 4-bit fractional field DLF is scaled
// by 16, so DLL holds divisor / 16 and DLF the remainder.
#define UART_BRATE_CONST 16

#define UART_RX_BUF_SIZE 32768
#define UART_TX_BUF_SIZE 4096

// DMA transfer chunk (in 32-bit words, one byte per word -- same packing as
// the SDK's uart_send_data_dma / uart_receive_data_dma).  RX is re-armed with
// a fresh RXDMA_SIZE block on every boundary event; TX writes > TXDMA_SIZE
// bytes are split into multiple blocking transfers.
#define UART_RXDMA_SIZE 512
#define UART_TXDMA_SIZE 256
// Spin budget for dmac_wait_done(CH4).  10^8 iterations is ~1s at 400MHz,
// comfortably above even a full TXDMA_SIZE chunk at 9600 baud (~270ms); the
// timeout only guards a wedged line, it is not part of normal operation.
#define UART_DMA_TIMEOUT 100000000UL

// RX/TX state, split exactly like uarths.c.  Each backing array is one byte
// longer than its size because a ringbuffer reserves one slot to tell
// "empty" apart from "full"; ringbuffer_capacity() reports the usable size.
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

// RX/TX path selection.  UART_MODE_INT by default; ioctl switches them.
// Guarded by the respective *_lock (rx_dma_active is read/written only under
// uart_rx.lock).  The DMAC requires the buffer to be memory on a cache-less
// 32-bit-aligned address; these are static so they live in kernel bss well
// within the 6MB is_memory() window at 0x80000000.
static int rx_mode = UART_MODE_INT;
static int tx_mode = UART_MODE_INT;
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

// Discard whatever sits in the hardware RX FIFO.  Used on flush and baud
// switch; the soft ring is untouched here.
static void
uart_hw_drain_fifo(void)
{
  while (uart_hw_getc() != -1)
    ;
}

// RX-enable is driver-internal state: it is toggled around a baud switch, a
// mode switch and at init, never by higher layers.  TX-enable is switched by
// the tx service loop, on only while the tx ring still holds data.
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

// Program the 20-bit divisor (DLH:DLL:DLF) for a baud rate.  The hardware
// divides the APB0 clock by (DLH<<12 | DLL<<4 | DLF); the integer part is
// split across DLH/DLL and the fraction lands in the 4-bit DLF field.
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

// One-shot line format + FIFO setup, mirrors the SDK's uart_configure() and
// uart_debug_init() defaults: 8 data bits, 1 stop bit, no parity, FIFO on,
// RX interrupt triggers at 1 byte.  DLAB is cleared so this must run after
// any uart_set_divisor() that shares the LCR register.
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
  // Clock + reset, as the SDK's uart_init(UART_DEVICE_1).
  sysctl_clock_enable(SYSCTL_CLOCK_UART1);
  sysctl_reset(SYSCTL_RESET_UART1);

  // Route the FPIOA pins to UART1 TX/RX.  uart.h pins are a working guess
  // pending real-hardware confirmation; swap UART_TX_IO/UART_RX_IO if the
  // board is wired the other way round.
  fpioa_set_function(UART_TX_IO, FUNC_UART1_TX);
  fpioa_set_function(UART_RX_IO, FUNC_UART1_RX);

  // FUNC_UART1_RX maps to a pure floating input (fpioa.c function_config has
  // pu=0/pd=0).  An undriven RX line that drifts low makes the DW receiver
  // frame it as an endless 0x00 stream -- u1test read showed buffered growing
  // past the 16-byte FIFO with nothing but zeros while IO8 was unconnected.
  // Pull the RX pin up so a disconnected/undriven line reads as UART idle
  // (high) instead of garbage.
  fpioa->io[UART_RX_IO].pu = 1;

  uart_set_divisor(115200);
  uart_configure_line();

  initlock(&uart_rx.lock, "uartrx");
  initlock(&uart_tx.lock, "uarttx");
  ringbuffer_init(&uart_rx.ring, (uint8 *)uart_rx.buf, UART_RX_BUF_SIZE);
  ringbuffer_init(&uart_tx.ring, (uint8 *)uart_tx.buf, UART_TX_BUF_SIZE);
  requested_baud = 115200;
  uart_txenable(0);
  uart_rxenable(1);
}

// ---------- UART RX ----------

// Drain the hardware FIFO into the rx ring, count bytes dropped when the
// ring is full, and wake readers if anything landed.  Caller must hold
// uart_rx.lock.
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

// Block until rx data is available.  Returns 1 on data; 0 if a flush bumped
// the epoch; -1 if an interruptible sleep was interrupted.  Caller must hold
// uart_rx.lock.
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
//
// CH5 is armed continuously (block size = UART_RXDMA_SIZE).  The DMAC pulls
// bytes out of the UART RBR as the FIFO passes the RX trigger and packs one
// byte per 32-bit word, so a WIDTH_32/MSIZE_1 transfer matches the SDK.  A
// *boundary* -- any of the three below -- closes a frame and makes the ISR
// (or the mode/baud/flush path) stop the channel, harvest, and re-arm:
//
//   - RDA (IIR=0x04): on the K210 this fires even with FCR[3]=1 (DMA mode),
//     so it must be treated as a boundary or the ISR storms while the DMA
//     runs on unread FIFO content.  Verified in Step 1.
//   - CTI (IIR=0x0C): the FIFO went idle below the trigger -- the short-frame
//     path that RDA can never see.  This is the biggest 待实机 risk: Step 1
//     only proved RDA.  If CTI is unreliable on K210, sub-trigger bursts
//     delay until the next boundary instead of being lost.
//   - CH5 transfer-done (PLIC IRQ 32): the block filled, RXDMA_SIZE bytes.
//
// All three funnel into uart_dma_rx_intr().  Re-arm inside it calls
// dmac_set_single_mode, which first disables CH5 -- a disabled channel is
// idle, so its internal dmac_wait_idle never enters the sleep loop and never
// touches myproc().  That invariant is what makes ISR-time re-arm safe; if
// real hardware ever breaks it, add a nowait variant to dmac.c instead.

// Called with uart_rx.lock held and CH5 already disabled.  Moves the bytes
// the DMAC already harvested into the rx ring, then whatever is still sitting
// in the hardware FIFO (the DMA may not have drained the last sub-trigger
// bytes).  The DMAC-side count is taken from the channel's dar register (the
// current destination address): every 32-bit word moved bumps dar by 4, so
// (dar - buf_base)/4 is the byte count even for a partial block.
//
// cmpltd_blk_size is NOT usable here: it counts *completed blocks*, and a
// 512-word single-block transfer only "completes" when the whole block is
// done, so it reads 0 on every short frame and the already-moved bytes would
// be silently dropped (real-hardware loopback lost the first 9 bytes of 16
// exactly this way).
static void
uart_rx_dma_harvest(void)
{
  uint32 n = ((uint32)(uintptr_t)dmac->channel[DMAC_CHANNEL5].dar
              - (uint32)(uintptr_t)uart_rx_dma_buf) / 4;
  int received = 0;
  int c;

  if (n > UART_RXDMA_SIZE)
    n = UART_RXDMA_SIZE;       // belt-and-braces clamp
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

// Called with uart_rx.lock held.  Arms a fresh RX block on CH5.  The caller
// must have stopped the old transfer first (or the channel must never have
// been armed) so dmac_set_single_mode's internal dmac_wait_idle is a no-op.
static void
uart_rx_dma_start(void)
{
  sysctl_dma_select(SYSCTL_DMA_CHANNEL_5, SYSCTL_DMA_SELECT_UART1_RX_REQ);
  dmac_set_single_mode(DMAC_CHANNEL5, (void *)&uart->RBR, uart_rx_dma_buf,
                       DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
                       DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, UART_RXDMA_SIZE);
  rx_dma_active = 1;
}

// RX frame boundary: harvest and re-arm.  Called from the PLIC CH5 ISR
// (devintr) and from uartintr on RDA/CTI while RX is in DMA mode.
//
// The channel's DMA-done flag is cleared up front, unconditionally: a done
// left pending from a transfer stopped mid-switch would otherwise hold the
// DMAC IRQ line asserted, and an edge/level PLIC would re-raise IRQ 32 in a
// no-op storm while RX is in interrupt mode.
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

// Discard the hardware FIFO (and any in-flight DMA) and the soft ring, reset
// the rx state, then wake blocked readers so uart_read returns 0 instead of
// waiting forever.  In DMA mode the harvested DMA buffer is dropped too and
// the channel is re-armed, keeping the byte stream continuous after the flush.
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

// info[0] bytes dropped, info[1] bytes buffered, info[2] ring capacity,
// info[3] active RX mode (UART_MODE_INT / UART_MODE_DMA).
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

// Switch the RX path.  Both directions preserve in-flight bytes by stopping
// the DMA and harvesting first; only then is the FIFO trigger level changed
// and the new path (re)started.  DMA mode raises the RX trigger to 8 bytes
// (SRT=2) so sub-trigger bursts stay in the FIFO and close via CTI instead of
// every byte raising an RDA that the DMAC races.
void
uart_set_rx_mode(int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == rx_mode)
    return;

  uart_rxenable(0);   // quiet RDA/CTI while reconfiguring
  acquire(&uart_rx.lock);
  // Harvest only when a transfer was actually armed: on a never-armed channel
  // (INT->DMA first switch) CH5's dar holds a stale address from whoever used
  // the channel before, and (stale - base)/4 would push hundreds of bogus
  // zero words from the bss buffer into the ring.
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

// DMA TX: pack up to TXDMA_SIZE bytes into 32-bit words, arm CH4 and block on
// dmac_wait_done.  The spinlock is released before the wait -- a busy DMA
// drain takes milliseconds and must not be held while spinning.  A second
// writer re-arming CH4 mid-transfer is therefore unsynchronized; the devsw
// layer serializes character-device writers in practice (u1test, burn.c).
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

// Wait until the tx ring and the hardware FIFO have drained.  Used by the
// baud switch and by the switch into DMA TX, both of which must not change
// the line mid-frame.  In DMA mode the ring is bypassed, so this reduces to
// waiting TEMT (covers an in-flight DMA block too).
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

// Switch the TX path.  DMA mode first flushes any bytes still queued in the
// tx ring (they were written while still in interrupt mode) and turns the
// THRE interrupt off -- the DMA owns the FIFO now.  STET stays 1: it is what
// drives both the THRE interrupt (INT mode) and DMATXREQ (DMA mode).
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

  // Remember whether RX interrupts were on, and restore them rather than
  // forcing them on: re-enabling unconditionally would let the ISR drain the
  // FIFO between the baud switch and the next read -- the "loop reads back 0
  // bytes" symptom from Step 1.
  int rx_was_on = (uart->IER & UART_IER_RX) != 0;

  uart_flush_tx();
  uart_rxenable(0);
  acquire(&uart_rx.lock);
  if (rx_mode == UART_MODE_DMA && rx_dma_active) {
    // Stop DMA and preserve the bytes it already moved: they belong to the
    // application and must survive the baud switch.
    dmac_channel_disable(DMAC_CHANNEL5);
    uart_rx_dma_harvest();
  } else {
    // Only drain the hardware FIFO: bytes already read into the soft ring
    // belong to the application and must survive the baud switch.
    uart_hw_drain_fifo();
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

  // DLH/DLL share addresses with IER/RBR and only decode as the divisor
  // latch while DLAB is set.  Without it the readback silently returns the
  // interrupt-enable and receive-buffer contents -- the bogus div=524300 in
  // early debugging was exactly that (IER 0x80 << 12 | RBR 0 << 4 | DLF 12).
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
