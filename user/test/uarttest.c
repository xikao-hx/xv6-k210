// uarttest: exercise /dev/uart1 in either direction.
//
//   uarttest [read|write|loop|stress|switch] [intrx] [inttx] [baud] ...
//
//   write (default)  send an incrementing test-pattern burst on UART1 TX.
//   read             dump whatever arrives on UART1 RX as hex.
//   loop             board-side self test: short UART1_TX to UART1_RX.
//   stress           loopback high-pressure at [baud], verifying byte-for-byte.
//   switch           loopback across repeated INT<->DMA mode toggles.
//   intrx / inttx    force the RX/TX path to interrupt mode.The driver default is DMA.
//   nofh             switch test: skip the flush after each mode toggle.
//   baud             line rate (default 115200; stress/switch default 1500000).
//

#include "user.h"
#include "fcntl.h"
#include "console.h"
#include "dev.h"
#include "uart.h"

#define TX_BURST_INTERVAL_TICKS 100   // ~500ms at K210's 5ms tick
#define RX_CHUNK 64
#define STRESS_CHUNK 2048             // loopback stress write/read chunk
#define STRESS_DEFAULT_BYTES 131072   // 128 KiB total unless overridden
#define SWITCH_FRAME 16               // frames sent across each mode toggle
#define SWITCH_DEFAULT_ITERS 20

static int
is_arg(const char *s, const char *name)
{
  while (*s && *name) {
    if (*s++ != *name++)
      return 0;
  }
  return *s == *name;
}

// ---- stress: loopback high-pressure at a given baud ----
//   uarttest stress [intrx] [inttx] [baud] [bytes]
static void
stress_run(int fd, int nbytes, int baud)
{
  static char wbuf[STRESS_CHUNK];
  static char rbuf[STRESS_CHUNK];
  int offset = 0;
  int received = 0;
  int mismatches = 0;
  int timeouts = 0;
  struct console_rx_stats rs;

  printf("uarttest: stress nbytes=%d baud=%d chunk=%d\n", nbytes, baud, STRESS_CHUNK);
  while (offset < nbytes) {
    int chunk = nbytes - offset;
    int w, got = 0, i, t0;

    if (chunk > STRESS_CHUNK)
      chunk = STRESS_CHUNK;
    for (i = 0; i < chunk; i++)
      wbuf[i] = (char)((offset + i) & 0xff);

    w = write(fd, wbuf, chunk);
    if (w != chunk) {
      printf("uarttest: stress write short w=%d want=%d off=%d\n", w, chunk, offset);
      break;
    }

    t0 = uptime();
    while (got < chunk) {
      int r = read(fd, rbuf + got, chunk - got);
      if (r <= 0)
        break;
      got += r;
      if (uptime() - t0 > 1000)   // ~5s: a 2KiB chunk at 9600 baud is ~1.7s
        break;
    }
    if (got != chunk) {
      printf("uarttest: stress read short got=%d want=%d off=%d\n", got, chunk, offset);
      timeouts++;
    }

    for (i = 0; i < got; i++) {
      if ((uint8)rbuf[i] != ((offset + i) & 0xff)) {
        if (mismatches < 5)
          printf("uarttest: stress MISMATCH off=%d got=%x expect=%x\n",
                 offset + i, (uint8)rbuf[i], (offset + i) & 0xff);
        mismatches++;
      }
    }
    offset += got;
    received += got;
    if ((offset / STRESS_CHUNK) % 16 == 0)
      printf("uarttest: stress %d/%d bytes\n", offset, nbytes);
  }

  ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)&rs);
  printf("uarttest: stress done w=%d r=%d mism=%d tmo=%d dropped=%u buffered=%u\n",
         offset, received, mismatches, timeouts, rs.dropped, rs.buffered);
  printf("uarttest: stress %s\n",
         (offset == nbytes && mismatches == 0 && rs.dropped == 0)
           ? "PASS" : "FAIL");
}

// ---- switch: INT<->DMA round-trip ----
//   uarttest switch [baud] [count] [nofh]
static void
switch_run(int fd, int count, int baud, int noflush)
{
  static char frame[SWITCH_FRAME];
  static char rbuf[SWITCH_FRAME + 4];
  int i;
  struct console_rx_stats rs;

  printf("uarttest: switch count=%d baud=%d %s\n", count, baud,
         noflush ? "nofh" : "flush");

  ioctl(fd, UART_IOCTL_SET_RX_MODE, UART_MODE_INT);
  ioctl(fd, UART_IOCTL_SET_TX_MODE, UART_MODE_INT);
  ioctl(fd, CONSOLE_IOCTL_FLUSH_INPUT, 0);

  for (i = 0; i < count; i++) {
    int rx = (i & 1) ? UART_MODE_DMA : UART_MODE_INT;
    int tx = (i % 3 == 2) ? UART_MODE_DMA : UART_MODE_INT;
    int w, got = 0, j, ok = 1;

    ioctl(fd, UART_IOCTL_SET_RX_MODE, rx);
    ioctl(fd, UART_IOCTL_SET_TX_MODE, tx);
    if (!noflush)
      ioctl(fd, CONSOLE_IOCTL_FLUSH_INPUT, 0);

    for (j = 0; j < SWITCH_FRAME; j++)
      frame[j] = (char)('a' + ((i + j) & 0xf));

    w = write(fd, frame, SWITCH_FRAME);
    if (w != SWITCH_FRAME) {
      printf("uarttest: switch it=%d write short w=%d\n", i, w);
      ok = 0;
    } else {
      while (got < SWITCH_FRAME) {
        int r = read(fd, rbuf + got, SWITCH_FRAME + 4 - got);
        if (r <= 0) {
          ok = 0;
          break;
        }
        got += r;
      }
      if (got != SWITCH_FRAME)
        ok = 0;   // too many bytes = a switch-induced spurious frame
      for (j = 0; ok && j < SWITCH_FRAME; j++)
        if (rbuf[j] != frame[j])
          ok = 0;
    }

    ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)&rs);
    if (rs.dropped != 0)
      ok = 0;
    if (!noflush && rs.buffered != 0)
      ok = 0;

    printf("uarttest: switch it=%d rx=%s tx=%s got=%d buffered=%u dropped=%u %s\n",
           i, rx ? "DMA" : "INT", tx ? "DMA" : "INT", got,
           rs.buffered, rs.dropped, ok ? "OK" : "FAIL");
    if (!ok)
      break;
  }

  ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)&rs);
  printf("uarttest: switch done iters=%d buffered=%u dropped=%u\n",
         i, rs.buffered, rs.dropped);
  printf("uarttest: switch %s\n", i == count ? "PASS" : "FAIL");
}

// ---- loop: board-side self test (short UART1_TX to UART1_RX) ----
//   uarttest loop [intrx] [inttx]  -- returns 0 on PASS, 1 on FAIL
static int
loop_run(int fd)
{
  static const char pat[] = "0123456789abcdef";
  char rbuf[sizeof(pat)];
  int n = sizeof(pat) - 1;
  int w, got, i, ok = 1;

  printf("uarttest: loopback (short UART1_TX to UART1_RX first)\n");
  // A mode/baud reconfiguration can leave a single spurious frame in the RX
  // (DMA arm in particular: real-hardware loopback showed buffered=1 before
  // the write).  Clear it so the read below starts from a clean ring.
  ioctl(fd, CONSOLE_IOCTL_FLUSH_INPUT, 0);
  w = write(fd, pat, n);
  printf("uarttest: wrote %d bytes\n", w);
  got = read(fd, rbuf, n);
  printf("uarttest: read back %d bytes:", got);
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
  printf("uarttest: loopback %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

// ---- read: dump incoming bytes as hex ----
//   uarttest read [intrx]
static void
read_run(int fd)
{
  char rbuf[RX_CHUNK];
  printf("uarttest: reading...\n");
  for (;;) {
    int got = read(fd, rbuf, sizeof(rbuf));
    if (got < 0) {
      printf("uarttest: read error\n");
      break;
    }
    if (got == 0)
      continue;   // only a flush bumps the epoch; otherwise block
    for (int i = 0; i < got; i++) {
      uint8 b = (uint8)rbuf[i];
      printf("%x%x ", b >> 4, b & 0xf);
    }
    printf("\n");
  }
  printf("uarttest: read done\n");
}

// ---- write: burst TX test pattern forever ----
//   uarttest write [inttx]
static void
write_run(int fd, int baud)
{
  int burst = 0;
  printf("uarttest: sending test pattern on /dev/uart1 TX at %d baud\n", baud);
  for (;;) {
    char msg[64];
    int n = snprintf(msg, sizeof(msg),
                     "uarttest burst %d: ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\r\n",
                     burst);
    write(fd, msg, n);
    sleep(TX_BURST_INTERVAL_TICKS);
    burst++;
    if ((burst % 10) == 0)
      printf("uarttest: sent %d bursts\n", burst);
  }
}

int
main(int argc, char *argv[])
{
  int do_read = 0;
  int do_loop = 0;
  int do_stress = 0;
  int do_switch = 0;
  int rx_mode = UART_MODE_DMA;  // driver default; intrx/inttx/dmarx/dmatx override
  int tx_mode = UART_MODE_DMA;
  int noflush = 0;
  int baud = 115200;
  int nbytes = STRESS_DEFAULT_BYTES;
  int ncount = SWITCH_DEFAULT_ITERS;
  int nums[4], nnum = 0;
  int fd;

  for (int i = 1; i < argc; i++) {
    if (is_arg(argv[i], "read"))
      do_read = 1;
    else if (is_arg(argv[i], "loop"))
      do_loop = 1;
    else if (is_arg(argv[i], "stress"))
      do_stress = 1;
    else if (is_arg(argv[i], "switch"))
      do_switch = 1;
    else if (is_arg(argv[i], "write"))
      do_read = 0;
    else if (is_arg(argv[i], "intrx"))
      rx_mode = UART_MODE_INT;
    else if (is_arg(argv[i], "inttx"))
      tx_mode = UART_MODE_INT;
    else if (is_arg(argv[i], "dmarx"))
      rx_mode = UART_MODE_DMA;   // no-op: DMA is already the default
    else if (is_arg(argv[i], "dmatx"))
      tx_mode = UART_MODE_DMA;
    else if (is_arg(argv[i], "nofh"))
      noflush = 1;
    else if (nnum < 4)
      nums[nnum++] = atoi(argv[i]);
  }

  // Per-mode numeric interpretation: [baud]; stress takes [baud] [bytes];
  // switch takes [count] [baud].
  if (do_switch) {
    ncount = nnum > 0 ? nums[0] : SWITCH_DEFAULT_ITERS;
    baud = nnum > 1 ? nums[1] : 1500000;
  } else if (do_stress) {
    baud = nnum > 0 ? nums[0] : 1500000;
    nbytes = nnum > 1 ? nums[1] : STRESS_DEFAULT_BYTES;
  } else {
    baud = nnum > 0 ? nums[0] : 115200;
  }
  if (baud < 9600 || baud > 20000000)
    baud = 115200;

  fd = open("/dev/uart1", O_RDWR);
  if (fd < 0) {
    printf("uarttest: open /dev/uart1 failed\n");
    exit(1);
  }
  ioctl(fd, UART_IOCTL_SET_RX_MODE, rx_mode);
  ioctl(fd, UART_IOCTL_SET_TX_MODE, tx_mode);
  ioctl(fd, CONSOLE_IOCTL_SET_BAUD, baud);

  printf("uarttest: rx=%s tx=%s /dev/uart1 @ %d baud\n",
         rx_mode ? "DMA" : "INT", tx_mode ? "DMA" : "INT", baud);

  struct console_baud_info bi;
  if (ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&bi) == 0)
    printf("uarttest: baud requested=%u actual=%u div=%u clock=%u\n",
           bi.requested, bi.actual, bi.div, bi.clock);

  struct console_rx_stats rs;
  if (ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)&rs) == 0)
    printf("uarttest: rx dropped=%u buffered=%u cap=%u mode=%u\n",
           rs.dropped, rs.buffered, rs.capacity, rs.mode);

  if (do_switch) {
    switch_run(fd, ncount, baud, noflush);
    close(fd);
    exit(0);
  }
  if (do_stress) {
    stress_run(fd, nbytes, baud);
    close(fd);
    exit(0);
  }
  if (do_loop) {
    int r = loop_run(fd);
    close(fd);
    exit(r);
  }
  if (do_read) {
    read_run(fd);
    close(fd);
    exit(0);
  }
  write_run(fd, baud);
  return 0;
}