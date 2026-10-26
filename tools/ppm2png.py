#!/usr/bin/env python3
"""ppm2png.py <in.ppm> <out.png>: frame dumps -> PNG (no dependencies)"""
import struct, sys, zlib
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split())
px = d[3]
raw = b''.join(b'\0' + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
c = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + c(b'IDAT', zlib.compress(raw)) + c(b'IEND', b''))
