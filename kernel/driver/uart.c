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
#include "utils.h"

static const uintptr_t uart_base[UART_DEVICE_MAX] = {
  UART0_V,
  UART1_V,
  UART2_V,
};

#define UART_LSR_DR      (1u << 0)
#define UART_LSR_OE      (1u << 1)  // RX FIFO overrun
#define UART_LSR_TX_BUSY (1u << 5)
#define UART_LSR_TEMT    (1u << 6)

#define UART_IIR_THRE    0x02
#define UART_IIR_RDA     0x04
#define UART_IIR_LSERR   0x06
#define UART_IIR_TIMEOUT 0x0c

#define UART_IER_RX      0x01
#define UART_IER_TX      0x02
#define UART_LCR_DLAB    (1u << 7)
#define UART_BRATE_CONST 16
#define UART_RXDMA_SIZE 512
#define UART_TXDMA_SIZE 256
#define UART_DMA_TIMEOUT_TICKS 2000UL

extern volatile int panicked;

// ---------- Hardware ----------

static inline volatile uint32 *
uart_reg_addr(uintptr_t base, uint32 reg)
{
  return (volatile uint32 *)(base + reg);
}

static int
uart_hw_tx_ready(struct uart_controller *uart_ctrl)
{
  uintptr_t base = uart_base[uart_ctrl->index];

  // K210 uses LSR bit 5 as 1 = busy.
  return !(readl(base, UART_REG_LSR) & UART_LSR_TX_BUSY);
}

static void
uart_hw_putc(struct uart_controller *uart_ctrl, int ch)
{
  uintptr_t base = uart_base[uart_ctrl->index];

  writel(base, UART_REG_THR, ch);
}

static int
uart_hw_getc(struct uart_controller *uart_ctrl)
{
  uintptr_t base = uart_base[uart_ctrl->index];

  if (!(readl(base, UART_REG_LSR) & UART_LSR_DR))
    return -1;
  return readl(base, UART_REG_RBR) & 0xff;
}

// Drain only the hardware RX FIFO.
static void
uart_hw_drain_fifo(struct uart_controller *uart_ctrl)
{
  while (uart_hw_getc(uart_ctrl) != -1)
    ;
}

static void
uart_rxenable(struct uart_controller *uart_ctrl, int enabled)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  uint32 ier = readl(base, UART_REG_IER);

  if (enabled)
    ier |= UART_IER_RX;
  else
    ier &= ~UART_IER_RX;
  writel(base, UART_REG_IER, ier);
}

static void
uart_txenable(struct uart_controller *uart_ctrl, int enabled)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  uint32 ier = readl(base, UART_REG_IER);

  if (enabled)
    ier |= UART_IER_TX;
  else
    ier &= ~UART_IER_TX;
  writel(base, UART_REG_IER, ier);
}

// Program the 20-bit divisor in DLH:DLL:DLF.
static void
uart_set_divisor(struct uart_controller *uart_ctrl, uint32 baud)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor = freq / baud;
  uint8 dlh = divisor >> 12;
  uint8 dll = (divisor - (dlh << 12)) / UART_BRATE_CONST;
  uint8 dlf = divisor - (dlh << 12) - dll * UART_BRATE_CONST;

  uint32 lcr = readl(base, UART_REG_LCR);

  writel(base, UART_REG_LCR, lcr | UART_LCR_DLAB);
  writel(base, UART_REG_DLH, dlh);
  writel(base, UART_REG_DLL, dll);
  writel(base, UART_REG_DLF, dlf);
  writel(base, UART_REG_LCR, lcr);
}

// Configure 8N1, FIFO and the initial RX trigger.
static void
uart_dw_init(struct uart_controller *uart_ctrl)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  uint32 ier;

  writel(base, UART_REG_LCR, 8 - 5);  // 8N1
  ier = readl(base, UART_REG_IER);
  writel(base, UART_REG_IER, ier | 0x80);
  writel(base, UART_REG_FCR, (3 << 4) | (1 << 3) | 1);
  writel(base, UART_REG_SRT, UART_SRT_ONE_CHAR);
  writel(base, UART_REG_STET, 0);
}

// ---------- UART Init ----------

void uart_dw_isr(void *data);
void uart_dma_tx_isr(void *data);
void uart_dma_rx_isr(void *data);
void
uart_init(struct uart_controller *uart_ctrl)
{
  sysctl_clock_enable(SYSCTL_CLOCK_UART1 + uart_ctrl->index);
  sysctl_reset(SYSCTL_RESET_UART1 + uart_ctrl->index);

  fpioa_set_function(uart_ctrl->tx_io, FUNC_UART1_TX + uart_ctrl->index * 2);
  fpioa_set_function(uart_ctrl->rx_io, FUNC_UART1_RX + uart_ctrl->index * 2);

  // Pull RX high to prevent an undriven line from producing 0x00 frames.
  fpioa->io[uart_ctrl->rx_io].pu = 1;

  uart_set_divisor(uart_ctrl, uart_ctrl->default_baud);
  uart_dw_init(uart_ctrl);

  initlock(&uart_ctrl->rx.lock, "uartrx");
  initlock(&uart_ctrl->tx.lock, "uarttx");
  ringbuffer_init(&uart_ctrl->rx.ring, (uint8 *)uart_ctrl->rx.buf, UART_RX_BUF_SIZE);
  ringbuffer_init(&uart_ctrl->tx.ring, (uint8 *)uart_ctrl->tx.buf, UART_TX_BUF_SIZE);
  uart_ctrl->requested_baud = uart_ctrl->default_baud;
  uart_ctrl->rx_dma_active = 0;

  irq_register(UART0_IRQ + uart_ctrl->index, uart_dw_isr, uart_ctrl);
  if (uart_ctrl->chan_tx < DMAC_CHANNEL_MAX) {
    irq_register(DMAC_CH0_IRQ + uart_ctrl->chan_tx, uart_dma_tx_isr, uart_ctrl);
  }
  if (uart_ctrl->chan_rx < DMAC_CHANNEL_MAX) {
    irq_register(DMAC_CH0_IRQ + uart_ctrl->chan_rx, uart_dma_rx_isr, uart_ctrl);
  }

  uart_set_mode(uart_ctrl, UART_MODE_DMA);
}

// ---------- UART RX ----------

// Drain RX FIFO into the ring; caller holds rx.lock.
static int
uart_rx_fifo_drain(struct uart_controller *uart_ctrl)
{
  int received = 0;
  int ch;

  while ((ch = uart_hw_getc(uart_ctrl)) != -1) {
    if (ringbuffer_push(&uart_ctrl->rx.ring, (uint8)ch))
      received++;
    else
      uart_ctrl->rx.dropped++;
  }

  return received;
}

// Wait with rx.lock held: 1 = data, 0 = flush, -1 = interrupted.
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

// Stop RX DMA and harvest its buffer with rx.lock held.
static int
uart_rx_dma_stop(struct uart_controller *uart_ctrl)
{
  uint32 n;
  int received = 0;

  dmac_channel_disable(uart_ctrl->chan_rx);
  writeq(0xffffffff, &dmac->channel[uart_ctrl->chan_rx].intclear);
  uart_ctrl->rx_dma_active = 0;

  // DMA stores one byte in each 32-bit buffer slot.
  n = ((uint32)readq(&dmac->channel[uart_ctrl->chan_rx].dar)
       - (uint32)(uintptr_t)uart_ctrl->rx_dma_buf) / 4;

  if (n > UART_RXDMA_SIZE)
    n = UART_RXDMA_SIZE;
  for (uint32 i = 0; i < n; i++) {
    if (ringbuffer_push(&uart_ctrl->rx.ring, (uint8)(uart_ctrl->rx_dma_buf[i] & 0xff)))
      received++;
    else
      uart_ctrl->rx.dropped++;
  }

  return received;
}

// Arm a new RX DMA block after the old transfer has stopped.
static void
uart_rx_dma_start(struct uart_controller *uart_ctrl)
{
  uintptr_t base = uart_base[uart_ctrl->index];

  sysctl_dma_select((sysctl_dma_channel_t)uart_ctrl->chan_rx,
                    SYSCTL_DMA_SELECT_UART1_RX_REQ + uart_ctrl->index * 2);
  dmac_set_single_mode(uart_ctrl->chan_rx, (void *)uart_reg_addr(base, UART_REG_RBR), uart_ctrl->rx_dma_buf,
                       DMAC_ADDR_NOCHANGE, DMAC_ADDR_INCREMENT,
                       DMAC_MSIZE_1, DMAC_TRANS_WIDTH_32, UART_RXDMA_SIZE);
  uart_ctrl->rx_dma_active = 1;
}

// Harvest and re-arm RX DMA with rx.lock held.
static void
uart_rx_dma(struct uart_controller *uart_ctrl)
{
  int received = uart_rx_dma_stop(uart_ctrl);

  uart_rx_dma_start(uart_ctrl);
  if (received)
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
}

// Handle RDA with rx.lock held.
static void
uart_rx_isr(struct uart_controller *uart_ctrl)
{
  if (uart_ctrl->mode == UART_MODE_DMA) {
    // Finish a block whose DMA interrupt has not run yet.
    if (uart_ctrl->rx_dma_active && dmac_is_done(uart_ctrl->chan_rx))
      uart_rx_dma(uart_ctrl);
    return;
  }

  if (uart_rx_fifo_drain(uart_ctrl))
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
}

// Handle CTI with rx.lock held.
static void
uart_rx_timeout_isr(struct uart_controller *uart_ctrl)
{
  int received = 0;

  if (uart_ctrl->mode == UART_MODE_DMA) {
    if (uart_ctrl->rx_dma_active) {
      // Stop DMA before the CPU drains the remaining FIFO data.
      received = uart_rx_dma_stop(uart_ctrl);
      received += uart_rx_fifo_drain(uart_ctrl);
      uart_rx_dma_start(uart_ctrl);
    }
  } else {
    received = uart_rx_fifo_drain(uart_ctrl);
  }

  if (received)
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
}

void
uart_dma_rx_isr(void *data)
{
  struct uart_controller *uart_ctrl = data;

  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->mode != UART_MODE_DMA || !uart_ctrl->rx_dma_active ||
      !dmac_is_done(uart_ctrl->chan_rx)) {
    release(&uart_ctrl->rx.lock);
    return;
  }

  uart_rx_dma(uart_ctrl);
  release(&uart_ctrl->rx.lock);
}

// Clear all RX data, wake readers and re-arm DMA when enabled.
void
uart_flush_rx(struct uart_controller *uart_ctrl)
{
  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->mode == UART_MODE_DMA && uart_ctrl->rx_dma_active) {
    dmac_channel_disable(uart_ctrl->chan_rx);
    writeq(0xffffffff, &dmac->channel[uart_ctrl->chan_rx].intclear);
    uart_ctrl->rx_dma_active = 0;
  }
  uart_hw_drain_fifo(uart_ctrl);
  ringbuffer_reset(&uart_ctrl->rx.ring);
  uart_ctrl->rx.dropped = 0;
  uart_ctrl->rx.overrun = 0;
  uart_ctrl->rx.epoch++;
  wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
  if (uart_ctrl->mode == UART_MODE_DMA)
    uart_rx_dma_start(uart_ctrl);
  release(&uart_ctrl->rx.lock);
}

// Return dropped, buffered, capacity, mode and overrun in info[0..4].
void
uart_get_rx_stats(struct uart_controller *uart_ctrl, uint32 *info)
{
  acquire(&uart_ctrl->rx.lock);
  info[0] = uart_ctrl->rx.dropped;
  info[1] = ringbuffer_used(&uart_ctrl->rx.ring);
  info[2] = ringbuffer_capacity(&uart_ctrl->rx.ring);
  info[3] = (uint32)uart_ctrl->mode;
  info[4] = uart_ctrl->rx.overrun;
  release(&uart_ctrl->rx.lock);
}

// ---------- UART TX ----------

static void
uart_tx_isr(struct uart_controller *uart_ctrl)
{
  uint8 ch;

  while (!ringbuffer_empty(&uart_ctrl->tx.ring) && uart_hw_tx_ready(uart_ctrl)) {
    ringbuffer_pop(&uart_ctrl->tx.ring, &ch);
    uart_hw_putc(uart_ctrl, ch);
  }

  wakeup_reason(&uart_ctrl->tx.ring, WAKEUP_DEVICE);
  uart_txenable(uart_ctrl, !ringbuffer_empty(&uart_ctrl->tx.ring));
}

void
uart_dma_tx_isr(void *data)
{
  struct uart_controller *uart_ctrl = data;

  dmac_intr(uart_ctrl->chan_tx);
}

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

// Send TXDMA_SIZE-byte chunks and wait for each DMA completion.
static int
uart_write_dma(struct uart_controller *uart_ctrl, const char *src, int n)
{
  uintptr_t base = uart_base[uart_ctrl->index];
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
    dmac_set_single_mode(uart_ctrl->chan_tx, uart_ctrl->tx_dma_buf,
                         (void *)uart_reg_addr(base, UART_REG_THR),
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

  if (uart_ctrl->mode == UART_MODE_DMA)
    return uart_write_dma(uart_ctrl, src, n);

  acquire(&uart_ctrl->tx.lock);
  for (i = 0; i < n; i++) {
    int was_empty;

    if (uart_tx_wait_room(uart_ctrl) < 0) {
      release(&uart_ctrl->tx.lock);
      return i > 0 ? i : -1;
    }
    was_empty = ringbuffer_empty(&uart_ctrl->tx.ring);
    ringbuffer_push(&uart_ctrl->tx.ring, (uint8)src[i]);
    // A newly non-empty ring starts the THRE interrupt flow.
    if (was_empty)
      uart_txenable(uart_ctrl, 1);
  }
  release(&uart_ctrl->tx.lock);
  return i;
}

// Wait until both the TX ring and UART transmitter are empty.
static void
uart_flush_tx(struct uart_controller *uart_ctrl)
{
  uintptr_t base = uart_base[uart_ctrl->index];

  acquire(&uart_ctrl->tx.lock);
  while (!ringbuffer_empty(&uart_ctrl->tx.ring)) {
    uart_txenable(uart_ctrl, 1);
    sleep(&uart_ctrl->tx.ring, &uart_ctrl->tx.lock);
  }
  release(&uart_ctrl->tx.lock);
  while (!(readl(base, UART_REG_LSR) & UART_LSR_TEMT))
    ;
}

// Drain pending traffic before switching RX and TX modes together.
int
uart_set_mode(struct uart_controller *uart_ctrl, int mode)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  int received = 0;

  if (mode != UART_MODE_PIO && mode != UART_MODE_DMA)
    return -1;
  if (mode == uart_ctrl->mode &&
      uart_ctrl->rx_dma_active == (mode == UART_MODE_DMA))
    return 0;

  uart_flush_tx(uart_ctrl);
  uart_rxenable(uart_ctrl, 0);

  acquire(&uart_ctrl->tx.lock);
  uart_txenable(uart_ctrl, 0);

  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->rx_dma_active) {
    received = uart_rx_dma_stop(uart_ctrl);
    received += uart_rx_fifo_drain(uart_ctrl);
  }

  uart_ctrl->mode = mode;
  if (mode == UART_MODE_DMA) {
    writel(base, UART_REG_SRT, UART_SRT_HALF_FULL);
    uart_rx_dma_start(uart_ctrl);
  } else {
    writel(base, UART_REG_SRT, UART_SRT_ONE_CHAR);
  }
  if (received)
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
  release(&uart_ctrl->rx.lock);
  release(&uart_ctrl->tx.lock);

  uart_rxenable(uart_ctrl, 1);
  return 0;
}

// ---------- Baud rate ----------

void
uart_set_baud(struct uart_controller *uart_ctrl, int baud)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  int received = 0;

  if (baud <= 0)
    return;

  // Preserve the RX interrupt state across the baud change.
  int rx_was_on = (readl(base, UART_REG_IER) & UART_IER_RX) != 0;

  uart_flush_tx(uart_ctrl);
  uart_rxenable(uart_ctrl, 0);
  acquire(&uart_ctrl->rx.lock);
  if (uart_ctrl->mode == UART_MODE_DMA && uart_ctrl->rx_dma_active) {
    received = uart_rx_dma_stop(uart_ctrl);
    received += uart_rx_fifo_drain(uart_ctrl);
  } else {
    uart_hw_drain_fifo(uart_ctrl);  // Keep bytes already in the ring.
  }
  uart_set_divisor(uart_ctrl, (uint32)baud);
  uart_ctrl->requested_baud = (uint32)baud;
  if (uart_ctrl->mode == UART_MODE_DMA)
    uart_rx_dma_start(uart_ctrl);
  if (received)
    wakeup_reason(&uart_ctrl->rx.ring, WAKEUP_DEVICE);
  release(&uart_ctrl->rx.lock);
  if (rx_was_on)
    uart_rxenable(uart_ctrl, 1);
}

void
uart_get_baud_info(struct uart_controller *uart_ctrl, uint32 *info)
{
  uintptr_t base = uart_base[uart_ctrl->index];
  uint32 freq = sysctl_clock_get_freq(SYSCTL_CLOCK_APB0);
  uint32 divisor;
  uint32 lcr;

  // DLAB selects divisor registers instead of their IER/RBR aliases.
  lcr = readl(base, UART_REG_LCR);
  writel(base, UART_REG_LCR, lcr | UART_LCR_DLAB);
  divisor = ((readl(base, UART_REG_DLH) & 0xff) << 12) |
            ((readl(base, UART_REG_DLL) & 0xff) << 4) |
            (readl(base, UART_REG_DLF) & 0xf);
  writel(base, UART_REG_LCR, lcr);

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
  uintptr_t base = uart_base[uart_ctrl->index];
  uint8 status = readl(base, UART_REG_IIR) & 0xf;

  switch (status) {
  case UART_IIR_THRE:
    if (uart_ctrl->mode == UART_MODE_DMA)
      break;  // DMA owns TX FIFO.
    acquire(&uart_ctrl->tx.lock);
    uart_tx_isr(uart_ctrl);
    release(&uart_ctrl->tx.lock);
    break;
  case UART_IIR_RDA:
    acquire(&uart_ctrl->rx.lock);
    uart_rx_isr(uart_ctrl);
    release(&uart_ctrl->rx.lock);
    break;
  case UART_IIR_TIMEOUT:  // CTI: RX FIFO became idle with data.
    acquire(&uart_ctrl->rx.lock);
    uart_rx_timeout_isr(uart_ctrl);
    release(&uart_ctrl->rx.lock);
    break;
  case UART_IIR_LSERR:
    // Reading LSR clears the line error.
    acquire(&uart_ctrl->rx.lock);
    if (readl(base, UART_REG_LSR) & UART_LSR_OE)
      uart_ctrl->rx.overrun++;
    release(&uart_ctrl->rx.lock);
    break;
  default:
    break;  // 0x01 means no pending interrupt.
  }
}
