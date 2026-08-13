// u1test: exercise /dev/uart1 in either direction.
//
//   u1test [read|write|loop] [poll] [baud]
//
//   write (default)  send an incrementing test-pattern burst on UART1 TX.
//   read             dump whatever arrives on UART1 RX as hex.
//   loop             board-side self test: physically short the UART1 TX pin
//                    to the UART1 RX pin, then write a pattern and read it
//                    back.  PASS = the UART1 TX/RX hardware path is good and
//                    any "no data" on the PC side is external wiring; FAIL =
//                    the UART isn't transmitting at all.  Runs in POLL mode.
//   poll             use device mode POLL (direct hardware, interrupts off)
//                    instead of the interrupt-driven RAW stream.  If the ISR
//                    path is suspect this is the decisive experiment: a poll
//                    success isolates the hardware, a poll failure points at
//                    pins / clock / baud.
//   baud             line rate (default 115200).
//
// On the PC side, attach a USB-TTL to the UART1 TX pin and watch at the same
// baud.  If nothing appears, check the pin wiring first: uart.h
// UART_TX_IO/UART_RX_IO must match the physical pin the adapter is on (this
// driver routes UART1 to IO7/IO8 by default; the old note said IO22).  The
// GET_BAUD_INFO line also verifies the divisor math: with the DLAB fix it
// now reads the real latch, so a wrong APB0 clock estimate shows up here.

#include "user.h"
#include "fcntl.h"
#include "console.h"
#include "dev.h"

#define TX_BURST_INTERVAL_TICKS 100   // ~500ms at K210's 5ms tick
#define RX_CHUNK 64

static int
is_arg(const char *s, const char *name)
{
  while (*s && *name) {
    if (*s++ != *name++)
      return 0;
  }
  return *s == *name;
}

int
main(int argc, char *argv[])
{
  int do_read = 0;
  int do_loop = 0;
  int do_poll = 0;
  int baud = 115200;
  int fd;
  int burst = 0;

  // TEMP-DIAG: print the argc/argv this process actually received.  "u1test
  // read poll" kept reporting RAW(irq) though source and binary provably set
  // do_poll for a "poll" arg -- this settles whether argv really contains it.
  printf("u1test: argc=%d", argc);
  for (int i = 0; i < argc; i++)
    printf(" [%s]", argv[i]);
  printf("\n");

  for (int i = 1; i < argc; i++) {
    if (is_arg(argv[i], "read"))
      do_read = 1;
    else if (is_arg(argv[i], "loop"))
      do_loop = 1;
    else if (is_arg(argv[i], "write"))
      do_read = 0;
    else if (is_arg(argv[i], "poll"))
      do_poll = 1;
    else
      baud = atoi(argv[i]);
  }
  if (do_loop)
    do_poll = 1;   // loopback exercises raw hardware, no ISR involved
  if (baud < 9600 || baud > 5000000)
    baud = 115200;

  fd = open("/dev/uart1", O_RDWR);
  if (fd < 0) {
    printf("u1test: open /dev/uart1 failed\n");
    exit(1);
  }
  ioctl(fd, CONSOLE_IOCTL_SET_MODE, do_poll ? CONSOLE_MODE_POLL : CONSOLE_MODE_RAW);
  ioctl(fd, CONSOLE_IOCTL_SET_BAUD, baud);

  printf("u1test: mode=%s /dev/uart1 @ %d baud%s\n",
         do_read ? "read" : "write", baud,
         do_poll ? "  POLL(direct,no-irq)" : "  RAW(irq)");

  struct console_baud_info bi;
  if (ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&bi) == 0)
    printf("u1test: baud requested=%u actual=%u div=%u clock=%u\n",
           bi.requested, bi.actual, bi.div, bi.clock);

  struct console_rx_stats rs;
  if (ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)&rs) == 0)
    printf("u1test: rx dropped=%u buffered=%u cap=%u mode=%u\n",
           rs.dropped, rs.buffered, rs.capacity, rs.mode);

  if (do_loop) {
    // Board-side self test.  Physically short the UART1 TX pin to the RX pin
    // first (IO7 <-> IO8 with the current uart.h routing), then this writes a
    // pattern straight to the THR and polls it back off the RBR -- no ISR, no
    // ring buffers.  PASS proves the UART1 silicon + pin path is alive, so a
    // silent PC means the adapter simply isn't on the pin the driver drives.
    static const char pat[] = "0123456789abcdef";
    char rbuf[sizeof(pat)];
    int n = sizeof(pat) - 1;
    int w, got, i, ok = 1;

    printf("u1test: loopback (short UART1_TX to UART1_RX first)\n");
    w = write(fd, pat, n);
    printf("u1test: wrote %d bytes\n", w);
    got = read(fd, rbuf, n);
    printf("u1test: read back %d bytes:", got);
    for (i = 0; i < got; i++) {
      uint8 b = (uint8)rbuf[i];
      printf(" %x%x", b >> 4, b & 0xf);   // %x has no width/pad support; split the nibbles
    }
    printf("\n");
    if (got != n)
      ok = 0;
    else
      for (i = 0; i < n; i++)
        if (rbuf[i] != pat[i]) { ok = 0; break; }
    printf("u1test: loopback %s\n", ok ? "PASS" : "FAIL");
    close(fd);
    exit(ok ? 0 : 1);
  }

  if (do_read) {
    char rbuf[RX_CHUNK];
    printf("u1test: reading...\n");
    for (;;) {
      int got = read(fd, rbuf, sizeof(rbuf));
      if (got < 0) {
        printf("u1test: read error\n");
        break;
      }
      if (got == 0) {
        if (do_poll)
          break;   // POLL read gives up after its per-byte budget
        continue;
      }
      for (int i = 0; i < got; i++) {
        uint8 b = (uint8)rbuf[i];
        printf("%x%x ", b >> 4, b & 0xf);
      }
      printf("\n");
    }
    printf("u1test: read done\n");
    close(fd);
    exit(0);
  }

  printf("u1test: sending test pattern on /dev/uart1 TX at %d baud\n", baud);
  for (;;) {
    char msg[64];
    int n = snprintf(msg, sizeof(msg),
                     "u1test burst %d: ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\n",
                     burst);
    write(fd, msg, n);
    sleep(TX_BURST_INTERVAL_TICKS);
    burst++;
    if ((burst % 10) == 0)
      printf("u1test: sent %d bursts\n", burst);
  }
  exit(0);
}
