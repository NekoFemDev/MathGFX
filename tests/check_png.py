#!/usr/bin/env python3
"""Validate the shape of an 8-bit RGB PNG written by MathGFX."""
import struct
import sys
import zlib


def check(path, want_w=None, want_h=None):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: bad PNG signature")

    pos = 8
    idat = bytearray()
    w = h = None
    while pos < len(data):
        if pos + 8 > len(data):
            raise SystemExit(f"{path}: truncated chunk header")
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        pos += 8
        chunk = data[pos:pos + ln]
        if len(chunk) != ln:
            raise SystemExit(f"{path}: truncated chunk")
        pos += ln
        crc = data[pos:pos + 4]
        if len(crc) != 4:
            raise SystemExit(f"{path}: missing CRC")
        pos += 4
        if typ == b"IHDR":
            w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            if (bd, ct, comp, filt, inter) != (8, 2, 0, 0, 0):
                raise SystemExit(f"{path}: unexpected IHDR fields")
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break

    if w is None:
        raise SystemExit(f"{path}: no IHDR")
    if want_w is not None and w != want_w:
        raise SystemExit(f"{path}: width {w} != {want_w}")
    if want_h is not None and h != want_h:
        raise SystemExit(f"{path}: height {h} != {want_h}")

    raw = zlib.decompress(bytes(idat))
    expect = h * (1 + w * 3)
    if len(raw) != expect:
        raise SystemExit(f"{path}: decompressed {len(raw)} bytes, expected {expect}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: check_png.py file.png [w h]")
    w = int(sys.argv[2]) if len(sys.argv) > 2 else None
    h = int(sys.argv[3]) if len(sys.argv) > 3 else None
    check(sys.argv[1], w, h)
    print(f"ok: {sys.argv[1]} ({w or '?'}x{h or '?'})")
