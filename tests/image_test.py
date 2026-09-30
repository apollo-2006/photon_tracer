#!/usr/bin/env python3
"""Tests the PNG decoder in include/image_io.hpp against PNGs made here with
Python's zlib: every supported color type and bit depth, all five row filters,
and stored, fixed and dynamic deflate blocks, each split over two IDAT chunks.
Needs tests/decode_image, which make test builds."""
import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DECODER = os.path.join(ROOT, "tests", "decode_image")
files, expected = {}, {}
random.seed(1)

def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
def png(name, w, h, ctype, depth, pixels, palette=None, level=6, strategy=None):
    """Encodes pixels (rows of raw sample bytes) as a PNG, cycling the row filters,
    and records the RGB bytes a decoder should produce."""
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]; bpp = ch * depth // 8; stride = bpp * w
    raw = b''; prev = bytes(stride)
    for y in range(h):
        row = pixels[y]; f = y % 5; filt = bytearray(stride)
        for x in range(stride):
            a = row[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            pred = [0, a, b, (a + b) // 2, paeth(a, b, c)][f]
            filt[x] = (row[x] - pred) & 255
        raw += bytes([f]) + bytes(filt); prev = row
    comp = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy if strategy is not None else zlib.Z_DEFAULT_STRATEGY)
    data = comp.compress(raw) + comp.flush()
    body = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, depth, ctype, 0, 0, 0))
    if palette: body += chunk(b'PLTE', palette)
    # Split IDAT in two, as encoders may.
    body += chunk(b'IDAT', data[:len(data)//2]) + chunk(b'IDAT', data[len(data)//2:]) + chunk(b'IEND', b'')
    files[name] = body
    rgb = bytearray()
    for y in range(h):
        for x in range(w):
            s = pixels[y][x * bpp: (x + 1) * bpp]; hi = s[::depth // 8]
            if ctype == 3: rgb += palette[3 * hi[0]: 3 * hi[0] + 3]
            elif ch >= 3: rgb += bytes(hi[:3])
            else: rgb += bytes([hi[0]] * 3)
    expected[name] = (w, h, bytes(rgb))
W, H = 37, 23  # Odd sizes, so rows do not line up with anything
def rnd(n, smooth):
    return [bytes((x * 3 + y * 5 + random.randint(0, 20)) & 255 if smooth else random.randint(0, 255) for x in range(n)) for y in range(H)]
png('rgb8', W, H, 2, 8, rnd(3 * W, True))
png('rgba8', W, H, 6, 8, rnd(4 * W, True))
png('gray8', W, H, 0, 8, rnd(W, True))
png('graya8', W, H, 4, 8, rnd(2 * W, False))
png('rgb16', W, H, 2, 16, rnd(6 * W, False))
pal = bytes(random.randint(0, 255) for _ in range(3 * 16))
png('pal8', W, H, 3, 8, [bytes(random.randint(0, 15) for _ in range(W)) for _ in range(H)], palette=pal)
png('stored', W, H, 2, 8, rnd(3 * W, False), level=0)
png('fixed', W, H, 2, 8, rnd(3 * W, True), strategy=zlib.Z_FIXED)
png('rle', W, H, 2, 8, rnd(3 * W, True), strategy=zlib.Z_RLE)


ok = True
with tempfile.TemporaryDirectory() as tmp:
    for name, data in files.items():
        src, dst = os.path.join(tmp, name + ".png"), os.path.join(tmp, name + ".ppm")
        with open(src, "wb") as f:
            f.write(data)
        run = subprocess.run([DECODER, src, dst], capture_output=True, text=True)
        w, h, rgb = expected[name]
        good = False
        if run.returncode == 0:
            with open(dst, "rb") as f:
                _, size, _, pixels = f.read().split(b"\n", 3)  # P6, size, 255, bytes
            good = size == b"%d %d" % (w, h) and pixels == rgb
        print(f"{'ok  ' if good else 'FAIL'} png {name}" + ("" if run.returncode == 0 else ": " + run.stderr.strip()))
        ok &= good
sys.exit(0 if ok else 1)
