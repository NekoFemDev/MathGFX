#!/usr/bin/env python3
"""Structural check for MathGFX H.264/MP4 output (no external ffmpeg).

usage: check_mp4.py file.mp4 expected_frames width height
"""
import struct
import sys


def u32(data, off):
    return struct.unpack_from(">I", data, off)[0]


def u16(data, off):
    return struct.unpack_from(">H", data, off)[0]


def boxes(data, start, end):
    pos = start
    while pos + 8 <= end:
        size = u32(data, pos)
        typ = data[pos + 4:pos + 8]
        if size < 8 or pos + size > end:
            raise SystemExit(f"bad box {typ!r} at {pos} size {size}")
        yield pos, size, typ
        pos += size


def find_box(data, start, end, typ):
    for pos, size, t in boxes(data, start, end):
        if t == typ:
            return pos, size
    return None, None


def check(path, expect_frames, expect_w, expect_h):
    data = open(path, "rb").read()
    top = {t: (p, s) for p, s, t in boxes(data, 0, len(data))}
    if b"ftyp" not in top or b"moov" not in top or b"mdat" not in top:
        raise SystemExit(f"{path}: missing ftyp/moov/mdat")

    fp, fs = top[b"ftyp"]
    if data[fp + 8:fp + 12] != b"isom":
        raise SystemExit(f"{path}: unexpected major brand")

    mp, ms = top[b"mdat"]
    mdat_start = mp + 8
    mdat_end = mp + ms
    if mdat_end > len(data):
        raise SystemExit(f"{path}: truncated mdat")

    mo, ms = top[b"moov"]

    mvhd, _ = find_box(data, mo + 8, mo + ms, b"mvhd")
    if mvhd is None:
        raise SystemExit(f"{path}: missing mvhd")
    movie_timescale = u32(data, mvhd + 20)
    movie_duration = u32(data, mvhd + 24)

    trak, _ = find_box(data, mo + 8, mo + ms, b"trak")
    if trak is None:
        raise SystemExit(f"{path}: missing trak")
    tkhd, _ = find_box(data, trak + 8, trak + ms, b"tkhd")
    if tkhd is None:
        raise SystemExit(f"{path}: missing tkhd")

    mdia, _ = find_box(data, trak + 8, trak + ms, b"mdia")
    if mdia is None:
        raise SystemExit(f"{path}: missing mdia")
    mdhd, _ = find_box(data, mdia + 8, mdia + ms, b"mdhd")
    if mdhd is None:
        raise SystemExit(f"{path}: missing mdhd")
    media_timescale = u32(data, mdhd + 20)
    media_duration = u32(data, mdhd + 24)

    hdlr, _ = find_box(data, mdia + 8, mdia + ms, b"hdlr")
    if hdlr is None or data[hdlr + 16:hdlr + 20] != b"vide":
        raise SystemExit(f"{path}: missing video hdlr")

    minf, _ = find_box(data, mdia + 8, mdia + ms, b"minf")
    if minf is None:
        raise SystemExit(f"{path}: missing minf")
    stbl, _ = find_box(data, minf + 8, minf + ms, b"stbl")
    if stbl is None:
        raise SystemExit(f"{path}: missing stbl")

    stsd, _ = find_box(data, stbl + 8, stbl + ms, b"stsd")
    if stsd is None:
        raise SystemExit(f"{path}: missing stsd")
    if u32(data, stsd + 12) != 1:
        raise SystemExit(f"{path}: stsd entry count is not 1")
    entry_start = stsd + 16
    avc1, avc1_size = find_box(data, entry_start, stsd + u32(data, stsd), b"avc1")
    if avc1 is None:
        raise SystemExit(f"{path}: missing avc1")
    if u16(data, avc1 + 8 + 6) != 1:
        raise SystemExit(f"{path}: bad data_reference_index")
    aw = u16(data, avc1 + 8 + 24)
    ah = u16(data, avc1 + 8 + 26)
    if (aw, ah) != (expect_w, expect_h):
        raise SystemExit(f"{path}: avc1 size {aw}x{ah}")

    avcc, _ = find_box(data, avc1 + 86, avc1 + avc1_size, b"avcC")
    if avcc is None:
        raise SystemExit(f"{path}: missing avcC")
    if data[avcc + 8] != 1 or (data[avcc + 12] & 3) != 3:
        raise SystemExit(f"{path}: bad avcC header")
    sps_len = u16(data, avcc + 14)
    sps = data[avcc + 16:avcc + 16 + sps_len]
    if len(sps) != sps_len or not (sps and (sps[0] & 0x1f) == 7):
        raise SystemExit(f"{path}: bad SPS")
    p = avcc + 16 + sps_len
    if data[p] != 1:
        raise SystemExit(f"{path}: bad PPS count")
    pps_len = u16(data, p + 1)
    pps = data[p + 3:p + 3 + pps_len]
    if len(pps) != pps_len or not (pps and (pps[0] & 0x1f) == 8):
        raise SystemExit(f"{path}: bad PPS")

    stts, _ = find_box(data, stbl + 8, stbl + ms, b"stts")
    if stts is None or u32(data, stts + 12) != 1:
        raise SystemExit(f"{path}: bad stts")
    sample_count = u32(data, stts + 16)
    sample_delta = u32(data, stts + 20)
    if sample_count != expect_frames or sample_delta != 1:
        raise SystemExit(f"{path}: stts {sample_count}/{sample_delta}")

    stsc, _ = find_box(data, stbl + 8, stbl + ms, b"stsc")
    if stsc is None or u32(data, stsc + 12) != 1:
        raise SystemExit(f"{path}: bad stsc")
    if u32(data, stsc + 16) != 1 or u32(data, stsc + 20) != expect_frames:
        raise SystemExit(f"{path}: stsc chunk layout")

    stsz, _ = find_box(data, stbl + 8, stbl + ms, b"stsz")
    if stsz is None:
        raise SystemExit(f"{path}: missing stsz")
    sample_size = u32(data, stsz + 12)
    count = u32(data, stsz + 16)
    if sample_size != 0 or count != expect_frames:
        raise SystemExit(f"{path}: stsz {sample_size}/{count}")
    sizes = [u32(data, stsz + 20 + i * 4) for i in range(count)]

    stco, _ = find_box(data, stbl + 8, stbl + ms, b"stco")
    if stco is None or u32(data, stco + 12) != 1:
        raise SystemExit(f"{path}: bad stco")
    chunk_offset = u32(data, stco + 16)
    if chunk_offset != mdat_start:
        raise SystemExit(f"{path}: stco {chunk_offset} != mdat {mdat_start}")

    stss, _ = find_box(data, stbl + 8, stbl + ms, b"stss")
    keys = []
    if stss is not None:
        nkey = u32(data, stss + 12)
        keys = [u32(data, stss + 16 + i * 4) for i in range(nkey)]
        if not keys or keys[0] != 1:
            raise SystemExit(f"{path}: first sample is not a keyframe")

    if media_timescale <= 0:
        raise SystemExit(f"{path}: bad media timescale")
    if media_duration != expect_frames:
        raise SystemExit(f"{path}: media duration {media_duration}")
    if movie_timescale != 1000:
        raise SystemExit(f"{path}: movie timescale {movie_timescale}")
    expect_ms = int(expect_frames * 1000 / media_timescale)
    if abs(movie_duration - expect_ms) > 1:
        raise SystemExit(f"{path}: movie duration {movie_duration} != {expect_ms}")

    off = chunk_offset
    total = 0
    for i, size in enumerate(sizes):
        end = off + size
        if end > mdat_end:
            raise SystemExit(f"{path}: sample {i} outside mdat")
        p = off
        first_type = None
        while p + 4 <= end:
            n = u32(data, p)
            p += 4
            if n == 0 or p + n > end:
                raise SystemExit(f"{path}: bad NAL length in sample {i}")
            t = data[p] & 0x1f
            first_type = first_type or t
            p += n
        if p != end:
            raise SystemExit(f"{path}: sample {i} has trailing bytes")
        total += size
        off = end
    if total != mdat_end - mdat_start:
        raise SystemExit(f"{path}: sample sizes sum {total} != mdat data")

    print(f"ok: {path} {count} frames {expect_w}x{expect_h} H.264 "
          f"@ {media_timescale} fps, keyframes={len(keys)}")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    check(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]))
