// /dev/uart1 character device on top of the uart byte-stream driver.
//
// ioctl command codes intentionally match CONSOLE_IOCTL_* so burn.c can swap
// its device entry without touching protocol logic.  Semantics are
// independent: uart is a raw byte stream, so SET_MODE only accepts RAW.

#include "console.h"
#include "file.h"
#include "printf.h"
#include "proc.h"
#include "uart.h"
#include "dev.h"

#define UART_IO_CHUNK 128
// Per-byte poll budget when reading in POLL mode.  Each spin is a few
// instructions; at K210's ~400MHz this is roughly a millisecond per byte,
// after which the read gives up and returns what it has.
#define UART_POLL_SPIN 500000

// Device mode.  RAW = interrupt-driven ring buffer; POLL = direct hardware
// access with interrupts disabled (see uart_poll_*), used to validate the
// TX/RX path independent of the interrupt machinery.
static int uart_mode = CONSOLE_MODE_RAW;

static int
uartdev_read(struct file *f, uint64 dst, int n)
{
  char buf[UART_IO_CHUNK];
  int count = n;
  int got;

  (void)f;
  if (count > sizeof(buf))
    count = sizeof(buf);

  if (uart_mode == CONSOLE_MODE_POLL) {
    int i;
    for (i = 0; i < count; i++) {
      int c = -1;
      for (volatile int spin = 0; spin < UART_POLL_SPIN; spin++) {
        c = uart_poll_getc();
        if (c >= 0)
          break;
      }
      if (c < 0)
        break;   // timed out: hand back whatever arrived
      buf[i] = (char)c;
    }
    got = i;
  } else {
    got = uart_read(buf, count);
  }
  if (got <= 0)
    return got;
  if (either_copyout(1, dst, buf, got) < 0)
    return -1;
  return got;
}

static int
uartdev_write(struct file *f, uint64 src, int n)
{
  char buf[UART_IO_CHUNK];
  int done = 0;

  (void)f;
  while (done < n) {
    int count = n - done;
    int written;

    if (count > sizeof(buf))
      count = sizeof(buf);
    if (either_copyin(buf, 1, src + done, count) < 0)
      break;
    if (uart_mode == CONSOLE_MODE_POLL) {
      for (int i = 0; i < count; i++)
        uart_poll_putc(buf[i]);
      written = count;
    } else {
      written = uart_write(buf, count);
    }
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
  uint32 info[4];

  (void)f;
  switch (cmd) {
  case CONSOLE_IOCTL_FLUSH_INPUT:
    uart_flush_rx();
    return 0;
  case CONSOLE_IOCTL_SET_MODE:
    // RAW = interrupt-driven byte stream; POLL = direct hardware access with
    // interrupts off, for validating the wire independent of the ISR.  TTY
    // line editing is not meaningful on a raw UART.
    if (arg == CONSOLE_MODE_RAW) {
      uart_mode = CONSOLE_MODE_RAW;
      uart_set_poll(0);
      return 0;
    }
    if (arg == CONSOLE_MODE_POLL) {
      uart_mode = CONSOLE_MODE_POLL;
      uart_set_poll(1);
      return 0;
    }
    return -1;
  case CONSOLE_IOCTL_GET_MODE:
    return uart_mode;
  case CONSOLE_IOCTL_SET_BAUD:
    if (arg == 0 || arg > 5000000)
      return -1;
    uart_set_baud((int)arg);
    return 0;
  case CONSOLE_IOCTL_GET_BAUD_INFO:
    uart_get_baud_info(info);
    return either_copyout(1, arg, info, sizeof(info));
  case CONSOLE_IOCTL_GET_RX_STATS:
    uart_get_rx_stats(info);
    info[3] = uart_mode;
    return either_copyout(1, arg, info, sizeof(info));
  default:
    return -1;
  }
}

static int
uartdev_open(struct file *f)
{
  return f->minor == 0 ? 0 : -1;
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
  uartinit();
  if(device_register(DEV_UART1, "uart1", &uart_ops) < 0)
    panic("uart1 device register");
}
