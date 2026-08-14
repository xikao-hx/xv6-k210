// Burn a FAT filesystem image over UART in framed RAW-mode messages.
//
// The burn data UART is chosen at runtime by the host: the INFO packet's
// optional 9th payload byte selects the data port.
//   data_port == 0 (default) -> /dev/console (UARTHS, interrupt-fed RX ring)
//   data_port == 1           -> /dev/uart1 (DW UART1, DMA RX CH5 / TX CH4)
// The shell handshake (BURN announcement + INFO) always runs on the console;
// only the DATA phase moves to uart1 when data_port==1.  Protocol logic is
// identical either way; only the device entry branches.
//
// Host flow:
//   1. Send "/burn\n" to the shell, which starts this program.
//   2. Wait for "BURN\n".
//   3. Send INFO with image size, optional transfer baud, and data_port.
//   4. Optionally switch UART baud with a BAUD/ACK sync.
//   5. Send one 512-byte DATA packet per sector.
//   6. Send DONE after all sectors are acknowledged.
//
// Packet format:
//   magic(4: 55 AA 55 AA), seq(4 LE), type(1), plen(2 LE),
//   payload(plen), crc32(4 LE over magic through payload).

#include "types.h"
#include "file.h"
#include "console.h"
#include "sdcarddev.h"
#include "oledfb.h"
#include "oled.h"
#include "user.h"
#include "fcntl.h"
#include <stdarg.h>
#include "uart.h"

#define MAX_RETRY    5
#define CONSOLE_BAUD 115200
// Mid-frame wire silence (ticks) before the board NAKs early instead of waiting
// out the host's 3s recv timeout.  0.5s at 5ms/tick = 100.  Only armed in the
// DATA phase; handshake messages keep the blocking read.
#define RX_STALL_NAK_TICKS 100

#define PKT_INFO   0x01  // Host to board: payload is total image size.
#define PKT_DATA   0x02  // Host to board: payload is one 512-byte sector.
#define PKT_DONE   0x03  // Host to board: transfer complete.
#define PKT_BAUD   0x04  // Host to board: request/confirm baud switch.
#define PKT_ACK    0x81  // Board to host: packet accepted.
#define PKT_NAK    0x82  // Board to host: packet rejected; payload is error.

#define ERR_CRC    0x01
#define ERR_WRITE  0x02

#define ACK_OK       0x00
#define ACK_DUP      0x01
#define ACK_INFO     0x02
#define ACK_DONE     0x03
#define ACK_BAUD     0x04

#define BURN_PROGRESS_TITLE "WRITING FS V2"

// ---- RX stall-timing diagnostics (frame-truncation signature) ----
// If the host drops the tail of a 1.5M frame, the board blocks in read_bytes
// waiting for bytes that never arrive; after the host's retry timeout the retry
// bytes complete a garbage frame and the CRC fails.  The signature is a long
// inter-read gap INSIDE the failing frame (a normal frame at 1.5M has gaps of a
// few ticks at most).  These counters are passive -- no sleep, no abort -- so
// the protocol timing is untouched.  Per-frame prints are gated on the DATA
// port being uart1 (console then free); in console mode the stream must stay
// printf-free, so the information lands only in the final stats line.
static uint diag_stall_ticks;     // worst inter-read gap (ticks) in the current recv_msg
static uint diag_stall_at;        // byte offset within that recv_msg where the gap occurred
static uint diag_crc_stall_max;   // worst such gap among all CRC-failing frames
static uint diag_crc_stall_max_sec;
static uint diag_long_stalls;     // CRC failures whose frame showed a nonzero stall

static void get_uart_rx_stats(int fd, struct console_rx_stats *stats);

static int
oled_init(void)
{
  OLED_init();   /* opens /dev/oledfb + mmap; exits on failure */
  OLED_Clear();
  return 0;
}

static void
oled_write_row(uint8 row, const char *str)
{
  if (row > 3)
    return;
  OLED_ClearArea(0, row * 16, OLEDFB_W, 16);  /* clear row */
  OLED_ShowString(0, row * 16, str, OLED_8X16);
  OLED_Flush();
}

int vsnprintf(char *buf, int n, const char *fmt, va_list ap);

static void
oled_printf(uint8 row, uint8 col, const char *fmt, ...)
{
  char buf[17];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  OLED_ClearArea(0, row * 16, OLEDFB_W, 16);  /* clear row */
  OLED_ShowString(col * 8, row * 16, buf, OLED_8X16);
  OLED_Flush();
}

static void
oled_show_hex_num(uint8 row, uint8 col, uint32 num, uint8 len)
{
  OLED_ShowHexNum(col * 8, row * 16, num, len, OLED_8X16);
  OLED_Flush();
}

static void
oled_show_hex32(uint8 row, uint8 col, uint32 val)
{
  OLED_ShowHex32(col * 8, row * 16, val, OLED_8X16);
  OLED_Flush();
}

static void
oled_write_hexrow(uint8 row, const char *label, const uint8 *data, int n)
{
  OLED_ClearArea(0, row * 16, OLEDFB_W, 16);  /* clear row */
  OLED_ShowHexRow(0, row * 16, label, data, n, OLED_8X16);
  OLED_Flush();
}

static void
put32le(uint8 *p, uint32 v)
{
  p[0] = v & 0xFF;
  p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF;
  p[3] = (v >> 24) & 0xFF;
}

static void
show_hex_value(uint8 row, const char *label, uint32 value)
{
  uint col = strlen(label);

  oled_write_row(row, label);
  if (col < 16)
    oled_show_hex32(row, col, value);
}

static void
show_dec_value(uint8 row, const char *label, uint32 value)
{
  char buf[17];

  snprintf(buf, sizeof(buf), "%s%u", label, value);
  oled_write_row(row, buf);
}

static void
show_phase(const char *phase)
{
  oled_write_row(0, phase);
}

static void
show_image_info(uint32 nsectors)
{
  show_dec_value(1, "TOT ", nsectors);
  oled_write_row(2, "");
  oled_write_row(3, "");
}

static void
show_error(const char *msg, uint32 sec)
{
  show_phase("BURN ERROR");
  oled_write_row(2, msg);
  show_dec_value(3, "SEC ", sec);
}

static void
show_retry(const char *why, uint32 sec, int retry)
{
  show_phase("RETRY");
  oled_printf(2, 0, "%s %d/%d", why, retry + 1, MAX_RETRY);
  show_dec_value(3, "SEC ", sec);
}

static void
show_progress(uint32 sec, uint32 nsectors)
{
  uint32 pct = nsectors ? (sec * 100) / nsectors : 0;
  char buf[17];

  show_phase(BURN_PROGRESS_TITLE);
  snprintf(buf, sizeof(buf), "SEC %u", sec);
  oled_write_row(1, buf);
  snprintf(buf, sizeof(buf), "TOT %u", nsectors);
  oled_write_row(2, buf);
  snprintf(buf, sizeof(buf), "PROGRESS %u%%", pct);
  oled_write_row(3, buf);
}

// CRC32, reflected polynomial 0xEDB88320, compatible with zlib.crc32().

static uint32 crc32_tab[256];

static void
crc32_init(void)
{
  for (uint32 i = 0; i < 256; i++) {
    uint32 crc = i;
    for (int j = 0; j < 8; j++) {
      if (crc & 1)
        crc = (crc >> 1) ^ 0xEDB88320;
      else
        crc >>= 1;
    }
    crc32_tab[i] = crc;
  }
}

static uint32
crc32(const uint8 *data, int len, uint32 crc)
{
  crc = ~crc;
  for (int i = 0; i < len; i++)
    crc = crc32_tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// Read exactly n bytes from the interrupt-fed raw UART ring.  The gap between
// consecutive read() returns is tracked: a long gap means the wire went silent
// mid-frame (host TX tail-drop), which is exactly the truncation signature.
// read() blocks until data arrives, so the gap includes the blocking wait.
static int
read_bytes(int fd, uint8 *buf, int n)
{
  int off = 0;
  uint t_prev = uptime();

  while (off < n) {
    int r = read(fd, buf + off, n - off);
    if (r <= 0)
      return -1;
    uint t_now = uptime();
    uint gap = t_now - t_prev;
    if (gap > diag_stall_ticks) {
      diag_stall_ticks = gap;
      diag_stall_at = off;
    }
    t_prev = t_now;
    off += r;
  }

  return 0;
}

// Read exactly n bytes, giving up if the wire goes silent mid-read for more
// than max_gap_ticks (a truncated frame: the host's TX dropped the frame, and
// without this the board would block for the full host retry timeout).  The
// ring depth is polled via ioctl; read() is only called when bytes are known
// buffered, so a mid-frame stall is observable instead of blocking forever.
// max_gap_ticks == 0 disables the timeout and falls back to a blocking read
// (handshake messages).  Returns 0 on success, -1 on I/O error, -2 on stall.
static int
read_bytes_timeout(int fd, uint8 *buf, int n, uint max_gap_ticks)
{
  int off = 0;
  uint t_last = uptime();

  while (off < n) {
    struct console_rx_stats st;
    get_uart_rx_stats(fd, &st);

    if (st.buffered > 0) {
      int want = n - off;
      if (want > (int)st.buffered)
        want = st.buffered;
      int r = read(fd, buf + off, want);
      if (r <= 0)
        return -1;
      uint t_now = uptime();
      uint gap = t_now - t_last;
      if (gap > diag_stall_ticks) {
        diag_stall_ticks = gap;
        diag_stall_at = off;
      }
      t_last = t_now;
      off += r;
      continue;
    }

    if (max_gap_ticks == 0) {
      // No timeout: block for data (original behavior).
      int r = read(fd, buf + off, n - off);
      if (r <= 0)
        return -1;
      uint t_now = uptime();
      uint gap = t_now - t_last;
      if (gap > diag_stall_ticks) {
        diag_stall_ticks = gap;
        diag_stall_at = off;
      }
      t_last = t_now;
      off += r;
      continue;
    }

    // Ring empty and a timeout is armed: the frame was truncated on the wire.
    if (uptime() - t_last > max_gap_ticks) {
      diag_stall_ticks = uptime() - t_last;
      diag_stall_at = off;
      return -2;
    }
  }

  return 0;
}

static void
send_msg(int fd, uint32 seq, uint8 type, const uint8 *payload, uint16 plen)
{
  uint8 magic[4] = {0x55, 0xAA, 0x55, 0xAA};
  uint8 hdr[7];
  hdr[0] = seq & 0xFF; hdr[1] = (seq >> 8) & 0xFF;
  hdr[2] = (seq >> 16) & 0xFF; hdr[3] = (seq >> 24) & 0xFF;
  hdr[4] = type;
  hdr[5] = plen & 0xFF; hdr[6] = (plen >> 8) & 0xFF;

  uint32 csum = crc32(magic, sizeof(magic), 0);
  csum = crc32(hdr, sizeof(hdr), csum);
  if (payload && plen)
    csum = crc32(payload, plen, csum);

  uint8 crc_buf[4];
  crc_buf[0] = csum & 0xFF; crc_buf[1] = (csum >> 8) & 0xFF;
  crc_buf[2] = (csum >> 16) & 0xFF; crc_buf[3] = (csum >> 24) & 0xFF;

  write(fd, magic, sizeof(magic));
  write(fd, hdr, sizeof(hdr));
  if (payload && plen)
    write(fd, payload, plen);
  write(fd, crc_buf, 4);
}

// Return the packet type, -1 on I/O/protocol error, -2 on CRC mismatch, or
// -3 if the wire went silent mid-frame for more than stall_nak_ticks (a
// truncated frame; the host's TX dropped it).  The stream is
// self-synchronizing: stray bytes are skipped until magic.  stall_nak_ticks
// == 0 keeps the old blocking reads (handshake messages, where the host
// paces the stream and silence is not an error).
static int
recv_msg(int fd, uint32 *seq_out, uint8 *payload_buf, uint16 *plen_out,
         uint stall_nak_ticks)
{
  uint8 sync[4];
  int   si = 0;

  diag_stall_ticks = 0;   // capture the worst inter-read gap of THIS message
  diag_stall_at = 0;

resync:
  while (1) {
    uint8 ch;
    if (read_bytes(fd, &ch, 1) < 0)
      return -1;
    sync[si] = ch;
    si = (si + 1) & 3;

    uint8 w[4];
    for (int i = 0; i < 4; i++)
      w[i] = sync[(si + i) & 3];
    if (w[0] == 0x55 && w[1] == 0xAA && w[2] == 0x55 && w[3] == 0xAA)
      break;
  }

  uint8 hdr[7];
  int rr = read_bytes_timeout(fd, hdr, 7, stall_nak_ticks);
  if (rr < 0) {
    if (rr == -2)
      return -3;  // mid-frame silence: truncated frame, NAK early
    show_phase("RECV FAIL");
    oled_write_row(2, "HDR");
    return -1;
  }

  uint32 seq = (uint32)hdr[0] | ((uint32)hdr[1] << 8)
             | ((uint32)hdr[2] << 16) | ((uint32)hdr[3] << 24);
  uint8  type  = hdr[4];
  uint16 plen  = (uint16)hdr[5] | ((uint16)hdr[6] << 8);

  // Payload bytes can contain the magic pattern.  If the following header
  // is impossible, treat it as false magic and keep scanning.
  if (type != PKT_INFO && type != PKT_DATA && type != PKT_DONE &&
      type != PKT_BAUD &&
      type != PKT_ACK && type != PKT_NAK) {
    // The seven bytes after a false magic may overlap the next real magic.
    // Keep the trailing bytes in the same ring-window order as the scanner
    // expects, then continue sliding instead of discarding the whole header.
    sync[0] = hdr[5];
    sync[1] = hdr[4];
    sync[2] = hdr[3];
    sync[3] = hdr[2];
    si = 2;
    goto resync;
  }

  if (plen > 512) {
    show_phase("BAD PACKET");
    oled_write_row(2, "PLEN ");
    oled_show_hex_num(2, 5, plen, 4);
    oled_write_hexrow(3, "H:", hdr, 7);
    return -1;
  }

  if (plen > 0) {
    rr = read_bytes_timeout(fd, payload_buf, plen, stall_nak_ticks);
    if (rr < 0) {
      if (rr == -2)
        return -3;
      show_phase("RECV FAIL");
      oled_write_row(2, "PAYLOAD");
      return -1;
    }
  }

  uint8 crc_raw[4];
  rr = read_bytes_timeout(fd, crc_raw, 4, stall_nak_ticks);
  if (rr < 0) {
    if (rr == -2)
      return -3;
    show_phase("RECV FAIL");
    oled_write_row(2, "CRC BYTES");
    return -1;
  }
  uint32 msg_crc = (uint32)crc_raw[0] | ((uint32)crc_raw[1] << 8)
                 | ((uint32)crc_raw[2] << 16) | ((uint32)crc_raw[3] << 24);

  uint8 magic[4] = {0x55, 0xAA, 0x55, 0xAA};
  uint32 csum = crc32(magic, 4, 0);
  csum = crc32(hdr, 7, csum);
  if (plen > 0)
    csum = crc32(payload_buf, plen, csum);

  if (csum != msg_crc)
    return -2;

  *seq_out   = seq;
  *plen_out  = plen;
  return (int)type;
}

static void
send_ack_payload(int fd, uint32 seq, uint8 reason, uint32 ticks)
{
  uint8 payload[5];
  payload[0] = reason;
  put32le(payload + 1, ticks);
  send_msg(fd, seq, PKT_ACK, payload, sizeof(payload));
}

static uint32
get_uart_actual_baud(int fd, uint32 *div)
{
  struct console_baud_info info;

  if (ioctl(fd, CONSOLE_IOCTL_GET_BAUD_INFO, (uint64)&info) < 0) {
    if (div)
      *div = 0;
    return 0;
  }

  if (div)
    *div = info.div;
  return info.actual;
}

static void
get_uart_rx_stats(int fd, struct console_rx_stats *stats)
{
  if (ioctl(fd, CONSOLE_IOCTL_GET_RX_STATS, (uint64)stats) < 0) {
    stats->dropped = 0;
    stats->buffered = 0;
    stats->capacity = 0;
    stats->mode = 0;
  }
}

static void
send_nak(int fd, uint32 seq, uint8 err)
{
  send_msg(fd, seq, PKT_NAK, &err, 1);
}

static void
log_open_failed(const char *name)
{
  printf("burn: open %s failed\n", name);
}

int
main(void)
{
  int console_fd, uart_fd, sdcard_fd;
  int type;
  int success = 0;
  int cache_rc = -1;
  uint32 transfer_baud = CONSOLE_BAUD;
  uint32 crc_errors = 0;
  uint32 io_errors = 0;
  uint32 pkt_errors = 0;
  uint32 sd_errors = 0;
  uint32 dup_packets = 0;
  uint32 stall_errors = 0;   // frames NAK'd early on mid-frame silence (>0.5s)
  uint32 sd_ticks_total = 0;
  uint32 sd_ticks_max = 0;
  uint32 transfer_start = 0;
  uint32 transfer_ticks = 0;
  uint32 total_size = 0;
  uint32 nsectors = 0;
  uint32 progress_step = 1;
  uint32 seq;
  uint8  payload[512];
  uint16 plen;
  struct console_rx_stats rx_stats = {0};

  crc32_init();

  printf("burn: init v2\n");

  // Open all devices before RAW mode. After SET_MODE, stdout is no
  // longer a safe debug channel because the host owns the UART stream.
  // The console is always the handshake port (shell + BURN + INFO); the
  // data port may later move to uart1, selected at runtime from INFO.
  console_fd = open("/dev/console", O_RDWR);
  if (console_fd < 0) {
    log_open_failed("console");
    exit(1);
  }
  uart_fd = console_fd;

  sdcard_fd = open("/dev/sdcard", O_RDWR);
  if (sdcard_fd < 0) {
    log_open_failed("sdcard");
    exit(1);
  }

  int oled_rc = oled_init();
  printf("burn: ready uart=%d sd=%d oled=%d\n",
         console_fd, sdcard_fd, oled_rc);

  if (ioctl(console_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_RAW) < 0) {
    show_error("RAW MODE", 0);
    close(sdcard_fd);
    close(console_fd);
    exit(1);
  }

  // Until TTY mode is restored, host communication must use uart_fd.  When
  // uart_fd is the console (single-port mode), console printf would interleave
  // with the frame stream -- keep the data path printf-free.  OLED updates are
  // kept sparse because I2C is slow and the UART RX FIFO is tiny; after ACK,
  // return to recv_msg() as quickly as possible.

  show_phase("WAIT INFO");
  write(console_fd, "BURN\n", 5);

  // INFO defines the exact number of image bytes the host will send.  The
  // host may trim unused FAT32 space, so this can be much smaller than fs.img.
  // The shell handshake always runs on the console; this packet also carries
  // the runtime data-port selection (payload[8], optional).
  type = recv_msg(console_fd, &seq, payload, &plen, 0);
  if (type != PKT_INFO || plen < 4) {
    show_error("NO INFO", 0);
    goto fail;
  }

  total_size = (uint32)payload[0] | ((uint32)payload[1] << 8)
              | ((uint32)payload[2] << 16) | ((uint32)payload[3] << 24);
  if (plen >= 8) {
    transfer_baud = (uint32)payload[4] | ((uint32)payload[5] << 8)
                  | ((uint32)payload[6] << 16) | ((uint32)payload[7] << 24);
    if (transfer_baud < 9600 || transfer_baud > 5000000)
      transfer_baud = CONSOLE_BAUD;
  }

  // Runtime data-port selection: payload[8]==1 moves the DATA path to the DW
  // UART1 (DMA RX CH5 / TX CH4).  uart1 boots in DMA mode, so the SET_*_MODE
  // calls are no-ops that document the intended path; the FLUSH discards any
  // RX residue (adapter-connect noise, a previous crash's garbage) before the
  // first real frame arrives.
  if (plen >= 9 && payload[8] == 1) {
    // Console-side diagnostics: the DATA path is on uart1, so printing to the
    // console is a safe side-channel captured by burn.py's read_console_tail.
    printf("burn: dp=1\n");
    int u1 = open("/dev/uart1", O_RDWR);
    if (u1 < 0) {
      printf("burn: open uart1 fail\n");
      show_error("OPEN UART1", 0);
      goto fail;
    }
    uart_fd = u1;

    // Pin the UART1 line rate back to console baud: a previous burn session
    // that crashed mid-transfer leaves the divisor at transfer_baud.
    ioctl(uart_fd, CONSOLE_IOCTL_SET_BAUD, CONSOLE_BAUD);
    ioctl(uart_fd, UART_IOCTL_SET_RX_MODE, UART_MODE_DMA);
    ioctl(uart_fd, UART_IOCTL_SET_TX_MODE, UART_MODE_DMA);
    ioctl(uart_fd, CONSOLE_IOCTL_FLUSH_INPUT, 0);
    printf("burn: uart1 ready\n");
  }
  nsectors  = (total_size + 511) / 512;
  progress_step = nsectors / 20;
  if (progress_step == 0)
    progress_step = 1;
  show_image_info(nsectors);

  uint32 card_sectors = 0;
  if (ioctl(sdcard_fd, SDCARD_IOCTL_NSECTORS, (uint64)&card_sectors) < 0 ||
      card_sectors < nsectors) {
    show_error("SD TOO SMALL", card_sectors);
    goto fail;
  }

  if (ioctl(sdcard_fd, SDCARD_IOCTL_SEEK, 0) < 0) {
    show_error("SD SEEK", 0);
    goto fail;
  }

  show_progress(0, nsectors);
  send_ack_payload(uart_fd, seq, ACK_INFO, 0);

  // Baud switching is two-step: first ACK the request at console baud, then
  // switch locally and wait for one more BAUD packet at the new rate.
  if (transfer_baud != CONSOLE_BAUD) {
    show_hex_value(2, "BAUD ", transfer_baud);
    show_phase("BAUD WAIT");

    type = recv_msg(uart_fd, &seq, payload, &plen, 0);
    if (type != PKT_BAUD) {
      show_error("BAUD REQ", nsectors);
      goto fail;
    }

    show_phase("BAUD READY");
    send_ack_payload(uart_fd, seq, ACK_BAUD, transfer_baud);

    ioctl(uart_fd, CONSOLE_IOCTL_SET_BAUD, transfer_baud);
    show_phase("BAUD SET");
    {
      uint32 div;
      get_uart_actual_baud(uart_fd, &div);
      show_hex_value(3, "DIV ", div);
    }

    type = recv_msg(uart_fd, &seq, payload, &plen, 0);
    if (type != PKT_BAUD) {
      show_error("BAUD SYNC", nsectors);
      goto fail;
    }
    send_ack_payload(uart_fd, seq, ACK_BAUD, get_uart_actual_baud(uart_fd, 0));
    show_progress(0, nsectors);
  }

  // Each DATA packet writes exactly one sector.  ACK only after the SD write
  // succeeds so the host can safely retry on timeout or NAK.
  transfer_start = uptime();
  for (uint32 sec = 0; sec < nsectors; ) {
    int retries = 0;
    int sector_ok = 0;

    while (retries <= MAX_RETRY) {
      type = recv_msg(uart_fd, &seq, payload, &plen, RX_STALL_NAK_TICKS);

      if (type == -2) {
        crc_errors++;
        // A stall inside this frame means the wire went silent mid-frame and the
        // retry's bytes completed a garbage frame: host TX dropped the tail.
        // Large stall (>> few ticks) => truncation; ~0 => bytes corrupted in place.
        if (diag_stall_ticks > diag_crc_stall_max) {
          diag_crc_stall_max = diag_stall_ticks;
          diag_crc_stall_max_sec = sec;
        }
        if (diag_stall_ticks > 0)
          diag_long_stalls++;
        if (uart_fd != console_fd)   // DATA on uart1: console is a free side-channel
          printf("burn: crc sec=%u stall=%u ticks atbyte=%u\n",
                 sec, diag_stall_ticks, diag_stall_at);
        show_retry("CRC", sec, retries);
        send_nak(uart_fd, sec, ERR_CRC);
        retries++;
        continue;
      }

      if (type == -3) {
        stall_errors++;
        // Host TX truncated this frame: the wire went silent mid-frame for
        // RX_STALL_NAK_TICKS and read_bytes_timeout gave up.  NAK right away
        // instead of sitting through the host's full retry timeout (which was
        // ~3s, i.e. ~50x the transfer rate of this early-NAK path).  The
        // measured gap is the silence we actually waited for.
        if (uart_fd != console_fd)
          printf("burn: stall sec=%u gap=%u ticks atbyte=%u\n",
                 sec, diag_stall_ticks, diag_stall_at);
        show_retry("STALL", sec, retries);
        send_nak(uart_fd, sec, ERR_CRC);
        retries++;
        continue;
      }

      if (type < 0) {
        io_errors++;
        if (uart_fd != console_fd)
          printf("burn: io sec=%u stall=%u ticks\n", sec, diag_stall_ticks);
        show_retry("IO", sec, retries);
        send_nak(uart_fd, sec, ERR_CRC);
        retries++;
        continue;
      }

      if (type == PKT_DATA && plen == 512 && seq < sec) {
        // The host did not see our previous ACK and retransmitted an
        // already-written sector.  ACK it again, but do not write it twice.
        dup_packets++;
        send_ack_payload(uart_fd, seq, ACK_DUP, sec);
        continue;
      }

      if (type != PKT_DATA || seq != sec || plen != 512) {
        uint8 t8 = (uint8)type;
        pkt_errors++;
        show_retry("PKT", sec, retries);
        oled_write_hexrow(3, "T:", &t8, 1);
        send_nak(uart_fd, seq, ERR_CRC);
        retries++;
        continue;
      }

      uint32 write_start = uptime();
      if (write(sdcard_fd, payload, 512) != 512) {
        sd_errors++;
        show_retry("SD", sec, retries);
        send_nak(uart_fd, sec, ERR_WRITE);
        retries++;
        continue;
      }
      uint32 write_ticks = uptime() - write_start;
      sd_ticks_total += write_ticks;
      if (write_ticks > sd_ticks_max)
        sd_ticks_max = write_ticks;

      send_ack_payload(uart_fd, sec, ACK_OK, write_ticks);
      sector_ok = 1;
      break;
    }

    if (!sector_ok) {
      show_error("RETRY LIMIT", sec);
      goto fail;
    }

    sec++;
    // OLED/I2C is slow; refresh progress sparsely so UART RX stays responsive.
    if (sec == nsectors || (sec % progress_step) == 0)
      show_progress(sec, nsectors);
  }

  // DONE lets the host distinguish "all sectors ACKed" from a clean protocol
  // close.  Restore the console before printing the final text summary.
  show_phase("WAIT DONE");
  type = recv_msg(uart_fd, &seq, payload, &plen, RX_STALL_NAK_TICKS);
  if (type == PKT_DONE)
    send_ack_payload(uart_fd, seq, ACK_DONE, 0);
  else {
    show_error("NO DONE", nsectors);
    goto fail;
  }
  transfer_ticks = uptime() - transfer_start;
  cache_rc = ioctl(sdcard_fd, SDCARD_IOCTL_INVALIDATE_CACHE, 0);
  success = 1;
  goto finish;

fail:
  success = 0;

finish:
  if (transfer_baud != CONSOLE_BAUD)
    ioctl(uart_fd, CONSOLE_IOCTL_SET_BAUD, CONSOLE_BAUD);
  get_uart_rx_stats(uart_fd, &rx_stats);
  // uart1 rejects SET_MODE(TTY) (-1, ignored).  The console must be restored
  // before the final printf so the summary is readable and the shell resumes.
  ioctl(uart_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_TTY);
  if (uart_fd != console_fd)
    ioctl(console_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_TTY);

  if (!success) {
    show_phase("BURN FAILED!");
    printf("burn: failed dup=%u crc=%u io=%u pkt=%u sd=%u stallnak=%u raw_drop=%u raw_buf=%u/%u ovr=%u stall=%u longstall=%u\n",
           dup_packets, crc_errors, io_errors, pkt_errors, sd_errors, stall_errors,
           rx_stats.dropped, rx_stats.buffered, rx_stats.capacity, rx_stats.overrun,
           diag_crc_stall_max, diag_long_stalls);
    exit(1);
  }

  show_phase("BURN SUCCESS!");
  oled_write_row(1, "");
  oled_write_row(2, "");
  oled_write_hexrow(3, "DONE", 0, 0);
  printf("burn: done sectors=%u bytes=%u cache=%d\n",
         nsectors, total_size, cache_rc);
  printf("burn: stats ticks=%u sd_total=%u sd_max=%u dup=%u crc=%u io=%u pkt=%u sd=%u stallnak=%u raw_drop=%u raw_buf=%u/%u ovr=%u stall=%u sec=%u longstall=%u\n",
         transfer_ticks, sd_ticks_total, sd_ticks_max, dup_packets,
         crc_errors, io_errors, pkt_errors, sd_errors, stall_errors,
         rx_stats.dropped, rx_stats.buffered, rx_stats.capacity, rx_stats.overrun,
         diag_crc_stall_max, diag_crc_stall_max_sec, diag_long_stalls);
  exit(0);
}