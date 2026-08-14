/* Board-level UART definitions -- controller instances.
 *
 * The generic DW UART driver (kernel/driver/uart.c) operates purely on a
 * struct uart_controller *; this file builds each instance's hardware identity
 * (base, fpioa pins, DMA channels) plus its embedded runtime state, exposed
 * via the extern table uart_ctrls[] declared in uart_board.h.  The table is
 * indexed by device number, which doubles as the devsw minor.
 *
 * Only UART1 is enabled today (DMA RX on CH5, IRQ 32).  UART2/3 stay NULL:
 * enabling them is a board-table entry + one irq_register (see
 * doc/重构文档/7.uart多实例重构方案.md).
 */
#include "dmac.h"
#include "memlayout.h"
#include "uart.h"
#include "uart-dw.h"
#include "uart_board.h"

/* UART1: IO7=TX, IO8=RX, DMA CH4(TX)/CH5(RX). */
static struct uart_controller uart_ctrl_1 = {
  .hw = (volatile uart_t *)UART_V,
  .index = UART_DEVICE_1,
  .tx_io = 7,
  .rx_io = 8,
  .chan_tx = DMAC_CHANNEL4,
  .chan_rx = DMAC_CHANNEL5,
  .default_baud = 115200,
};

/* Public controller table -- indexed by device number (devsw minor).
 * UART2/3 slots stay NULL until enabled. */
struct uart_controller *uart_ctrls[UART_DEVICE_MAX] = {
  [UART_DEVICE_1] = &uart_ctrl_1,
};
