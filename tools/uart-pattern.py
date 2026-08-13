#!/usr/bin/env python3
"""Send a known byte pattern over a serial port for baud-rate measurement.

Use with a logic analyzer on the adapter's TX output pin to measure the
host chip's ACTUAL baud: for pattern 0x55 one square-wave half-period is
exactly one UART bit, so actual_baud = 2 / measured_period.

Usage:
    python3 tools/uartpattern.py [PORT] [BAUD] [PATTERN]
      PORT    serial port (default /dev/ttyUSB0)
      BAUD    nominal baud (default 230400)
      PATTERN pattern byte, decimal or 0x hex (default 0x55)

Example:
    python3 tools/uartpattern.py /dev/ttyUSB0 230400 0x55
    python3 tools/uartpattern.py /dev/ttyUSB0 460800 0x55
"""

import sys

import serial

port = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyUSB0'
baud = int(sys.argv[2]) if len(sys.argv) > 2 else 230400
pat = bytes([int(sys.argv[3], 0)]) if len(sys.argv) > 3 else b'\x55'

ser = serial.Serial(port, baud, timeout=1)
print(f"uartpattern: port={port} nominal_baud={baud} pattern={pat.hex()}")
print("measuring... (Ctrl-C to stop)")
buf = pat * 64
try:
    while True:
        ser.write(buf)
        ser.flush()
except KeyboardInterrupt:
    ser.close()
