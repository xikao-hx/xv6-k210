// /dev/uart1 character device on top of the uart byte-stream driver.
//
// Each devsw minor maps to one struct uart_controller via the board-layer
// table uart_ctrls[] (index == minor, see kernel/board/uart_board.c).  The
// chosen controller is stashed in f->private_data at open and recovered in
// read/write/ioctl, so a single devsw entry serves every enabled UART instance
// without any driver-side dispatch table.
//
// ioctl command codes intentionally match CONSOLE_IOCTL_* so burn.c can swap
// its device entry without touching protocol logic.  Semantics are
// independent: uart is a raw byte stream, so SET_MODE only accepts RAW.
// The RX/TX interrupt-vs-DMA path is chosen per direction with
// UART_IOCTL_SET_RX_MODE / UART_IOCTL_SET_TX_MODE (UART_MODE_INT/DMA), and
// GET_RX_STATS reports the active RX mode in info[3].

#include "console.h"
#include "file.h"
#include "printf.h"
#include "proc.h"
#include "uart.h"
#include "uart_board.h"
#include "dev.h"

#define UART_IO_CHUNK 128

static struct uart_controller *
uartdev_ctrl(struct file *f)
{
  struct uart_controller *c = f->private_data;

  // A fd opened against an enabled UART always carries its controller; a NULL
  // here means open-time routing was bypassed.  Open already refused unknown
  // minors, so this is defense-in-depth, not a dispatch path.
  if (c == 0)
    panic("uartdev: no controller for fd");
  return c;
}

static int
uartdev_read(struct file *f, uint64 dst, int n)
{
  struct uart_controller *c = uartdev_ctrl(f);
  char buf[UART_IO_CHUNK];
  int count = n;
  int got;

  if (count > sizeof(buf))
    count = sizeof(buf);

  got = uart_read(c, buf, count);
  if (got <= 0)
    return got;
  if (either_copyout(1, dst, buf, got) < 0)
    return -1;
  return got;
}

static int
uartdev_write(struct file *f, uint64 src, int n)
{
  struct uart_controller *c = uartdev_ctrl(f);
  char buf[UART_IO_CHUNK];
  int done = 0;

  while (done < n) {
    int count = n - done;
    int written;

    if (count > sizeof(buf))
      count = sizeof(buf);
    if (either_copyin(buf, 1, src + done, count) < 0)
      break;
    written = uart_write(c, buf, count);
    if (written <= 0)
      break;
    done += written;
    if (written != count)
      break;
  }
  return done;
}

static int
uartdev_ioctl(struct file *f, uint64 cmd, uint64 arg)
{
  struct uart_controller *c = uartdev_ctrl(f);
  uint32 info[4];

  switch (cmd) {
  case CONSOLE_IOCTL_FLUSH_INPUT:
    uart_flush_rx(c);
    return 0;
  case CONSOLE_IOCTL_SET_MODE:
    // RAW = the byte-stream device (interrupt or DMA path per direction,
    // chosen with the *_MODE ioctls).  TTY line editing is not meaningful on
    // a raw UART; POLL was removed with Step 2.
    if (arg == CONSOLE_MODE_RAW)
      return 0;
    return -1;
  case CONSOLE_IOCTL_GET_MODE:
    return CONSOLE_MODE_RAW;
  case CONSOLE_IOCTL_SET_BAUD:
    if (arg == 0 || arg > 5000000)
      return -1;
    uart_set_baud(c, (int)arg);
    return 0;
  case CONSOLE_IOCTL_GET_BAUD_INFO:
    uart_get_baud_info(c, info);
    return either_copyout(1, arg, info, sizeof(info));
  case CONSOLE_IOCTL_GET_RX_STATS: {
    uint32 st[5];              // dropped/buffered/capacity/mode/overrun
    uart_get_rx_stats(c, st);
    return either_copyout(1, arg, st, sizeof(st));
  }
  case UART_IOCTL_SET_RX_MODE:
    uart_set_rx_mode(c, (int)arg);
    return 0;
  case UART_IOCTL_SET_TX_MODE:
    uart_set_tx_mode(c, (int)arg);
    return 0;
  default:
    return -1;
  }
}

static int
uartdev_open(struct file *f)
{
  struct uart_controller *c = (f->minor >= 0 && f->minor < UART_DEVICE_MAX)
                              ? uart_ctrls[f->minor] : 0;

  if (c == 0)
    return -1;                 // /dev/uart1 = minor 0; 1/2 are NULL slots
  f->private_data = c;
  return 0;
}

static const struct file_operations uart_ops = {
  .open = uartdev_open,
  .read = uartdev_read,
  .write = uartdev_write,
  .ioctl = uartdev_ioctl,
};

void
uartdev_init(void)
{
  // Initialize every enabled instance from the board table (only UART1 today,
  // with its own DMA RX on CH5/IRQ32).  NULL slots are untouched so enabling
  // UART2/3 is a board-table edit, not a driver change.
  for (int i = 0; i < UART_DEVICE_MAX; i++) {
    if (uart_ctrls[i])
      uartinit(uart_ctrls[i]);
  }
  if(device_register(DEV_UART1, "uart1", &uart_ops) < 0)
    panic("uart1 device register");
}