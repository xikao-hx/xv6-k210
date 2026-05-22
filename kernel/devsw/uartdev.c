//
// Raw UART device — bypasses the console for binary I/O.
//
// read:  disable RX interrupt, poll uartgetc(), return raw bytes
// write: synchronous putc (uartputc_sync) for each byte
// ioctl: UART_IOCTL_RAW_START / UART_IOCTL_RAW_END
//
// The caller must bracket a raw-transfer sequence with:
//   ioctl(fd, UART_IOCTL_RAW_START, 0);
//   ... read() / write() ...
//   ioctl(fd, UIOCTL_RAW_END, 0);
// RAW_START disables the UART RX interrupt so that the interrupt
// handler does not steal bytes while we poll.  RAW_END re-enables it.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "file.h"
#include "defs.h"
#include "uartdev.h"

static int
uartdev_read(int user_dst, uint64 dst, int n)
{
  // Cap at one page.
  if (n > 4096)
    n = 4096;

  char *buf = kalloc();
  if (buf == 0)
    return -1;

  // Disable preemption during the polling loop to prevent RX FIFO
  // overflow (K210 UART HS has only 8-entry FIFO).
  push_off();
  for (int i = 0; i < n; i++) {
    int c;
    while ((c = uartgetc()) == -1)
      ;
    buf[i] = (char)c;
  }
  pop_off();

  int ret = n;
  if (either_copyout(user_dst, dst, buf, n) < 0)
    ret = -1;

  kfree(buf);
  return ret;
}

static int
uartdev_write(int user_src, uint64 src, int n)
{
  if (n > 4096)
    n = 4096;

  char *buf = kalloc();
  if (buf == 0)
    return -1;

  if (either_copyin(buf, user_src, src, n) < 0) {
    kfree(buf);
    return -1;
  }

  for (int i = 0; i < n; i++)
    uartputc_sync(buf[i]);

  kfree(buf);
  return n;
}

static int
uartdev_ioctl(int minor, uint64 cmd, uint64 arg)
{
  (void)minor;
  (void)arg;

  switch (cmd) {
  case UART_IOCTL_RAW_START:
    uartrx_disable();
    // Drain any stale byte that might have arrived before we disabled.
    while (uartgetc() != -1)
      ;
    // Prevent scheduler from preempting us during raw-mode polling,
    // so the 8-entry RX FIFO doesn't overflow while we're switched out.
    myproc()->no_preempt = 1;
    return 0;
  case UART_IOCTL_RAW_END:
    myproc()->no_preempt = 0;
    uartrx_enable();
    return 0;
  case UART_IOCTL_SET_BAUD:
    uart_set_baud((int)arg);
    return 0;
  case UART_IOCTL_FLUSH_RX:
    while (uartgetc() != -1)
      ;
    return 0;
  case UART_IOCTL_GETC: {
    // Efficient single-byte read — bypasses kalloc/copyout/kfree.
    // Returns the byte value (0-255).  Spins until a byte arrives;
    // the caller must ensure the UART has data (or will have data)
    // before calling, otherwise this hangs forever.
    push_off();
    int v;
    while ((v = uartgetc()) == -1)
      ;
    pop_off();
    return v;
  }
  default:
    return -1;
  }
}

void
uartdev_init(void)
{
  devsw[UART_DEV].read   = uartdev_read;
  devsw[UART_DEV].write  = uartdev_write;
  devsw[UART_DEV].ioctl  = uartdev_ioctl;
}
