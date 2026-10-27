#!/usr/bin/env python3
"""Convert the screen's PPM snapshot (ALTEREGO_SNAPSHOT) to PNG. No PIL needed.

    scripts/ppm2png.py shot.ppm shot.png
"""
import struct
import sys
import zlib

src, dst = sys.argv[1], sys.argv[2]
data = open(src, "rb").read()
magic, size, depth, px = data.split(b"\n", 3)
assert magic == b"P6" and depth == b"255", "not a binary PPM"
w, h = map(int, size.split())
raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(tag, body):
    return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)


png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
open(dst, "wb").write(png)
