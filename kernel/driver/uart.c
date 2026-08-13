// UART byte-stream driver for the K210 generic UART (DW APB 16550), the
// UART1/2/3 block distinct from UARTHS (which drives the console).
//
// Register-level port of kendryte-standalone-sdk/lib/drivers/uart.c, wrapped
// in the xv6 interrupt-driven ring-buffer model used by uarths.c:
//   - RX: a HW FIFO interrupt (IIR RDA / char-timeout) drains into the rx
//     ring; uart_read blocks on it.
//   - TX: uart_write pushes into the tx ring and pumps the HW; the THRE
//     interrupt keeps draining the ring only while it is non-empty.
// This is Step 1 of the uart.h plan (interrupt-driven RX/TX).  DMA-mode
// RX/TX (the SDK's uart_send_data_dma & friends) is Step 2 and not ported.

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

// RX-enable is driver-internal state: it is toggled around a baud switch and
// at init, never by higher layers.  TX-enable is switched by the tx service
// loop, on only while the tx ring still holds data.
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
  uart->STET = 0;              // TX trigger water level: 0 disables the TX-empty
                               // interrupt, so a RAW-mode write that fills the
                               // FIFO would never resume (see uart1-bank/arch)
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

  // TEMP-DIAG: verify the TX fix on the next boot.  apb0en/uart1en = clock
  // enable bits actually set; lsr0 should read 0x60 (bit5=0 ready, bit6=1
  // transmitter empty) in the healthy idle state; byte-drained=1 means the
  // probe 'U' shifted out, i.e. the baud clock is alive.
  {
    uint32 cenc = *(volatile uint32 *)(SYSCTL + 0x28);   // clk_en_cent
    uint32 peri = *(volatile uint32 *)(SYSCTL + 0x2c);   // clk_en_peri
    uint32 lsr0 = uart->LSR;
    uint32 iir0 = uart->IIR;
    int cleared = 0;
    int drained = 0;
    uart->THR = 'U';
    for (int i = 0; i < 4000000; i++) {
      uint32 l = uart->LSR;
      if (!(l & UART_LSR_TEMT))
        cleared = 1;          // byte entered the TX path
      else if (cleared) {
        drained = 1;          // ...and shifted out
        break;
      }
    }
    printf("uart1 diag: apb0en=%d uart1en=%d lsr0=%x iir0=%x"
           " txbusy0=%d byte-drained=%d\n",
           (cenc >> 3) & 1, (peri >> 16) & 1, lsr0, iir0,
           (lsr0 & UART_LSR_TX_BUSY) ? 1 : 0, drained);
  }

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

// Discard the hardware FIFO and the soft ring, reset the rx state, then
// wake blocked readers so uart_read returns 0 instead of waiting forever.
void
uart_flush_rx(void)
{
  acquire(&uart_rx.lock);
  uart_hw_drain_fifo();
  ringbuffer_reset(&uart_rx.ring);
  uart_rx.dropped = 0;
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

// Wait until the tx ring and the hardware FIFO have drained.  Used by the
// baud switch, which must not change the line rate mid-frame.
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

// ---------- poll-mode primitives ----------
// Bypass the soft ring and the interrupt service loop: read/write the DW
// hardware directly.  uart_set_poll(1) first switches the RX/TX interrupt
// enables off, otherwise the ISR would drain the RBR behind poll_getc's back
// and bytes would be lost.  This is the fastest way to tell "hardware path
// broken" apart from "interrupt path broken".

int
uart_poll_putc(int c)
{
  while (uart->LSR & UART_LSR_TX_BUSY)
    ;
  uart->THR = c;
  return c & 0xff;
}

int
uart_poll_getc(void)
{
  if (!(uart->LSR & UART_LSR_DR))
    return -1;
  return uart->RBR & 0xff;
}

void
uart_set_poll(int on)
{
  if (on) {
    uart_rxenable(0);
    uart_txenable(0);
  } else {
    uart_txenable(0);
    uart_rxenable(1);
  }
}

// ---------- baudrate setup ----------

void
uart_set_baud(int baud)
{
  if (baud <= 0)
    return;

  // Remember whether RX interrupts were on.  uart_set_poll(1) turns them off,
  // and unconditionally re-enabling them here would let the ISR drain the FIFO
  // between the baud switch and the next poll read -- the "loop reads back 0
  // bytes" symptom.  Restore the previous state instead of forcing them on.
  int rx_was_on = (uart->IER & UART_IER_RX) != 0;

  uart_flush_tx();
  uart_rxenable(0);
  acquire(&uart_rx.lock);
  // Only drain the hardware FIFO: bytes already read into the soft ring
  // belong to the application and must survive the baud switch.
  uart_hw_drain_fifo();
  release(&uart_rx.lock);
  uart_set_divisor((uint32)baud);
  requested_baud = (uint32)baud;
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
    acquire(&uart_tx.lock);
    uart_tx_service();
    release(&uart_tx.lock);
    break;
  case UART_IIR_RDA:
  case UART_IIR_TIMEOUT:
    acquire(&uart_rx.lock);
    uart_rx_service();
    release(&uart_rx.lock);
    break;
  case UART_IIR_LSERR:
    // Line-status error (overrun/parity/framing): reading LSR clears it.
    (void)uart->LSR;
    break;
  default:
    break;  // 0x01 = no interrupt pending
  }
}
