// Console TTY/RAW device layered on the UART byte-stream driver.

#include "proc.h"
#include "console.h"
#include "file.h"
#include "uarths.h"

#define BACKSPACE 0x100
#define C(x)  ((x) - '@')
#define CONSOLE_IO_CHUNK 128

struct {
  struct spinlock lock;
  int mode;
  int esc;
  int drop_lf_after_cr;
  int eof_pending;
} cons;

void
consputc(int c)
{
  if (c == BACKSPACE) {
    uartputc_sync('\b');
    uartputc_sync(' ');
    uartputc_sync('\b');
  } else {
    uartputc_sync(c);
  }
}

int
consolewrite(int user_src, uint64 src, int n)
{
  char buf[CONSOLE_IO_CHUNK];
  int done = 0;

  while (done < n) {
    int count = n - done;
    int written;

    if (count > sizeof(buf))
      count = sizeof(buf);
    if (either_copyin(buf, user_src, src + done, count) < 0)
      break;
    written = uart_write(buf, count);
    if (written <= 0)
      break;
    done += written;
    if (written != count)
      break;
  }
  return done;
}

static int
console_mode_get(void)
{
  int mode;

  acquire(&cons.lock);
  mode = cons.mode;
  release(&cons.lock);
  return mode;
}

static void
console_set_mode(int mode)
{
  // IRQ is paused before the ring is cleared, so no boundary byte can be
  // classified using the old mode after the transition completes.
  uartrx_disable();
  uart_flush_rx();
  acquire(&cons.lock);
  cons.mode = mode;
  cons.esc = 0;
  cons.drop_lf_after_cr = 0;
  cons.eof_pending = 0;
  release(&cons.lock);
  uartrx_enable();
}

static int
consoleioctl(int minor, uint64 cmd, uint64 arg)
{
  uint32 info[4];

  (void)minor;
  switch (cmd) {
  case CONSOLE_IOCTL_FLUSH_INPUT:
    uartrx_disable();
    uart_flush_rx();
    acquire(&cons.lock);
    cons.esc = 0;
    cons.drop_lf_after_cr = 0;
    cons.eof_pending = 0;
    release(&cons.lock);
    uartrx_enable();
    return 0;
  case CONSOLE_IOCTL_SET_MODE:
    if (arg != CONSOLE_MODE_TTY && arg != CONSOLE_MODE_RAW)
      return -1;
    console_set_mode((int)arg);
    return 0;
  case CONSOLE_IOCTL_GET_MODE:
    return console_mode_get();
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
    info[3] = console_mode_get();
    return either_copyout(1, arg, info, sizeof(info));
  default:
    return -1;
  }
}

static int
console_raw_read(int user_dst, uint64 dst, int n)
{
  char buf[CONSOLE_IO_CHUNK];
  int count = n;
  int got;

  if (count > sizeof(buf))
    count = sizeof(buf);
  got = uart_read(buf, count);
  if (got <= 0)
    return got;
  if (either_copyout(user_dst, dst, buf, got) < 0)
    return -1;
  return got;
}

static int
console_tty_char(int c, char *out)
{
  int echo;

  acquire(&cons.lock);
  if (cons.mode != CONSOLE_MODE_TTY) {
    release(&cons.lock);
    return 0;
  }

  if (c == C('P')) {
    release(&cons.lock);
    procdump();
    return 0;
  }

  if (c == 0) {
    release(&cons.lock);
    return 0;
  }

#ifndef QEMU
  if (c == '\r') {
    c = '\n';
    cons.drop_lf_after_cr = 1;
  } else if (cons.drop_lf_after_cr && c == '\n') {
    cons.drop_lf_after_cr = 0;
    release(&cons.lock);
    return 0;
  } else {
    cons.drop_lf_after_cr = 0;
  }
#else
  if (c == '\r')
    c = '\n';
#endif

  echo = (c >= ' ' && c <= '~') || c == '\n';
  if (c == 0x1b) {
    cons.esc = 1;
    echo = 0;
  } else if (cons.esc) {
    echo = 0;
    if (cons.esc == 1 && (c == '[' || c == 'O'))
      cons.esc = 2;
    else
      cons.esc = 0;
  }
  release(&cons.lock);

  if (echo)
    consputc(c);
  *out = c;
  return 1;
}

static int
console_tty_read(int user_dst, uint64 dst, int n)
{
  int done = 0;

  acquire(&cons.lock);
  if (cons.eof_pending) {
    cons.eof_pending = 0;
    release(&cons.lock);
    return 0;
  }
  release(&cons.lock);

  while (done < n) {
    char input;
    char output;
    int got = uart_read(&input, 1);

    if (got <= 0)
      return done > 0 ? done : got;
    if (!console_tty_char((unsigned char)input, &output))
      continue;
    if (output == C('D')) {
      if (done > 0) {
        acquire(&cons.lock);
        cons.eof_pending = 1;
        release(&cons.lock);
      }
      break;
    }
    if (either_copyout(user_dst, dst + done, &output, 1) < 0)
      return done > 0 ? done : -1;
    done++;
    if (output == '\n')
      break;
  }
  return done;
}

int
consoleread(int user_dst, uint64 dst, int n)
{
  if (n <= 0)
    return 0;
  if (console_mode_get() == CONSOLE_MODE_RAW)
    return console_raw_read(user_dst, dst, n);
  return console_tty_read(user_dst, dst, n);
}

void
consoleinit(void)
{
  initlock(&cons.lock, "cons");
  cons.mode = CONSOLE_MODE_TTY;
  uartinit();
  devsw[DEV_CONSOLE].read = consoleread;
  devsw[DEV_CONSOLE].write = consolewrite;
  devsw[DEV_CONSOLE].ioctl = consoleioctl;
}
