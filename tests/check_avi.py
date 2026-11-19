#!/usr/bin/env python3
"""Validate MathGFX MJPEG AVI: RIFF/AVI, hdrl, MJPG, movi chunks, idx1, JPEGs.

usage: check_avi.py file.avi expected_frames width height
"""
import struct
import sys


def u32(data, off):
    return struct.unpack_from("<I", data, off)[0]


def find_tag(data, tag, start=0):
    pos = data.find(tag, start)
    if pos < 0:
        raise SystemExit(f"missing {tag!r}")
    return pos


def jpeg_dimensions(jpg):
    """Parse the SOF marker of a baseline/progressive JPEG (no PIL)."""
    if len(jpg) < 4 or jpg[:2] != b"\xff\xd8":
        return None
    i = 2
    while i + 4 <= len(jpg):
        if jpg[i] != 0xFF:
            i += 1
            continue
        marker = jpg[i + 1]
        i += 2
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if marker == 0xDA:  # start of scan, SOF already passed
            break
        if i + 2 > len(jpg):
            break
        length = struct.unpack_from(">H", jpg, i)[0]
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6,
                      0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            if i + 7 > len(jpg):
                break
            h = struct.unpack_from(">H", jpg, i + 3)[0]
            w = struct.unpack_from(">H", jpg, i + 5)[0]
            return w, h
        i += length
    return None


def check(path, expect_frames, expect_w, expect_h):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"AVI ":
        raise SystemExit(f"{path}: not a RIFF/AVI file")
    if u32(data, 4) != len(data) - 8:
        raise SystemExit(f"{path}: bad RIFF size")

    avih = find_tag(data, b"avih")
    if u32(data, avih + 4) != 56:
        raise SystemExit(f"{path}: bad avih size")
    total = u32(data, avih + 8 + 16)
    width = u32(data, avih + 8 + 32)
    height = u32(data, avih + 8 + 36)
    if (width, height) != (expect_w, expect_h):
        raise SystemExit(f"{path}: avih size {width}x{height}")

    strh = find_tag(data, b"strh")
    sdata = strh + 8
    if data[sdata:sdata + 4] != b"vids":
        raise SystemExit(f"{path}: strh is not video")
    if data[sdata + 4:sdata + 8] != b"MJPG":
        raise SystemExit(f"{path}: handler is not MJPG")
    scale = u32(data, sdata + 20)
    rate = u32(data, sdata + 24)
    length = u32(data, sdata + 32)
    fps = rate / scale if scale else 0

    strf = find_tag(data, b"strf")
    fdata = strf + 8
    if u32(data, fdata) != 40:
        raise SystemExit(f"{path}: bad strf size")
    fw = u32(data, fdata + 4)
    fh = struct.unpack_from("<i", data, fdata + 8)[0]
    if (fw, abs(fh)) != (expect_w, expect_h):
        raise SystemExit(f"{path}: strf size {fw}x{fh}")
    if data[fdata + 16:fdata + 20] != b"MJPG":
        raise SystemExit(f"{path}: strf compression is not MJPG")

    movi = find_tag(data, b"movi")
    pos = movi + 4
    frames = []
    while pos + 8 <= len(data):
        tag = data[pos:pos + 4]
        if tag == b"idx1":
            break
        size = u32(data, pos + 4)
        if tag == b"00dc":
            jpg = data[pos + 8:pos + 8 + size]
            frames.append((pos, size, jpg))
            if not jpg.startswith(b"\xff\xd8") or not jpg.endswith(b"\xff\xd9"):
                raise SystemExit(f"{path}: frame {len(frames)-1} is not JPEG")
            jsize = jpeg_dimensions(jpg)
            if jsize != (expect_w, expect_h):
                raise SystemExit(f"{path}: frame {len(frames)-1} is {jsize}")
        pos += 8 + size + (size & 1)

    if total != expect_frames or length != expect_frames:
        raise SystemExit(f"{path}: header frames {total}/{length}, expected {expect_frames}")
    if len(frames) != expect_frames:
        raise SystemExit(f"{path}: movi has {len(frames)} frames, expected {expect_frames}")

    idx = find_tag(data, b"idx1", pos)
    idx_size = u32(data, idx + 4)
    if idx_size != expect_frames * 16:
        raise SystemExit(f"{path}: bad idx1 size")
    for i in range(expect_frames):
        o = idx + 8 + i * 16
        if data[o:o + 4] != b"00dc":
            raise SystemExit(f"{path}: idx entry {i} is not 00dc")
        off = u32(data, o + 8)
        size = u32(data, o + 12)
        chunk = movi + off
        if data[chunk:chunk + 4] != b"00dc":
            raise SystemExit(f"{path}: idx offset {i} does not point to a chunk")
        if u32(data, chunk + 4) != size:
            raise SystemExit(f"{path}: idx size {i} mismatch")

    print(f"ok: {path} {len(frames)} frames {expect_w}x{expect_h} @ {fps:g} fps")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    check(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
