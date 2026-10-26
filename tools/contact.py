#!/usr/bin/env python3
"""contact.py <out.png> <cols> <frame.ppm>...: downscaled (1/2) grid of frame dumps, no dependencies."""
import struct, sys, zlib
out, cols, files = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
imgs = []
for f in files:
    d = open(f, 'rb').read().split(b'\n', 3)
    w, h = map(int, d[1].split())
    px = d[3]
    sw, sh = w // 2, h // 2
    small = bytearray(sw * sh * 3)
    for y in range(sh):
        row = px[(2 * y) * w * 3:(2 * y + 1) * w * 3]
        for x in range(sw):
            small[(y * sw + x) * 3:(y * sw + x) * 3 + 3] = row[2 * x * 3:2 * x * 3 + 3]
    imgs.append((sw, sh, small))
sw, sh = imgs[0][0], imgs[0][1]
rows = (len(imgs) + cols - 1) // cols
W, H = cols * (sw + 4), rows * (sh + 4)
canvas = bytearray(W * H * 3)
for i, (w, h, im) in enumerate(imgs):
    ox, oy = (i % cols) * (sw + 4), (i // cols) * (sh + 4)
    for y in range(h):
        canvas[((oy + y) * W + ox) * 3:((oy + y) * W + ox + w) * 3] = im[y * w * 3:(y + 1) * w * 3]
raw = b''.join(b'\0' + bytes(canvas[y * W * 3:(y + 1) * W * 3]) for y in range(H))
c = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + c(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + c(b'IDAT', zlib.compress(raw)) + c(b'IEND', b''))
