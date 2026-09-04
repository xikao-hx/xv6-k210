// Burn a FAT filesystem image to the SD card over UART.
//
// Transfer phases:
//   1. Console handshake: announce BURN and receive image INFO.
//   2. Link setup: select the data UART and synchronize the baud rate.
//   3. Data transfer: validate, write and ACK one 512-byte sector at a time.
//   4. Completion: receive DONE, invalidate the SD cache and restore the TTY.
//
// INFO selects the data path at runtime:
//   data_port == 0 -> /dev/console (UARTHS, interrupt RX)
//   data_port == 1 -> /dev/ttyS0 (DW UART1, DMA RX/TX)
// INFO always arrives on the console; later framed traffic may move to ttyS0.
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
// DATA frames are NAKed after 0.5 s of mid-frame silence (5 ms/tick).
#define RX_STALL_NAK_TICKS 100

#define PKT_INFO   0x01  // Host to board: payload is total image size.
#define PKT_DATA   0x02  // Host to board: payload is one 512-byte sector.
#define PKT_DONE   0x03  // Host to board: transfer complete.
#define PKT_BAUD   0x04  // Host to board: request/confirm baud switch.
#define PKT_ACK    0x81  // Board to host: packet accepted.
#define PKT_NAK    0x82  // Board to host: packet rejected; payload is error.

#define RECV_ERR_IO        -1
#define RECV_ERR_CRC       -2
#define RECV_ERR_TRUNCATED -3
#define RECV_ERR_PROTOCOL  -4

#define READ_ERR_IO        -1
#define READ_ERR_TIMEOUT -2

#define ERR_CRC       0x01
#define ERR_WRITE     0x02
#define ERR_TRUNCATED 0x03
#define ERR_PROTOCOL  0x04
#define ERR_IO        0x05

#define ACK_OK       0x00
#define ACK_DUP      0x01
#define ACK_INFO     0x02
#define ACK_DONE     0x03
#define ACK_BAUD     0x04

#define BURN_PROGRESS_TITLE "WRITING FS V2"

/* ---- UART diagnostics ---- */

// A long gap inside a frame distinguishes a truncated frame from byte damage.
// Console-mode diagnostics are deferred because printf would corrupt the link.
static uint diag_stall_ticks;     // longest gap in the current frame
static uint diag_stall_at;        // byte offset where that gap began
static uint diag_crc_stall_max;   // longest gap among CRC failures
static uint diag_crc_stall_max_sec;
static uint diag_crc_stall_max_at;
static uint diag_long_stalls;

static void get_uart_rx_stats(int fd, struct console_rx_stats *stats);

/* ---- OLED show helper ---- */

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
oled_write_hexrow(uint8 row, const char *label, const uint8 *data, int n)
{
  OLED_ClearArea(0, row * 16, OLEDFB_W, 16);  /* clear row */
  OLED_ShowHexRow(0, row * 16, label, data, n, OLED_8X16);
  OLED_Flush();
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

/* ---- Phase show ---- */

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

/* ---- CRC32 ---- */

// Reflected CRC32 compatible with the host's zlib.crc32().
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

/* ---- framed transport ---- */

// Poll ring depth so a truncated DATA frame can time out before the host retry.
// max_gap_ticks == 0 uses blocking reads.
// Returns 0 on success or a READ_ERR_* code.
static int
read_bytes_timeout(int fd, uint8 *buf, int n, uint max_gap_ticks)
{
  int off = 0;
  uint t_last = uptime();

  while (off < n) {
    int want = n - off;

    if (max_gap_ticks != 0) {
      struct console_rx_stats st;
      get_uart_rx_stats(fd, &st);
      if (st.buffered == 0) {
        uint gap = uptime() - t_last;
        if (gap > max_gap_ticks) {
          diag_stall_ticks = gap;
          diag_stall_at = off;
          return READ_ERR_TIMEOUT;
        }
        continue;
      }
      if (want > (int)st.buffered)
        want = st.buffered;
    }

    int r = read(fd, buf + off, want);
    if (r <= 0)
      return READ_ERR_IO;

    uint t_now = uptime();
    uint gap = t_now - t_last;
    if (gap > diag_stall_ticks) {
      diag_stall_ticks = gap;
      diag_stall_at = off;
    }
    t_last = t_now;
    off += r;
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

// Scan to magic and receive one frame.
static int
recv_msg(int fd, uint32 *seq_out, uint8 *payload_buf, uint16 *plen_out,
         uint stall_nak_ticks)
{
  uint8 sync[4] = {0};
  int   si = 0;

  diag_stall_ticks = 0;
  diag_stall_at = 0;

resync:
  while (1) {
    uint8 ch;
    int rr = read_bytes_timeout(fd, &ch, 1, stall_nak_ticks);
    if (rr < 0)
      return rr == READ_ERR_TIMEOUT ? RECV_ERR_TRUNCATED : RECV_ERR_IO;
    sync[si] = ch;
    si = (si + 1) & 3;

    uint8 w[4];
    for (int i = 0; i < 4; i++)
      w[i] = sync[(si + i) & 3];
    if (w[0] == 0x55 && w[1] == 0xAA && w[2] == 0x55 && w[3] == 0xAA)
      break;
  }

  // Inter-frame waiting is not a stall inside the frame.
  diag_stall_ticks = 0;
  diag_stall_at = 0;

  uint8 hdr[7];
  int rr = read_bytes_timeout(fd, hdr, 7, stall_nak_ticks);
  if (rr < 0) {
    if (rr == READ_ERR_TIMEOUT)
      return RECV_ERR_TRUNCATED;
    show_phase("RECV FAIL");
    oled_write_row(2, "HDR");
    return RECV_ERR_IO;
  }

  uint32 seq = (uint32)hdr[0] | ((uint32)hdr[1] << 8)
             | ((uint32)hdr[2] << 16) | ((uint32)hdr[3] << 24);
  uint8  type  = hdr[4];
  uint16 plen  = (uint16)hdr[5] | ((uint16)hdr[6] << 8);

  // Reject false magic inside payload data without losing an overlapping frame.
  if (type != PKT_INFO && type != PKT_DATA && type != PKT_DONE &&
      type != PKT_BAUD &&
      type != PKT_ACK && type != PKT_NAK) {
    sync[0] = hdr[3];
    sync[1] = hdr[4];
    sync[2] = hdr[5];
    sync[3] = hdr[6];
    si = 0;
    goto resync;
  }

  if (plen > 512) {
    show_phase("BAD PACKET");
    oled_write_row(2, "PLEN ");
    oled_show_hex_num(2, 5, plen, 4);
    oled_write_hexrow(3, "H:", hdr, 7);
    return RECV_ERR_PROTOCOL;
  }

  if (plen > 0) {
    rr = read_bytes_timeout(fd, payload_buf, plen, stall_nak_ticks);
    if (rr < 0) {
      if (rr == READ_ERR_TIMEOUT)
        return RECV_ERR_TRUNCATED;
      show_phase("RECV FAIL");
      oled_write_row(2, "PAYLOAD");
      return RECV_ERR_IO;
    }
  }

  uint8 crc_raw[4];
  rr = read_bytes_timeout(fd, crc_raw, 4, stall_nak_ticks);
  if (rr < 0) {
    if (rr == READ_ERR_TIMEOUT)
      return RECV_ERR_TRUNCATED;
    show_phase("RECV FAIL");
    oled_write_row(2, "CRC BYTES");
    return RECV_ERR_IO;
  }
  uint32 msg_crc = (uint32)crc_raw[0] | ((uint32)crc_raw[1] << 8)
                 | ((uint32)crc_raw[2] << 16) | ((uint32)crc_raw[3] << 24);

  uint8 magic[4] = {0x55, 0xAA, 0x55, 0xAA};
  uint32 csum = crc32(magic, 4, 0);
  csum = crc32(hdr, 7, csum);
  if (plen > 0)
    csum = crc32(payload_buf, plen, csum);

  if (csum != msg_crc)
    return RECV_ERR_CRC;

  *seq_out   = seq;
  *plen_out  = plen;
  return (int)type;
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
send_ack(int fd, uint32 seq, uint8 reason, uint32 ticks)
{
  uint8 payload[5];
  payload[0] = reason;
  put32le(payload + 1, ticks);
  send_msg(fd, seq, PKT_ACK, payload, sizeof(payload));
}

static void
send_nak(int fd, uint32 seq, uint8 err)
{
  send_msg(fd, seq, PKT_NAK, &err, 1);
}

/* ---- Get uart status ---- */

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

int
main(void)
{
  int console_fd, uart_fd = -1, transfer_fd, sdcard_fd;
  int type;
  int success = 0;
  int cache_rc = -1;
  uint32 transfer_baud = CONSOLE_BAUD;
  uint32 crc_errors = 0;
  uint32 io_errors = 0;
  uint32 pkt_errors = 0;
  uint32 sd_errors = 0;
  uint32 dup_packets = 0;
  uint32 stall_errors = 0;
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

  /* ---- phase 1: device initialization ---- */

  // Open devices before RAW mode takes ownership of the console stream.
  console_fd = open("/dev/console", O_RDWR);
  if (console_fd < 0) {
    printf("burn: open console failed\n");
    exit(1);
  }
  transfer_fd = console_fd;

  sdcard_fd = open("/dev/sdcard", O_RDWR);
  if (sdcard_fd < 0) {
    printf("burn: open sdcard failed\n");
    exit(1);
  }

  oled_init();

  /* ---- phase 2: console handshake and INFO ---- */

  if (ioctl(console_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_RAW) < 0) {
    show_error("RAW MODE", 0);
    close(sdcard_fd);
    close(console_fd);
    exit(1);
  }

  show_phase("WAIT INFO");
  write(transfer_fd, "BURN\n", 5);

  // INFO carries the trimmed image size, transfer baud and optional data port.
  type = recv_msg(transfer_fd, &seq, payload, &plen, 0);
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

  /* ---- phase 3: data path and target validation ---- */

  if (plen >= 9 && payload[8] == 1) {
    // Once DATA moves to ttyS0, the console becomes a diagnostic side channel.
    uart_fd = open("/dev/ttyS0", O_RDWR);
    if (uart_fd < 0) {
      printf("burn: open ttyS0 fail\n");
      show_error("OPEN TTYS0", 0);
      goto fail;
    }

    // A crashed transfer may leave ttyS0 at the previous transfer baud.
    ioctl(uart_fd, CONSOLE_IOCTL_SET_BAUD, CONSOLE_BAUD);
    ioctl(uart_fd, UART_IOCTL_SET_MODE, UART_MODE_DMA);
    ioctl(uart_fd, CONSOLE_IOCTL_FLUSH_INPUT, 0);
    transfer_fd = uart_fd;
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
  send_ack(transfer_fd, seq, ACK_INFO, 0);

  /* ---- phase 4: baud synchronization ---- */

  // Confirm BAUD once before and once after changing the local divisor.
  if (transfer_baud != CONSOLE_BAUD) {
    show_dec_value(2, "BAUD ", transfer_baud);
    show_phase("BAUD WAIT");

    type = recv_msg(transfer_fd, &seq, payload, &plen, 0);
    if (type != PKT_BAUD) {
      show_error("BAUD REQ", nsectors);
      goto fail;
    }

    show_phase("BAUD READY");
    send_ack(transfer_fd, seq, ACK_BAUD, transfer_baud);

    ioctl(transfer_fd, CONSOLE_IOCTL_SET_BAUD, transfer_baud);
    show_phase("BAUD SET");
    {
      uint32 div;
      get_uart_actual_baud(transfer_fd, &div);
      show_dec_value(3, "DIV ", div);
    }

    type = recv_msg(transfer_fd, &seq, payload, &plen, 0);
    if (type != PKT_BAUD) {
      show_error("BAUD SYNC", nsectors);
      goto fail;
    }
    send_ack(transfer_fd, seq, ACK_BAUD,
             get_uart_actual_baud(transfer_fd, 0));
    show_progress(0, nsectors);
  }

  /* ---- phase 5: sector transfer ---- */

  // ACK only after the 512-byte sector has been written successfully.
  transfer_start = uptime();
  for (uint32 sec = 0; sec < nsectors; ) {
    int retries = 0;
    int sector_ok = 0;

    while (retries <= MAX_RETRY) {
      type = recv_msg(transfer_fd, &seq, payload, &plen,
                      RX_STALL_NAK_TICKS);

      if (type == RECV_ERR_CRC) {
        crc_errors++;
        if (diag_stall_ticks > diag_crc_stall_max) {
          diag_crc_stall_max = diag_stall_ticks;
          diag_crc_stall_max_sec = sec;
          diag_crc_stall_max_at = diag_stall_at;
        }
        if (diag_stall_ticks > 0)
          diag_long_stalls++;
        show_retry("CRC", sec, retries);
        send_nak(transfer_fd, sec, ERR_CRC);
        retries++;
        continue;
      }

      if (type == RECV_ERR_TRUNCATED) {
        stall_errors++;
        show_retry("STALL", sec, retries);
        send_nak(transfer_fd, sec, ERR_TRUNCATED);
        retries++;
        continue;
      }

      if (type == RECV_ERR_PROTOCOL) {
        pkt_errors++;
        show_retry("PKT", sec, retries);
        send_nak(transfer_fd, sec, ERR_PROTOCOL);
        retries++;
        continue;
      }

      if (type == RECV_ERR_IO) {
        io_errors++;
        show_retry("IO", sec, retries);
        send_nak(transfer_fd, sec, ERR_IO);
        retries++;
        continue;
      }

      if (type == PKT_DATA && plen == 512 && seq < sec) {
        // Lost ACK: acknowledge the duplicate without writing it twice.
        dup_packets++;
        send_ack(transfer_fd, seq, ACK_DUP, sec);
        continue;
      }

      if (type != PKT_DATA || seq != sec || plen != 512) {
        uint8 t8 = (uint8)type;
        pkt_errors++;
        show_retry("PKT", sec, retries);
        oled_write_hexrow(3, "T:", &t8, 1);
        send_nak(transfer_fd, seq, ERR_PROTOCOL);
        retries++;
        continue;
      }

      uint32 write_start = uptime();
      if (write(sdcard_fd, payload, 512) != 512) {
        sd_errors++;
        show_retry("SD", sec, retries);
        send_nak(transfer_fd, sec, ERR_WRITE);
        retries++;
        continue;
      }
      uint32 write_ticks = uptime() - write_start;
      if (write_ticks > sd_ticks_max)
        sd_ticks_max = write_ticks;

      send_ack(transfer_fd, sec, ACK_OK, write_ticks);
      sector_ok = 1;
      break;
    }

    if (!sector_ok) {
      show_error("RETRY LIMIT", sec);
      goto fail;
    }

    sec++;
    // Sparse OLED refresh avoids blocking the shallow UART RX path on I2C.
    if (sec == nsectors || (sec % progress_step) == 0)
      show_progress(sec, nsectors);
  }

  /* ---- phase 6: transfer completion ---- */

  show_phase("WAIT DONE");
  type = recv_msg(transfer_fd, &seq, payload, &plen,
                  RX_STALL_NAK_TICKS);
  if (type == PKT_DONE)
    send_ack(transfer_fd, seq, ACK_DONE, 0);
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
  /* ---- phase 7: link cleanup and result ---- */

  if (transfer_baud != CONSOLE_BAUD)
    ioctl(transfer_fd, CONSOLE_IOCTL_SET_BAUD, CONSOLE_BAUD);
  get_uart_rx_stats(transfer_fd, &rx_stats);
  // Restore the console before printf returns the stream to the shell.
  ioctl(transfer_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_TTY);
  if (transfer_fd != console_fd)
    ioctl(console_fd, CONSOLE_IOCTL_SET_MODE, CONSOLE_MODE_TTY);

  if (!success) {
    show_phase("BURN FAILED!");
    printf("burn: fail crc=%u stall=%u io=%u pkt=%u sd=%u dup=%u drop=%u ovr=%u worst=%u@%u:%u long=%u\n",
           crc_errors, stall_errors, io_errors, pkt_errors, sd_errors, dup_packets,
           rx_stats.dropped, rx_stats.overrun, diag_crc_stall_max,
           diag_crc_stall_max_sec, diag_crc_stall_max_at, diag_long_stalls);
    exit(1);
  }

  show_phase("BURN SUCCESS!");
  oled_write_row(1, "");
  oled_write_row(2, "");
  oled_write_hexrow(3, "DONE", 0, 0);
  printf("burn: ok sectors=%u bytes=%u ticks=%u crc=%u stall=%u io=%u pkt=%u sd=%u dup=%u drop=%u ovr=%u sdmax=%u worst=%u@%u:%u long=%u cache=%d\n",
         nsectors, total_size, transfer_ticks, crc_errors, stall_errors,
         io_errors, pkt_errors, sd_errors, dup_packets, rx_stats.dropped,
         rx_stats.overrun, sd_ticks_max, diag_crc_stall_max,
         diag_crc_stall_max_sec, diag_crc_stall_max_at, diag_long_stalls,
         cache_rc);
  exit(0);
}
