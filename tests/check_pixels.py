#!/usr/bin/env python3
"""Check individual RGB pixels in an 8-bit PNG written by MathGFX.

usage: check_pixels.py file.png x y r g b [x y r g b ...]
"""
import struct
import sys
import zlib


def paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def load_rgb(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: bad PNG signature")

    pos = 8
    idat = bytearray()
    w = h = None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        pos += 8
        chunk = data[pos:pos + ln]
        pos += ln + 4
        if typ == b"IHDR":
            w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            if (bd, ct, comp, filt, inter) != (8, 2, 0, 0, 0):
                raise SystemExit(f"{path}: unexpected IHDR fields")
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break

    raw = zlib.decompress(bytes(idat))
    stride = w * 3
    if len(raw) != h * (stride + 1):
        raise SystemExit(f"{path}: bad decompressed size")

    out = bytearray(h * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        row = bytearray(raw[pos:pos + stride])
        pos += stride
        for i in range(stride):
            a = row[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 0:
                v = row[i]
            elif f == 1:
                v = (row[i] + a) & 255
            elif f == 2:
                v = (row[i] + b) & 255
            elif f == 3:
                v = (row[i] + ((a + b) // 2)) & 255
            elif f == 4:
                v = (row[i] + paeth(a, b, c)) & 255
            else:
                raise SystemExit(f"{path}: unknown filter {f}")
            row[i] = v
        out[y * stride:(y + 1) * stride] = row
        prev = row

    return w, h, out


def main():
    if len(sys.argv) < 7 or (len(sys.argv) - 2) % 5:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    w, h, rgb = load_rgb(path)
    args = sys.argv[2:]
    for i in range(0, len(args), 5):
        x, y, r, g, b = map(int, args[i:i + 5])
        if not (0 <= x < w and 0 <= y < h):
            raise SystemExit(f"{path}: pixel ({x},{y}) outside {w}x{h}")
        off = (y * w + x) * 3
        got = tuple(rgb[off:off + 3])
        want = (r, g, b)
        if got != want:
            raise SystemExit(f"{path}: pixel ({x},{y}) = {got}, want {want}")
    print(f"ok: {path} pixel checks")


if __name__ == "__main__":
    main()
