#!/usr/bin/env python3
"""
Burn a filesystem image to xv6-k210 SD card via UART.

Uses CRC32-protected framed messages (inspired by kflash.py) for
reliable data transfer.  All traffic uses 115200 baud.

Usage:
    python3 tools/burn.py [--baud RATE] <serial_port> [image_file]

Protocol:
  Phase 1:
    1. Host sends "burn\\n"              to shell → starts burn program
    2. Board sends "BURN\\n"             → host knows program is alive
    3. Host sends INFO message            → 4-byte total size LE
    4. Board sends ACK message

  Phase 2 (CRC32-protected messages at 115200 baud):
    5. Host sends DATA messages (one per 512-byte sector)
    6. Board ACKs or NAKs  → host retries on NAK (up to 5 tries)
    7. Host sends DONE message
    8. Board sends ACK

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
PKT_ACK  = 0x81
PKT_NAK  = 0x82

ERR_CRC   = 0x01
ERR_WRITE = 0x02

ACK_OK = 0x00
ACK_DUP = 0x01
ACK_INFO = 0x02
ACK_DONE = 0x03

MAGIC = b'\x55\xAA\x55\xAA'
MAX_RETRY = 5
TRACE_FIRST_SECTORS = 8


def hexdump(data):
    return data.hex(" ").upper() if data else "-"


def pkt_name(type_):
    return {
        PKT_INFO: "INFO",
        PKT_DATA: "DATA",
        PKT_DONE: "DONE",
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


def send_msg(ser, seq, type_, payload=b''):
    """Send a framed message over serial."""
    hdr = MAGIC + struct.pack('<IB', seq, type_) + struct.pack('<H', len(payload))
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    ser.write(hdr + payload + struct.pack('<I', crc))
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
    parser.add_argument("--baud", "-b", type=int, default=115200,
                        help="UART baud rate (default: 115200)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print every DATA/ACK exchange, not just startup and failures")
    args = parser.parse_args()

    port = args.port
    img_path = args.image

    # Read the image file
    with open(img_path, "rb") as f:
        img_data = f.read()

    img_size = len(img_data)
    nsectors = (img_size + 511) // 512
    print(f"Image: {img_path}")
    print(f"Size: {img_size} bytes ({nsectors} sectors)")

    ser = serial.Serial(port, args.baud, timeout=5)
    time.sleep(0.5)
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # Phase 1: Handshake at 115200
    print("Sending 'burn' command to board...")
    ser.write(b"burn\n")

    print("Waiting for BURN signal...")
    buf = b""
    while True:
        c = ser.read(1)
        if not c:
            print("Timeout, retrying 'burn'...")
            ser.write(b"burn\n")
            continue
        buf += c
        if buf.endswith(b"BURN\n"):
            break
        if len(buf) > 32:
            buf = buf[-32:]

    print("Board ready, sending image info...")

    # Send INFO message: total image size.
    send_msg(ser, 0, PKT_INFO, struct.pack('<I', img_size))

    seq, type_, payload = recv_msg(ser, context="INFO ACK")
    if type_ != PKT_ACK:
        print(f"Expected ACK, got type 0x{type_:02X}")
        ser.close()
        sys.exit(1)
    reason, ticks = ack_info(payload)
    print(f"INFO ACK seq={seq} reason={reason} ticks={ticks}")
    print("Handshake confirmed success!!!")

    # Phase 2: Data transfer at 115200 — send sectors, wait for ACK/NAK
    for sec in range(nsectors):
        offset = sec * 512
        chunk = img_data[offset:offset + 512]
        if len(chunk) < 512:
            chunk = chunk + b'\x00' * (512 - len(chunk))

        for attempt in range(MAX_RETRY + 1):
            trace = args.verbose or sec < TRACE_FIRST_SECTORS or attempt > 0
            if trace:
                print(f"\n  sec={sec} attempt={attempt + 1}: send DATA len={len(chunk)}")
            send_msg(ser, sec, PKT_DATA, chunk)

            try:
                seq, type_, payload = recv_msg(
                    ser, timeout=10,
                    context=f"sec={sec} attempt={attempt + 1}")
            except (TimeoutError, ValueError) as e:
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
                print(f"\n  Stale response (seq={seq}, want={sec}, type=0x{type_:02X}), retry {attempt + 1}/{MAX_RETRY}")
                if attempt >= MAX_RETRY:
                    ser.close()
                    sys.exit(1)
                continue

            if type_ == PKT_ACK:
                break  # sector written successfully
            elif type_ == PKT_NAK:
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

    ser.close()


if __name__ == "__main__":
    main()
