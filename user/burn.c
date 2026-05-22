// Burn a FAT filesystem image to the SD card through the raw UART device.
//
// Host flow:
//   1. Send "burn\n" to the shell, which starts this program.
//   2. Wait for "BURN\n".
//   3. Send INFO with total image size.
//   4. Send one 512-byte DATA packet per sector.
//   5. Send DONE after all sectors are acknowledged.
//
// Packet format:
//   magic(4: 55 AA 55 AA), seq(4 LE), type(1), plen(2 LE),
//   payload(plen), crc32(4 LE over magic through payload).

#include "types.h"
#include "file.h"
#include "uartdev.h"
#include "sdcarddev.h"
#include "oled.h"
#include "user/user.h"
#include "fcntl.h"

#define MAX_RETRY    5

#define PKT_INFO   0x01  // Host to board: payload is total image size.
#define PKT_DATA   0x02  // Host to board: payload is one 512-byte sector.
#define PKT_DONE   0x03  // Host to board: transfer complete.
#define PKT_ACK    0x81  // Board to host: packet accepted.
#define PKT_NAK    0x82  // Board to host: packet rejected; payload is error.

#define ERR_CRC    0x01
#define ERR_WRITE  0x02

#define ACK_OK       0x00
#define ACK_DUP      0x01
#define ACK_INFO     0x02
#define ACK_DONE     0x03

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
show_phase(const char *phase)
{
  oled_write_row(0, phase);
}

static void
show_image_info(uint32 nsectors)
{
  show_hex_value(1, "TOT ", nsectors);
  oled_write_row(2, "");
  oled_write_row(3, "");
}

static void
show_error(const char *msg, uint32 sec)
{
  show_phase("BURN ERROR");
  oled_write_row(2, msg);
  show_hex_value(3, "SEC ", sec);
}

static void
show_retry(const char *why, uint32 sec, int retry)
{
  show_phase("RETRY");
  oled_printf(2, 0, "%s %d/%d", why, retry + 1, MAX_RETRY);
  show_hex_value(3, "SEC ", sec);
}

// CRC32, reflected polynomial 0xEDB88320, compatible with zlib.crc32().

static uint32 crc32_tab[256];
static int    crc32_inited = 0;

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
  crc32_inited = 1;
}

static uint32
crc32(const uint8 *data, int len, uint32 crc)
{
  if (!crc32_inited) crc32_init();
  crc = ~crc;
  for (int i = 0; i < len; i++)
    crc = crc32_tab[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// Read exactly n bytes from the interrupt-fed raw UART ring.
static int
read_bytes(int fd, uint8 *buf, int n)
{
  int off = 0;

  while (off < n) {
    int r = read(fd, buf + off, n - off);
    if (r <= 0)
      return -1;
    off += r;
  }

  return 0;
}

static void
send_msg(int fd, uint32 seq, uint8 type, const uint8 *payload, uint16 plen)
{
  uint8 hdr[11];
  hdr[0] = 0x55; hdr[1] = 0xAA; hdr[2] = 0x55; hdr[3] = 0xAA;
  hdr[4] = seq & 0xFF; hdr[5] = (seq >> 8) & 0xFF;
  hdr[6] = (seq >> 16) & 0xFF; hdr[7] = (seq >> 24) & 0xFF;
  hdr[8] = type;
  hdr[9] = plen & 0xFF; hdr[10] = (plen >> 8) & 0xFF;

  uint32 csum = crc32(hdr, 11, 0);
  if (payload && plen)
    csum = crc32(payload, plen, csum);

  uint8 crc_buf[4];
  crc_buf[0] = csum & 0xFF; crc_buf[1] = (csum >> 8) & 0xFF;
  crc_buf[2] = (csum >> 16) & 0xFF; crc_buf[3] = (csum >> 24) & 0xFF;

  write(fd, hdr, 11);
  if (payload && plen)
    write(fd, payload, plen);
  write(fd, crc_buf, 4);
}

// Return the packet type, -1 on I/O/protocol error, or -2 on CRC mismatch.
// The stream is self-synchronizing: stray bytes are skipped until magic.
static int
recv_msg(int fd, uint32 *seq_out, uint8 *payload_buf, uint16 *plen_out)
{
  uint8 sync[4];
  int   si = 0;

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
  if (read_bytes(fd, hdr, 7) < 0) {
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
      type != PKT_ACK && type != PKT_NAK) {
    // Re-seed the sliding window with bytes that followed the false magic,
    // so a real magic that overlaps this region is not skipped.
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

  if (plen > 0 && read_bytes(fd, payload_buf, plen) < 0) {
    show_phase("RECV FAIL");
    oled_write_row(2, "PAYLOAD");
    return -1;
  }

  uint8 crc_raw[4];
  if (read_bytes(fd, crc_raw, 4) < 0) {
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

static void
send_nak(int fd, uint32 seq, uint8 err)
{
  send_msg(fd, seq, PKT_NAK, &err, 1);
}

int
main(void)
{
  int uart_fd, sdcard_fd;

  uart_fd = dev(O_RDWR, UART_DEV, 0);
  if (uart_fd < 0) {
    printf("Error: cannot open /dev/uart\n");
    exit(1);
  }

  sdcard_fd = dev(O_RDWR, SDCARD_DEV, 0);
  if (sdcard_fd < 0) {
    printf("Error: cannot open /dev/sdcard\n");
    exit(1);
  }

  oled_init();

  printf("SD Card Burn Utility\n");

  // Raw mode prevents the console interrupt handler from stealing bytes
  // while this program polls the UART.
  ioctl(uart_fd, UART_IOCTL_RAW_START, 0);

  // Until RAW_END, host communication must use uart_fd, not printf().
  // OLED updates are kept sparse because I2C is slow and the UART RX FIFO
  // is tiny; after ACK, return to recv_msg() as quickly as possible.

  show_phase("WAIT INFO");
  write(uart_fd, "BURN\n", 5);

  uint32 total_size, nsectors;
  uint32 seq;
  uint8  payload[512];
  uint16 plen;

  int type = recv_msg(uart_fd, &seq, payload, &plen);
  if (type != PKT_INFO || plen < 4) {
    show_error("NO INFO", 0);
    goto fail;
  }

  total_size = (uint32)payload[0] | ((uint32)payload[1] << 8)
              | ((uint32)payload[2] << 16) | ((uint32)payload[3] << 24);
  nsectors  = (total_size + 511) / 512;
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

  show_phase("WRITING fs.img");
  send_ack_payload(uart_fd, seq, ACK_INFO, 0);

  for (uint32 sec = 0; sec < nsectors; ) {
    int retries = 0;
    int ok = 0;

    while (retries <= MAX_RETRY) {
      uint32 rseq;
      uint8  payload[512];
      uint16 plen;

      int type = recv_msg(uart_fd, &rseq, payload, &plen);

      if (type == -2) {
        show_retry("CRC", sec, retries);
        send_nak(uart_fd, sec, ERR_CRC);
        retries++;
        continue;
      }

      if (type < 0) {
        show_retry("IO", sec, retries);
        send_nak(uart_fd, sec, ERR_CRC);
        retries++;
        continue;
      }

      if (type == PKT_DATA && plen == 512 && rseq < sec) {
        // The host did not see our previous ACK and retransmitted an
        // already-written sector.  ACK it again, but do not write it twice.
        send_ack_payload(uart_fd, rseq, ACK_DUP, sec);
        continue;
      }

      if (type != PKT_DATA || rseq != sec || plen != 512) {
        show_retry("PKT", sec, retries);
        { uint8 t8 = (uint8)type; oled_write_hexrow(3, "T:", &t8, 1); }
        send_nak(uart_fd, rseq, ERR_CRC);
        retries++;
        continue;
      }

      uint32 write_start = uptime();
      if (write(sdcard_fd, payload, 512) != 512) {
        show_retry("SD", sec, retries);
        send_nak(uart_fd, sec, ERR_WRITE);
        retries++;
        continue;
      }
      uint32 write_ticks = uptime() - write_start;

      send_ack_payload(uart_fd, sec, ACK_OK, write_ticks);
      ok = 1;
      break;
    }

    if (!ok) {
      show_error("RETRY LIMIT", sec);
      goto fail;
    }

    sec++;
  }

  show_phase("WAIT DONE");
  {
    uint32 seq;
    uint8  payload[512];
    uint16 plen;
    int type = recv_msg(uart_fd, &seq, payload, &plen);
    if (type == PKT_DONE)
      send_ack_payload(uart_fd, seq, ACK_DONE, 0);
    else {
      show_error("NO DONE", nsectors);
      goto fail;
    }
  }

  ioctl(uart_fd, UART_IOCTL_RAW_END, 0);
  show_phase("BURN SUCCESS!");
  oled_write_row(1, "");
  oled_write_row(2, "");
  oled_write_hexrow(3, "DONE", 0, 0);
  printf("Done: %u sectors, %u bytes\n", nsectors, total_size);
  exit(0);

fail:
  ioctl(uart_fd, UART_IOCTL_RAW_END, 0);
  show_phase("BURN FAILED!");
  printf("Burn failed\n");
  exit(1);
}
