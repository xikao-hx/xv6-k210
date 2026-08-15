/* Board-level UART definitions — controller instances.
 *
 * The generic DW UART driver (kernel/driver/uart.c) accesses controllers via
 * the extern table uart_ctrls[] declared in uart_board.h.  Each entry is a
 * self-describing struct uart_controller: hardware base, index (derives the
 * sysctl clock/reset/DMA-select), board pins, DMA channels and default baud.
 *
 * UART2/3 capability is wired (index/pins/DMA-select derive from the entry),
 * but the slots are left NULL: those UARTs are not enabled this round.
 */
#include "dmac.h"
#include "uart_board.h"

/* UART1: index derives the register base (uart.c table) + sysctl
 * clock/reset/DMA-select; IO7=TX, IO8=RX, DMA CH4(TX)/CH5(RX), 115200 default. */
static struct uart_controller uart_ctrl_1 = {
  .index = UART_DEVICE_1,
  .tx_io = 7,
  .rx_io = 8,
  .chan_tx = DMAC_CHANNEL4,
  .chan_rx = DMAC_CHANNEL5,
  .default_baud = 115200,
};

/* Public controller table — the generic driver indexes this by minor. */
struct uart_controller *uart_ctrls[UART_DEVICE_MAX] = {
  [UART_DEVICE_1] = &uart_ctrl_1,   /* UART2/3 槽位留 NULL：未启用 */
};
