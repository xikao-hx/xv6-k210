#!/usr/bin/env python3
"""
Burn a filesystem image to xv6-k210 SD card via UART.

Uses CRC32-protected framed messages (inspired by kflash.py) for
reliable data transfer.  The shell handshake starts at 115200 baud;
after INFO is acknowledged, the host switches to the transfer baud and
the board switches to the board-side baud setting for DATA/DONE.  On the
current K210 UARTHS path the board-side baud needs a small compensation
for the host to decode board replies correctly at the default fast baud.

Usage:
    python3 tools/burn.py [--baud RATE] <serial_port> [image_file]

Protocol:
  Phase 1:
    1. Host sends "burn\\n"              to shell → starts burn program
    2. Board sends "BURN\\n"             → host knows program is alive
    3. Host sends INFO message            → total size LE + board baud LE
    4. Board sends ACK message
    5. Host and board switch baud and exchange BAUD sync messages

  Phase 2 (CRC32-protected messages at transfer baud):
    6. Host sends DATA messages (one per 512-byte sector)
    7. Board ACKs or NAKs  → host retries on NAK (up to 5 tries)
    8. Host sends DONE message
    9. Board sends ACK

Message format:
    [magic:4=0x55AA55AA][seq:4 LE][type:1][plen:2 LE][payload:plen][crc32:4 LE]
"""

import struct
import sys
import time
import argparse
import zlib

import serial

PKT_INFO = 0x01
PKT_DATA = 0x02
PKT_DONE = 0x03
PKT_BAUD = 0x04
PKT_ACK  = 0x81
PKT_NAK  = 0x82

ERR_CRC   = 0x01
ERR_WRITE = 0x02

ACK_OK = 0x00
ACK_DUP = 0x01
ACK_INFO = 0x02
ACK_DONE = 0x03
ACK_BAUD = 0x04

MAGIC = b'\x55\xAA\x55\xAA'
MAX_RETRY = 5
CONSOLE_BAUD = 115200
BOARD_BAUD_COMP_NUM = 11
BOARD_BAUD_COMP_DEN = 10
SECTOR_SIZE = 512


def le16(data, off):
    return struct.unpack_from('<H', data, off)[0]


def le32(data, off):
    return struct.unpack_from('<I', data, off)[0]


def default_board_baud(host_baud, console_baud):
    if host_baud == console_baud:
        return host_baud
    return (
        host_baud * BOARD_BAUD_COMP_NUM + BOARD_BAUD_COMP_DEN // 2
    ) // BOARD_BAUD_COMP_DEN


def hexdump(data):
    return data.hex(" ").upper() if data else "-"


def printable_serial(data):
    text = []
    for b in data:
        if b == 10:
            text.append("\\n")
        elif b == 13:
            text.append("\\r")
        elif 32 <= b <= 126:
            text.append(chr(b))
        else:
            text.append(f"\\x{b:02x}")
    return "".join(text)


def align_up(value, align):
    return (value + align - 1) // align * align


def fat32_effective_size(img_data):
    """Return (size, info) for the FAT32 region that actually matters.

    The whole reserved/FAT area is always kept.  The data area is trimmed
    after the highest allocated cluster, so stale free-space sectors at the
    end of a fixed-size fs.img are not sent over UART.
    """
    full_size = len(img_data)
    if full_size < SECTOR_SIZE:
        raise ValueError("image is smaller than one sector")

    bytes_per_sector = le16(img_data, 11)
    sectors_per_cluster = img_data[13]
    reserved_sectors = le16(img_data, 14)
    num_fats = img_data[16]
    root_entries = le16(img_data, 17)
    total_sectors = le16(img_data, 19) or le32(img_data, 32)
    fat_size = le16(img_data, 22) or le32(img_data, 36)
    root_cluster = le32(img_data, 44)

    if bytes_per_sector != SECTOR_SIZE:
        raise ValueError(f"unsupported sector size {bytes_per_sector}")
    if sectors_per_cluster == 0 or reserved_sectors == 0 or num_fats == 0:
        raise ValueError("invalid FAT BPB")
    if root_entries != 0 or fat_size == 0 or root_cluster < 2:
        raise ValueError("image is not FAT32")
    if total_sectors == 0:
        total_sectors = full_size // SECTOR_SIZE

    image_sectors = full_size // SECTOR_SIZE
    total_sectors = min(total_sectors, image_sectors)
    fat_start = reserved_sectors * SECTOR_SIZE
    data_start_sector = reserved_sectors + num_fats * fat_size
    data_start = data_start_sector * SECTOR_SIZE

    if data_start > full_size or fat_start + fat_size * SECTOR_SIZE > full_size:
        raise ValueError("FAT metadata points outside image")

    data_sectors = total_sectors - data_start_sector
    if data_sectors <= 0:
        raise ValueError("image has no FAT data area")
    cluster_count = data_sectors // sectors_per_cluster
    max_cluster = cluster_count + 1
    fat_entries = min(fat_size * SECTOR_SIZE // 4, max_cluster + 1)

    last_cluster = 0
    for cluster in range(2, fat_entries):
        entry = le32(img_data, fat_start + cluster * 4) & 0x0FFFFFFF
        if entry != 0:
            last_cluster = cluster

    if last_cluster == 0:
        effective_sectors = data_start_sector
    else:
        effective_sectors = (
            data_start_sector +
            (last_cluster - 2 + 1) * sectors_per_cluster
        )

    effective_sectors = min(effective_sectors, total_sectors)
    effective_size = align_up(effective_sectors * SECTOR_SIZE, SECTOR_SIZE)
    info = {
        "bytes_per_sector": bytes_per_sector,
        "sectors_per_cluster": sectors_per_cluster,
        "reserved_sectors": reserved_sectors,
        "num_fats": num_fats,
        "fat_size": fat_size,
        "data_start_sector": data_start_sector,
        "total_sectors": total_sectors,
        "last_cluster": last_cluster,
        "effective_sectors": effective_sectors,
    }
    return effective_size, info


def pkt_name(type_):
    return {
        PKT_INFO: "INFO",
        PKT_DATA: "DATA",
        PKT_DONE: "DONE",
        PKT_BAUD: "BAUD",
        PKT_ACK: "ACK",
        PKT_NAK: "NAK",
    }.get(type_, f"0x{type_:02X}")


def ack_info(payload):
    if len(payload) >= 5:
        reason = payload[0]
        ticks = struct.unpack('<I', payload[1:5])[0]
        reason_name = {
            ACK_OK: "OK",
            ACK_DUP: "DUP",
            ACK_INFO: "INFO",
            ACK_DONE: "DONE",
            ACK_BAUD: "BAUD",
        }.get(reason, f"0x{reason:02X}")
        return reason_name, ticks
    return "OLD", 0


def in_waiting(ser):
    try:
        return ser.in_waiting
    except Exception:
        return "?"


def readall(ser, n, label="bytes"):
    """Read exactly n bytes from serial port (raises on timeout)."""
    data = b''
    while len(data) < n:
        chunk = ser.read(n - len(data))
        if not chunk:
            raise TimeoutError(
                f"timeout reading {label}: got {len(data)} of {n} bytes, "
                f"partial={hexdump(data)}, in_waiting={in_waiting(ser)}")
        data += chunk
    return data


def read_console_tail(ser, timeout=0.8):
    old_timeout = ser.timeout
    ser.timeout = 0.05
    end = time.monotonic() + timeout
    data = b""
    try:
        while time.monotonic() < end:
            chunk = ser.read(128)
            if chunk:
                data += chunk
                end = time.monotonic() + 0.2
    finally:
        ser.timeout = old_timeout
    return data


def send_msg(ser, seq, type_, payload=b''):
    """Send a framed message over serial."""
    hdr = MAGIC + struct.pack('<IB', seq, type_) + struct.pack('<H', len(payload))
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    ser.write(hdr + payload + struct.pack('<I', crc))
    ser.flush()


def send_shell_command(ser, cmd):
    """Clear the current shell input line and send one command."""
    ser.write(b"\x15")
    ser.write(cmd.encode("ascii") + b"\n")
    ser.flush()


def recv_msg(ser, timeout=5, context="recv"):
    """Receive a framed message.

    Returns (seq, type_, payload).
    Raises TimeoutError / ValueError on failure.
    """
    old_timeout = ser.timeout
    ser.timeout = timeout
    start = time.monotonic()
    scanned = 0
    last = bytearray()
    try:
        # Scan for magic
        buf = b'\x00' * 3
        while True:
            c = ser.read(1)
            if not c:
                elapsed = time.monotonic() - start
                raise TimeoutError(
                    f"{context}: timeout waiting for magic after {elapsed:.2f}s, "
                    f"scanned={scanned}, last={hexdump(last[-16:])}, "
                    f"in_waiting={in_waiting(ser)}")
            scanned += 1
            last += c
            if len(last) > 16:
                del last[:-16]
            buf = (buf + c)[-4:]
            if buf == MAGIC:
                break

        # Header after magic: seq(4) + type(1) + plen(2)
        hdr = readall(ser, 7, f"{context} header")
        seq = struct.unpack('<I', hdr[:4])[0]
        type_ = hdr[4]
        plen = struct.unpack('<H', hdr[5:7])[0]

        # Payload
        payload = readall(ser, plen, f"{context} payload") if plen > 0 else b''

        # CRC32
        recv_crc = struct.unpack('<I', readall(ser, 4, f"{context} crc"))[0]
        expect_crc = zlib.crc32(MAGIC + hdr + payload) & 0xFFFFFFFF
        if recv_crc != expect_crc:
            raise ValueError(
                f"{context}: CRC mismatch seq={seq} type={pkt_name(type_)} "
                f"plen={plen} got=0x{recv_crc:08X} expected=0x{expect_crc:08X}")

        return seq, type_, payload
    finally:
        ser.timeout = old_timeout


def main():
    parser = argparse.ArgumentParser(
        description="Burn a filesystem image to xv6-k210 SD card via UART")
    parser.add_argument("port", help="Serial port (e.g. /dev/ttyUSB2)")
    parser.add_argument("image", nargs="?", default="target/fs.img",
                        help="Filesystem image file (default: target/fs.img)")
    parser.add_argument("--baud", "-b", type=int, default=230400,
                        help="UART transfer baud after handshake (default: 230400)")
    parser.add_argument("--board-baud", type=int, default=0,
                        help="Baud value sent to the board (default: compensated from --baud)")
    parser.add_argument("--console-baud", type=int, default=CONSOLE_BAUD,
                        help="Shell handshake baud (default: 115200)")
    parser.add_argument("--burn-cmd", default="/burn",
                        help="Command used to start the board burn program (default: /burn)")
    parser.add_argument("--full-image", action="store_true",
                        help="Send the whole image file instead of trimming FAT32 free space")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print computed transfer size and exit before opening serial")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print every DATA/ACK exchange, not just startup and failures")
    args = parser.parse_args()

    port = args.port
    img_path = args.image

    board_baud = args.board_baud
    if board_baud == 0:
        board_baud = default_board_baud(args.baud, args.console_baud)

    if args.baud < 9600 or args.baud > 5000000 or board_baud < 9600 or board_baud > 5000000:
        print("Transfer baud must be between 9600 and 5000000")
        if args.board_baud == 0 and board_baud > 5000000:
            print("The compensated board baud is too high; pass --board-baud explicitly.")
        sys.exit(1)

    # Read the image file
    with open(img_path, "rb") as f:
        img_data = f.read()

    full_size = len(img_data)
    img_size = full_size
    fat_info = None
    if not args.full_image:
        try:
            img_size, fat_info = fat32_effective_size(img_data)
            img_data = img_data[:img_size]
        except ValueError as e:
            print(f"FAT32 trim disabled: {e}; sending full image")

    nsectors = (img_size + SECTOR_SIZE - 1) // SECTOR_SIZE
    print(f"Image: {img_path}")
    print(f"Size: {img_size} bytes ({nsectors} sectors)")
    if fat_info:
        saved = full_size - img_size
        saved_pct = saved * 100 // full_size if full_size else 0
        print(
            "FAT32 trim: "
            f"full={full_size} bytes, send={img_size} bytes, "
            f"saved={saved} bytes ({saved_pct}%)")
        print(
            "FAT32 layout: "
            f"data_start={fat_info['data_start_sector']} sectors, "
            f"spc={fat_info['sectors_per_cluster']}, "
            f"last_cluster={fat_info['last_cluster']}")
    elif args.full_image:
        print("FAT32 trim: disabled by --full-image")
    print(f"Baud: host={args.baud}, board={board_baud}, console={args.console_baud}")

    if args.dry_run:
        return

    ser = serial.Serial(port, args.console_baud, timeout=5)
    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Phase 1: shell handshake at console baud.
    burn_candidates = []
    for cmd in (args.burn_cmd, "burn", "/bin/burn", "bin/burn"):
        if cmd not in burn_candidates:
            burn_candidates.append(cmd)
    burn_idx = 0
    burn_cmd = burn_candidates[burn_idx]

    print(f"Sending '{burn_cmd}' command to board at {args.console_baud} baud...")
    send_shell_command(ser, burn_cmd)

    print("Waiting for BURN signal...")
    buf = b""
    retries = 0
    while True:
        c = ser.read(1)
        if not c:
            retries += 1
            print(
                "Timeout waiting for BURN, retrying "
                f"'{burn_cmd}' ({retries}); last serial='{printable_serial(buf[-96:])}'")
            send_shell_command(ser, burn_cmd)
            continue
        buf += c
        if buf.endswith(b"BURN\n"):
            break
        failed = f"exec {burn_cmd} failed".encode("ascii")
        if failed in buf:
            if burn_idx + 1 >= len(burn_candidates):
                print(
                    "Board shell could not exec burn program; "
                    f"last serial='{printable_serial(buf[-160:])}'")
                ser.close()
                sys.exit(1)
            burn_idx += 1
            burn_cmd = burn_candidates[burn_idx]
            print(f"Board could not exec previous command, trying '{burn_cmd}'...")
            buf = b""
            send_shell_command(ser, burn_cmd)
            continue
        if len(buf) > 256:
            buf = buf[-256:]

    print("Board ready, sending image info...")

    # Send INFO message: total image size and board-side baud setting.
    send_msg(ser, 0, PKT_INFO, struct.pack('<II', img_size, board_baud))

    seq, type_, payload = recv_msg(ser, context="INFO ACK")
    if type_ != PKT_ACK:
        print(f"Expected ACK, got type 0x{type_:02X}")
        ser.close()
        sys.exit(1)
    reason, ticks = ack_info(payload)
    print(f"INFO ACK seq={seq} reason={reason} ticks={ticks}")
    print("Handshake confirmed success!!!")

    if args.baud != args.console_baud:
        print(f"Switching host to {args.baud} baud (board setting {board_baud})...")
        print(f"  baud step 1: send switch request at {args.console_baud} baud")
        send_msg(ser, 0, PKT_BAUD, struct.pack('<I', board_baud))
        ser.flush()
        try:
            seq, type_, payload = recv_msg(ser, timeout=2, context="BAUD READY")
            if type_ == PKT_ACK:
                reason, ticks = ack_info(payload)
                print(f"  BAUD READY seq={seq} reason={reason} target={ticks}")
            else:
                print(f"  Expected BAUD READY ACK, got {pkt_name(type_)}")
        except (TimeoutError, ValueError) as e:
            print(f"  BAUD READY not received at {args.console_baud}: {e}")

        print(f"  baud step 2: switch host port to {args.baud} baud")
        time.sleep(0.05)
        ser.baudrate = args.baud
        time.sleep(0.10)
        ser.reset_input_buffer()
        baud_synced = False
        for sync_try in range(3):
            print(f"  baud step 3.{sync_try + 1}: send sync at {args.baud} baud")
            send_msg(ser, 0, PKT_BAUD, struct.pack('<I', board_baud))
            try:
                seq, type_, payload = recv_msg(
                    ser, timeout=1,
                    context=f"BAUD ACK attempt={sync_try + 1}")
            except (TimeoutError, ValueError) as e:
                if sync_try == 2:
                    print(f"BAUD ACK not received: {e}")
                continue

            if type_ == PKT_ACK:
                reason, ticks = ack_info(payload)
                print(f"BAUD ACK seq={seq} reason={reason} actual={ticks}")
                baud_synced = True
                break

            print(f"Expected BAUD ACK, got {pkt_name(type_)}")
            break

        if not baud_synced:
            print("Baud switch failed before DATA phase.")
            print(
                "This means the board did not receive the new-baud sync, or the host "
                "could not decode the board's BAUD ACK.")
            print(
                "Reset the board and calibrate this rate first, for example: "
                f"python3 tools/uartbaud.py {port} --baud {args.baud} "
                f"--board-baud {board_baud}")
            print("Use --baud 230400 for the currently verified fast path.")
            ser.close()
            sys.exit(1)

    # Phase 2: data transfer at the negotiated baud.
    data_start = time.monotonic()
    retries_total = 0
    timeout_errors = 0
    crc_errors = 0
    nak_count = 0
    stale_count = 0
    ack_dup_count = 0
    sd_ticks_total = 0
    sd_ticks_max = 0
    for sec in range(nsectors):
        offset = sec * 512
        chunk = img_data[offset:offset + 512]
        if len(chunk) < 512:
            chunk = chunk + b'\x00' * (512 - len(chunk))

        for attempt in range(MAX_RETRY + 1):
            if attempt > 0:
                retries_total += 1
            trace = args.verbose or attempt > 0
            if trace:
                print(f"\n  sec={sec} attempt={attempt + 1}: send DATA len={len(chunk)}")
            send_msg(ser, sec, PKT_DATA, chunk)

            try:
                seq, type_, payload = recv_msg(
                    ser, timeout=10,
                    context=f"sec={sec} attempt={attempt + 1}")
            except TimeoutError as e:
                timeout_errors += 1
                print(f"\n  sec={sec} attempt={attempt + 1} failed: {e}")
                if attempt < MAX_RETRY:
                    continue
                print("  Max retries reached, aborting")
                ser.close()
                sys.exit(1)
            except ValueError as e:
                crc_errors += 1
                print(f"\n  sec={sec} attempt={attempt + 1} failed: {e}")
                if attempt < MAX_RETRY:
                    continue
                print("  Max retries reached, aborting")
                ser.close()
                sys.exit(1)

            if trace:
                if type_ == PKT_ACK:
                    reason, ticks = ack_info(payload)
                    print(f"  sec={sec} attempt={attempt + 1}: recv ACK seq={seq} reason={reason} ticks={ticks}")
                else:
                    print(f"  sec={sec} attempt={attempt + 1}: recv {pkt_name(type_)} seq={seq} plen={len(payload)}")

            if seq != sec:
                stale_count += 1
                print(f"\n  Stale response (seq={seq}, want={sec}, type=0x{type_:02X}), retry {attempt + 1}/{MAX_RETRY}")
                if attempt >= MAX_RETRY:
                    ser.close()
                    sys.exit(1)
                continue

            if type_ == PKT_ACK:
                reason, ticks = ack_info(payload)
                if reason == "DUP":
                    ack_dup_count += 1
                else:
                    sd_ticks_total += ticks
                    sd_ticks_max = max(sd_ticks_max, ticks)
                break  # sector written successfully
            elif type_ == PKT_NAK:
                nak_count += 1
                err_code = payload[0] if payload else 0
                err_name = {ERR_CRC: "CRC", ERR_WRITE: "WRITE"}.get(err_code, f"0x{err_code:02X}")
                print(f"\n  NAK (seq={seq}, error={err_name}), retry {attempt + 1}/{MAX_RETRY}")
                if attempt >= MAX_RETRY:
                    print("  Max retries reached, aborting")
                    ser.close()
                    sys.exit(1)
            else:
                print(f"\n  Unexpected type 0x{type_:02X}, retry {attempt + 1}/{MAX_RETRY}")
                if attempt >= MAX_RETRY:
                    ser.close()
                    sys.exit(1)

        # Progress
        pct = (sec + 1) * 100 // nsectors
        sys.stdout.write(f"\r  Sector {sec + 1}/{nsectors} ({pct}%)")
        sys.stdout.flush()

    print()
    data_elapsed = time.monotonic() - data_start
    kib_per_sec = (img_size / 1024.0) / data_elapsed if data_elapsed > 0 else 0.0

    # Signal completion
    send_msg(ser, nsectors, PKT_DONE)
    try:
        seq, type_, payload = recv_msg(ser, timeout=5)
        if type_ == PKT_ACK:
            reason, ticks = ack_info(payload)
            print(f"Transfer completed successfully! ACK reason={reason} ticks={ticks}")
        else:
            print(f"Unexpected response to DONE: type 0x{type_:02X}")
    except (TimeoutError, ValueError) as e:
        print(f"DONE response error: {e}")

    print(
        "Transfer stats: "
        f"elapsed={data_elapsed:.2f}s, throughput={kib_per_sec:.1f} KiB/s, "
        f"retries={retries_total}, timeouts={timeout_errors}, "
        f"crc_errors={crc_errors}, naks={nak_count}, stale={stale_count}, "
        f"ack_dup={ack_dup_count}, sd_ticks_total={sd_ticks_total}, "
        f"sd_ticks_max={sd_ticks_max}")

    if args.baud != args.console_baud:
        ser.baudrate = args.console_baud
        time.sleep(0.05)

    tail = read_console_tail(ser)
    if tail:
        print("Board console tail:")
        print(printable_serial(tail))

    ser.close()


if __name__ == "__main__":
    main()
