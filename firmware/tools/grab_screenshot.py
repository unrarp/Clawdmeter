#!/usr/bin/env python3
"""Capture the LVGL framebuffer over serial and write a PNG — ffmpeg-free.

Mirrors screenshot.sh's serial protocol but decodes RGB565LE → PNG in pure
Python (zlib only), so it works on hosts without ffmpeg. Usage:
    grab_screenshot.py <out.png> [port]
"""

import struct
import sys
import zlib

import serial

out = sys.argv[1]
port_path = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyACM0"

port = serial.Serial(port_path, 115200, timeout=12)
port.reset_input_buffer()
port.write(b"screenshot\n")
port.flush()

w = h = raw_size = None
while True:
    line = port.readline().decode("utf-8", errors="replace").strip()
    if line.startswith("SCREENSHOT_START"):
        _, sw, sh, sz = line.split()
        w, h, raw_size = int(sw), int(sh), int(sz)
        break
    if line == "SCREENSHOT_ERR":
        sys.exit("Device reported screenshot error")

data = b""
while len(data) < raw_size:
    chunk = port.read(min(4096, raw_size - len(data)))
    if not chunk:
        sys.exit(f"Timeout: got {len(data)} of {raw_size}")
    data += chunk
port.close()

# RGB565LE -> RGB888 rows
rows = bytearray()
for y in range(h):
    rows.append(0)  # PNG filter type 0 for this scanline
    base = y * w * 2
    for x in range(w):
        v = data[base + x * 2] | (data[base + x * 2 + 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        rows += bytes(
            (
                (r * 255 + 15) // 31,
                (g * 255 + 31) // 63,
                (b * 255 + 15) // 31,
            )
        )


def chunk(tag, payload):
    return (
        struct.pack(">I", len(payload))
        + tag
        + payload
        + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
    )


png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 9))
png += chunk(b"IEND", b"")
with open(out, "wb") as f:
    f.write(png)
print(f"Saved {out} ({w}x{h}, {len(data)} raw bytes)")
