//
// Raw UART device — bypasses the console for binary I/O.
//
// read:  sleep until bytes are available in the interrupt-fed raw RX ring
// write: synchronous putc (uartputc_sync) for each byte
// ioctl: RAW_START / RAW_END / SET_BAUD / GET_BAUD_INFO / GET_RAW_STATS
//
// The caller must bracket a raw-transfer sequence with:
//   ioctl(fd, UART_IOCTL_RAW_START, 0);
//   ... read() / write() ...
//   ioctl(fd, UART_IOCTL_RAW_END, 0);
// RAW_START redirects received bytes from the console to the raw RX ring.
// RAW_END restores normal console input handling.
//

#include "file.h"
#include "kalloc.h"
#include "proc.h"
#include "uarths.h"
#include "uartdev.h"
#include "vm.h"

static int
uartdev_read(int user_dst, uint64 dst, int n)
{
  // Cap at one page.
  if (n > 4096)
    n = 4096;

  char *buf = kalloc();
  if (buf == 0)
    return -1;

  int ret = uart_raw_read(buf, n);

  if (ret > 0 && either_copyout(user_dst, dst, buf, ret) < 0)
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

  if (cmd == UART_IOCTL_RAW_START) {
    uartrx_disable();
    while (uartgetc() != -1)
      ;
    uart_raw_start();
    uartrx_enable();
    return 0;
  }

  if (cmd == UART_IOCTL_RAW_END) {
    uart_raw_end();
    uart_raw_flush();
    uartrx_enable();
    return 0;
  }

  if (cmd == UART_IOCTL_SET_BAUD) {
    uart_set_baud((int)arg);
    return 0;
  }

  if (cmd == UART_IOCTL_GET_BAUD_INFO) {
    uint32 info[4];
    uart_get_baud_info(info);
    if (copyout(myproc()->pagetable, arg, (char *)info, sizeof(info)) < 0)
      return -1;
    return 0;
  }

  if (cmd == UART_IOCTL_GET_RAW_STATS) {
    uint32 info[4];
    uart_raw_get_stats(info);
    if (copyout(myproc()->pagetable, arg, (char *)info, sizeof(info)) < 0)
      return -1;
    return 0;
  }

  return -1;
}

void
uartdev_init(void)
{
  devsw[DEV_UART].read   = uartdev_read;
  devsw[DEV_UART].write  = uartdev_write;
  devsw[DEV_UART].ioctl  = uartdev_ioctl;
}
