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

static volatile uart_t *const uart_base[UART_DEVICE_MAX] = {
  (volatile uart_t *)UART0_V,
  (volatile uart_t *)UART1_V,
  (volatile uart_t *)UART2_V,
};

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
#define UART_RXDMA_SIZE 512
#define UART_TXDMA_SIZE 256
#define UART_DMA_TIMEOUT_TICKS 2000UL

extern volatile int panicked;

// ---------- Hardware Operation ----------

static int
uart_hw_getc(struct uart_controller *uart_ctrl)
{
  if (!(uart_base[uart_ctrl->index]->LSR & UART_LSR_DR))
    return -1;
  return uart_base[uart_ctrl->index]->RBR & 0xff;
}

static int
uart_hw_tx_ready(struct uart_controller *uart_ctrl)
{
  // K210 inverts LSR bit 5: 1 = busy, 0 = ready to accept a byte.
  return !(uart_base[uart_ctrl->index]->LSR & UART_LSR_TX_BUSY);
}

static void
uart_hw_putc(struct uart_controller *uart_ctrl, int ch)
{
  uart_base[uart_ctrl->index]->THR = ch;
}

// Drain the hardware RX FIFO (flush/baud switch); the soft ring is untouched.
static void
uart_hw_drain_fifo(struct uart_controller *uart_ctrl)
{
  while (uart_hw_getc(uart_ctrl) != -1)
    ;
}

// RX-enable is internal state, toggled only around baud/mode switches and at
// init.  TX-enable is owned by the tx service loop (on while the ring has data).
static void
uart_rxenable(struct uart_controller *uart_ctrl, int enabled)
{
  uint32 ier = uart_base[uart_ctrl->index]->IER;

  if (enabled)
    ier |= UART_IER_RX;
  else
    ier &= ~UART_IER_RX;
  uart_base[uart_ctrl->index]->IER = ier;
}

static void
uart_txenable(struct uart_controller *uart_ctrl, int enabled)
{
  uint32 ier = uart_base[uart_ctrl->index]->IER;

  if (enabled)
    ier |= UART_IER_TX;
  else
    ier &= ~UART_IER_TX;
  uart_base[uart_ctrl->index]->IER = ier;
}

// Program the 20-bit divisor (DLH:DLL:DLF): the APB0 clock is divided by
// (DLH<<12 | DLL<<4 | DLF), integer in DLH/DLL, fraction in the 4-bit DLF.
static void
uart_set_divisor(struct uart_controller *uart_ctrl, uint32 baud)
{
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor = freq / baud;
  uint8 dlh = divisor >> 12;
  uint8 dll = (divisor - (dlh << 12)) / UART_BRATE_CONST;
  uint8 dlf = divisor - (dlh << 12) - dll * UART_BRATE_CONST;

  uart_base[uart_ctrl->index]->LCR |= (1u << 7);       // DLAB: latch DLL/DLH/DLF
  uart_base[uart_ctrl->index]->DLH = dlh;
  uart_base[uart_ctrl->index]->DLL = dll;
  uart_base[uart_ctrl->index]->DLF = dlf;
  uart_base[uart_ctrl->index]->LCR &= ~(1u << 7);      // clear DLAB, keep the current LCR format
}

// 8N1 + FIFO on + RX trigger at 1 byte (SDK defaults).  DLAB must be clear,
// so this must run after any uart_set_divisor() that uses the LCR register.
static void
uart_configure_line(struct uart_controller *uart_ctrl)
{
  uart_base[uart_ctrl->index]->LCR = (8 - 5);          // 8N1: (data_width-5) | stop<<2 | parity<<3
  uart_base[uart_ctrl->index]->IER |= 0x80;            // keep the SDK's THRE bit
  uart_base[uart_ctrl->index]->FCR = (0 << 6) | (3 << 4) | (1 << 3) | 1;  // FIFO on, RX trig 1, TX trig 8
  uart_base[uart_ctrl->index]->SRT = 0;                // receive FIFO trigger = 1 byte (UART_RECEIVE_FIFO_1)
  uart_base[uart_ctrl->index]->STET = 0;
}

// ---------- UART Init ----------
void uart_dw_isr(void *data);
void uart_dma_tx_isr(void *data);
void uart_dma_rx_isr(void *data);
void
uartinit(struct uart_controller *uart_ctrl)
{
  sysctl_clock_enable(SYSCTL_CLOCK_UART1 + uart_ctrl->index);
  sysctl_reset(SYSCTL_RESET_UART1 + uart_ctrl->index);

  fpioa_set_function(uart_ctrl->tx_io, FUNC_UART1_TX + uart_ctrl->index * 2);
  fpioa_set_function(uart_ctrl->rx_io, FUNC_UART1_RX + uart_ctrl->index * 2);

  // FUNC_UART1_RX is a floating input (pu=0); a low undriven line is framed as
  // an endless 0x00 stream.  Pull it up so idle reads as high.
  fpioa->io[uart_ctrl->rx_io].pu = 1;

  uart_set_divisor(uart_ctrl, uart_ctrl->default_baud);
  uart_configure_line(uart_ctrl);

  initlock(&uart_ctrl->rx.lock, "uartrx");
  initlock(&uart_ctrl->tx.lock, "uarttx");
  ringbuffer_init(&uart_ctrl->rx.ring, (uint8 *)uart_ctrl->rx.buf, UART_RX_BUF_SIZE);
  ringbuffer_init(&uart_ctrl->tx.ring, (uint8 *)uart_ctrl->tx.buf, UART_TX_BUF_SIZE);
  uart_ctrl->requested_baud = uart_ctrl->default_baud;
  uart_ctrl->rx_mode = UART_MODE_DMA;
  uart_ctrl->tx_mode = UART_MODE_DMA;
  uart_ctrl->rx_dma_active = 0;

  irq_register(UART0_IRQ + uart_ctrl->index, uart_dw_isr, uart_ctrl);
  if (uart_ctrl->chan_tx < DMAC_CHANNEL_MAX)
    irq_register(DMAC_CH0_IRQ + uart_ctrl->chan_tx, uart_dma_tx_isr, uart_ctrl);
  if (uart_ctrl->chan_rx < DMAC_CHANNEL_MAX)
    irq_register(DMAC_CH0_IRQ + uart_ctrl->chan_rx, uart_dma_rx_isr, uart_ctrl);

  uart_set_rx_mode(uart_ctrl, UART_MODE_DMA);
}

// ---------- UART RX ----------

// Drain the hardware FIFO into the rx ring, count drops, wake readers.
// Caller must hold uart_ctrl->rx.lock.
static void
uart_rx_service(struct uart_controller *uart_ctrl)
{
  int received = 0;
  int ch;

  while ((ch = uart_hw_getc(uart_ctrl)) != -1) {
    if (ringbuffer_push(&uart_ctrl->rx.ring, (uint8)ch))
      received = 1;
    else
      uart_ctrl->rx.dropped++;
  }

  if (received)
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
}

// Block for rx data.  1 = data; 0 = flush bumped the epoch; -1 = interrupted.
// Caller must hold uart_ctrl->rx.lock.
static int
uart_rx_wait_data(struct uart_controller *uart_ctrl, uint epoch)
{
  struct proc *p = myproc();

  for (;;) {
    if (!ringbuffer_empty(&uart_ctrl->rx.ring))
      return 1;
    if (p && sleep_interruptible(&uart_ctrl->rx.ring, &uart_ctrl->rx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uart_ctrl->rx.ring, &uart_ctrl->rx.lock);
    if (epoch != uart_ctrl->rx.epoch)
      return 0;
  }
}

int
uart_read(struct uart_controller *uart_ctrl, char *dst, int n)
{
  int i = 0;
  int reason;
  uint epoch;

  if (n <= 0)
    return 0;

  acquire(&uart_ctrl->rx.lock);
  epoch = uart_ctrl->rx.epoch;
  reason = uart_rx_wait_data(uart_ctrl, epoch);
  if (reason != 1) {
    release(&uart_ctrl->rx.lock);
    return reason;
  }
  while (i < n && ringbuffer_pop(&uart_ctrl->rx.ring, (uint8 *)&dst[i]))
    i++;
  release(&uart_ctrl->rx.lock);
  return i;
}

// ---- RX DMA (boundary-event-driven frame harvesting) ----
static void
uart_rx_dma_harvest(struct uart_controller *uart_ctrl)
{
  uint32 n = ((uint32)(uintptr_t)dmac->channel[uart_ctrl->chan_rx].dar
              - (uint32)(uintptr_t)uart_ctrl->rx_dma_buf) / 4;
  int received = 0;
  int ch;

  if (n > UART_RXDMA_SIZE)
    n = UART_RXDMA_SIZE;         // clamp
  for (uint32 i = 0; i < n; i++) {
    if (ringbuffer_push(&uart_ctrl->rx.ring, (uint8)(uart_ctrl->rx_dma_buf[i] & 0xff)))
      received = 1;
    else
      uart_ctrl->rx.dropped++;
  }
  while ((ch = uart_hw_getc(uart_ctrl)) != -1) {
    if (ringbuffer_push(&uart_ctrl->rx.ring, (uint8)ch))
      received = 1;
    else
      uart_ctrl->rx.dropped++;
  }

  if (received)
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
}

// Arm a fresh RX block on this instance's RX channel (lock held).  Caller
// must have stopped the old transfer so dmac_set_single_mode's internal
// dmac_wait_idle is a no-op.
static void
uart_rx_dma_start(struct uart_controller *uart_ctrl)
{
  sysctl_dma_select((sysctl_dma_channel_t)uart_ctrl->chan_rx,
                    SYSCTL_DMA_SELECT_UART1_RX_REQ + uart_ctrl->index * 2);
  dmac_set_single_mode(uart_ctrl->chan_rx, (void *)&uart_base[uart_ctrl->index]->RBR, uart_ctrl->rx_dma_buf,
                       DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
                       DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, UART_RXDMA_SIZE);
  uart_ctrl->rx_dma_active = 1;
}

void
uart_dma_tx_isr(void *data)
{
  struct uart_controller *uart_ctrl = data;

  dmac_intr(uart_ctrl->chan_tx);
}

void
uart_dma_rx_isr(void *data)
{
  struct uart_controller *uart_ctrl = data;

  dmac->channel[uart_ctrl->chan_rx].intclear = 0xffffffff;
  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->rx_mode == UART_MODE_DMA && uart_ctrl->rx_dma_active) {
    dmac_channel_disable(uart_ctrl->chan_rx);
    uart_rx_dma_harvest(uart_ctrl);
    uart_rx_dma_start(uart_ctrl);
  }
  release(&uart_ctrl->rx.lock);
}

// Discard the FIFO (and in-flight DMA) and soft ring, reset rx state, then
// wake readers so uart_read returns 0.  In DMA mode the harvested buffer is
// dropped and the RX channel re-armed to keep the stream continuous.
void
uart_flush_rx(struct uart_controller *uart_ctrl)
{
  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->rx_mode == UART_MODE_DMA && uart_ctrl->rx_dma_active) {
    dmac_channel_disable(uart_ctrl->chan_rx);   // residual bytes are discarded
  } else {
    uart_hw_drain_fifo(uart_ctrl);
  }
  ringbuffer_reset(&uart_ctrl->rx.ring);
  uart_ctrl->rx.dropped = 0;
  uart_ctrl->rx.overrun = 0;
  uart_ctrl->rx.epoch++;
  wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
  if (uart_ctrl->rx_mode == UART_MODE_DMA && uart_ctrl->rx_dma_active)
    uart_rx_dma_start(uart_ctrl);
  release(&uart_ctrl->rx.lock);
}

// info[0]=dropped, [1]=buffered, [2]=ring capacity, [3]=RX mode (INT/DMA),
// [4]=hardware overruns (LSR OE).
void
uart_get_rx_stats(struct uart_controller *uart_ctrl, uint32 *info)
{
  acquire(&uart_ctrl->rx.lock);
  info[0] = uart_ctrl->rx.dropped;
  info[1] = ringbuffer_used(&uart_ctrl->rx.ring);
  info[2] = ringbuffer_capacity(&uart_ctrl->rx.ring);
  info[3] = (uint32)uart_ctrl->rx_mode;
  info[4] = uart_ctrl->rx.overrun;
  release(&uart_ctrl->rx.lock);
}

// Switch the RX path, preserving in-flight bytes (stop the DMA and harvest
// first).  DMA mode uses an 8-byte trigger (SRT=2) so short bursts stay in the
// FIFO and close via CTI instead of raising an RDA every byte.
void
uart_set_rx_mode(struct uart_controller *uart_ctrl, int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == uart_ctrl->rx_mode && uart_ctrl->rx_dma_active == (mode == UART_MODE_DMA))
    return;

  uart_rxenable(uart_ctrl, 0);   // quiet RDA/CTI while reconfiguring
  acquire(&uart_ctrl->rx.lock);
  // Harvest only if armed: on a never-armed channel dar holds a stale address
  // and (stale - base)/4 would push hundreds of bogus zero words into the ring.
  if (uart_ctrl->rx_dma_active) {
    dmac_channel_disable(uart_ctrl->chan_rx);
    uart_rx_dma_harvest(uart_ctrl);       // preserve bytes already in flight
  }
  if (mode == UART_MODE_DMA) {
    uart_base[uart_ctrl->index]->SRT = 2;               // 8-byte trigger: short bursts -> CTI
    uart_rx_dma_start(uart_ctrl);
  } else {
    uart_base[uart_ctrl->index]->SRT = 0;               // 1-byte trigger for the interrupt path
    uart_ctrl->rx_dma_active = 0;
  }
  uart_ctrl->rx_mode = mode;
  release(&uart_ctrl->rx.lock);
  uart_rxenable(uart_ctrl, 1);
}

// ---------- UART TX ----------

static void
uart_tx_service(struct uart_controller *uart_ctrl)
{
  uint8 ch;

  while (!ringbuffer_empty(&uart_ctrl->tx.ring) && uart_hw_tx_ready(uart_ctrl)) {
    ringbuffer_pop(&uart_ctrl->tx.ring, &ch);
    uart_hw_putc(uart_ctrl, ch);
  }

  wakeup_reason(&uart_ctrl->tx.ring, WAKEUP_DEVICE);
  uart_txenable(uart_ctrl, !ringbuffer_empty(&uart_ctrl->tx.ring));
}

// Block until the tx ring has room.
static int
uart_tx_wait_room(struct uart_controller *uart_ctrl)
{
  struct proc *p = myproc();

  while (ringbuffer_full(&uart_ctrl->tx.ring)) {
    if (panicked)
      return -1;
    if (p && sleep_interruptible(&uart_ctrl->tx.ring, &uart_ctrl->tx.lock) < 0)
      return -1;
    if (!p)
      sleep(&uart_ctrl->tx.ring, &uart_ctrl->tx.lock);
  }
  return 0;
}

// TX via DMA: pack up to TXDMA_SIZE bytes into words, arm the TX channel,
// block on the DMAC CH4 completion IRQ (dmac_wait_idle_timeout).
static int
uart_write_dma(struct uart_controller *uart_ctrl, const char *src, int n)
{
  int done = 0;

  while (done < n) {
    int chunk = n - done;
    int i;

    if (chunk > UART_TXDMA_SIZE)
      chunk = UART_TXDMA_SIZE;
    acquire(&uart_ctrl->tx.lock);
    for (i = 0; i < chunk; i++)
      uart_ctrl->tx_dma_buf[i] = (uint32)(uint8)src[done + i];
    sysctl_dma_select((sysctl_dma_channel_t)uart_ctrl->chan_tx,
                      SYSCTL_DMA_SELECT_UART1_TX_REQ + uart_ctrl->index * 2);
    dmac_set_single_mode(uart_ctrl->chan_tx, uart_ctrl->tx_dma_buf, (void *)&uart_base[uart_ctrl->index]->THR,
                         DMAC_ADDR_INCREMENT, DMAC_ADDR_NOCHANGE,
                         DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, (uint64)chunk);
    release(&uart_ctrl->tx.lock);
    if (dmac_wait_idle_timeout(uart_ctrl->chan_tx, UART_DMA_TIMEOUT_TICKS) < 0)
      return done > 0 ? done : -1;
    done += chunk;
  }
  return done;
}

int
uart_write(struct uart_controller *uart_ctrl, const char *src, int n)
{
  int i;

  if (uart_ctrl->tx_mode == UART_MODE_DMA)
    return uart_write_dma(uart_ctrl, src, n);

  acquire(&uart_ctrl->tx.lock);
  for (i = 0; i < n; i++) {
    if (uart_tx_wait_room(uart_ctrl) < 0) {
      release(&uart_ctrl->tx.lock);
      return i > 0 ? i : -1;
    }
    ringbuffer_push(&uart_ctrl->tx.ring, (uint8)src[i]);
    uart_tx_service(uart_ctrl);
  }
  release(&uart_ctrl->tx.lock);
  return i;
}

// Wait for the tx ring and hardware FIFO to drain; used by the baud switch and
// the switch into DMA TX so neither changes the line mid-frame.  In DMA mode
// the ring is bypassed, so this reduces to waiting TEMT.
static void
uart_flush_tx(struct uart_controller *uart_ctrl)
{
  acquire(&uart_ctrl->tx.lock);
  while (!ringbuffer_empty(&uart_ctrl->tx.ring)) {
    uart_tx_service(uart_ctrl);
    if (!ringbuffer_empty(&uart_ctrl->tx.ring))
      sleep(&uart_ctrl->tx.ring, &uart_ctrl->tx.lock);
  }
  release(&uart_ctrl->tx.lock);
  while (!(uart_base[uart_ctrl->index]->LSR & UART_LSR_TEMT))
    ;
}

// Switch the TX path.  DMA mode flushes any bytes queued while still in INT
// mode, then turns the THRE interrupt off -- DMA owns the FIFO.  STET stays 1:
// it drives both THRE (INT) and DMATXREQ (DMA).
void
uart_set_tx_mode(struct uart_controller *uart_ctrl, int mode)
{
  if (mode != UART_MODE_INT && mode != UART_MODE_DMA)
    return;
  if (mode == uart_ctrl->tx_mode)
    return;

  if (mode == UART_MODE_DMA)
    uart_flush_tx(uart_ctrl);
  acquire(&uart_ctrl->tx.lock);
  uart_txenable(uart_ctrl, 0);
  uart_ctrl->tx_mode = mode;
  release(&uart_ctrl->tx.lock);
}

// ---------- baudrate setup ----------

void
uart_set_baud(struct uart_controller *uart_ctrl, int baud)
{
  if (baud <= 0)
    return;

  // Restore the prior RX interrupt state: forcing it on lets the ISR drain the
  // FIFO before the next read (the "loop reads back 0 bytes" bug).
  int rx_was_on = (uart_base[uart_ctrl->index]->IER & UART_IER_RX) != 0;

  uart_flush_tx(uart_ctrl);
  uart_rxenable(uart_ctrl, 0);
  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->rx_mode == UART_MODE_DMA && uart_ctrl->rx_dma_active) {
    dmac_channel_disable(uart_ctrl->chan_rx);   // stop DMA, keep harvested bytes
    uart_rx_dma_harvest(uart_ctrl);
  } else {
    uart_hw_drain_fifo(uart_ctrl);              // bytes in the soft ring are kept
  }
  uart_set_divisor(uart_ctrl, (uint32)baud);
  uart_ctrl->requested_baud = (uint32)baud;
  if (uart_ctrl->rx_mode == UART_MODE_DMA && uart_ctrl->rx_dma_active)
    uart_rx_dma_start(uart_ctrl);   // re-arm on the new line rate
  release(&uart_ctrl->rx.lock);
  if (rx_was_on)
    uart_rxenable(uart_ctrl, 1);
}

void
uart_get_baud_info(struct uart_controller *uart_ctrl, uint32 *info)
{
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor;

  // DLH/DLL alias IER/RBR and only decode as the divisor latch while DLAB is
  // set -- without it the readback silently returns IER/RBR contents.
  uart_base[uart_ctrl->index]->LCR |= (1u << 7);   // DLAB on: DLH/DLL/DLF visible at 0x04/0x00/0xc0
  divisor = ((uart_base[uart_ctrl->index]->DLH & 0xff) << 12) | ((uart_base[uart_ctrl->index]->DLL & 0xff) << 4) | (uart_base[uart_ctrl->index]->DLF & 0xf);
  uart_base[uart_ctrl->index]->LCR &= ~(1u << 7);  // DLAB off, line format preserved

  info[0] = uart_ctrl->requested_baud;
  info[1] = divisor ? freq / divisor : 0;
  info[2] = divisor;
  info[3] = freq;
}

// ---------- handler ----------

void
uart_dw_isr(void *data)
{
  struct uart_controller *uart_ctrl = data;
  uint8 status = uart_base[uart_ctrl->index]->IIR & 0xf;

  switch (status) {
  case UART_IIR_THRE:
    if (uart_ctrl->tx_mode == UART_MODE_DMA)
      break;   // DMA TX owns the FIFO; a stale THRE is spurious
    acquire(&uart_ctrl->tx.lock);
    uart_tx_service(uart_ctrl);
    release(&uart_ctrl->tx.lock);
    break;
  case UART_IIR_RDA:
    if (uart_ctrl->rx_mode == UART_MODE_DMA)
      break;   // DMA drains the FIFO; done/CTI harvest.  Harvesting on RDA
               // disables the RX channel with dar mid-transfer and races the
               // DMA-done ISR, corrupting frames (seen as 1.5M burn CRC errors).
    acquire(&uart_ctrl->rx.lock);
    uart_rx_service(uart_ctrl);
    release(&uart_ctrl->rx.lock);
    break;
  case UART_IIR_TIMEOUT:   // CTI: FIFO idle with bytes left
    if (uart_ctrl->rx_mode == UART_MODE_DMA) {
      // A frame tail too short to fill a DMA block closes via CTI.
      uart_dma_rx_isr(uart_ctrl);
    } else {
      acquire(&uart_ctrl->rx.lock);
      uart_rx_service(uart_ctrl);
      release(&uart_ctrl->rx.lock);
    }
    break;
  case UART_IIR_LSERR:
    // Line-status error (overrun/parity/framing): reading LSR clears it.
    acquire(&uart_ctrl->rx.lock);
    if (uart_base[uart_ctrl->index]->LSR & UART_LSR_OE)
      uart_ctrl->rx.overrun++;
    release(&uart_ctrl->rx.lock);
    break;
  default:
    break;  // 0x01 = no interrupt pending
  }
}