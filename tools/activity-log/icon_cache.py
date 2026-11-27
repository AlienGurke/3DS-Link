#!/usr/bin/env python3
"""Names and pictures from the HOME Menu's icon cache (idb.dat + idbt.dat, as 3DS Link saves
them in sdmc:/3ds/3DS-Link/activity/). Format: docs/activity-log-archive.md.

    python3 icon_cache.py <idbt.dat> <idb.dat> <out-folder> [language]

Writes <out-folder>/titles.csv (title id;name;publisher) and <out-folder>/<title id>.png
(48x48). language: 0 Japanese, 1 English, 2 French, 3 German (default), 4 Italian,
5 Spanish, ... - falls back to English, then any.
"""
import os
import struct
import sys
import zlib

SLOTS = 512
RECORD = 0x36A0           # 0x20 settings, 0x2000 names (16 x 0x200), 0x480 icon 24x24, 0x1200 icon 48x48
NAMES = 0x20
ICON48 = 0x20 + 0x2000 + 0x480


def entries(idbt, idb):
    """(slot, title id, time, name structs) for every used slot"""
    for i in range(SLOTS):
        tid, t, flags = struct.unpack_from("<QII", idbt, 8 + i * 16)
        rec = idb[i * RECORD:(i + 1) * RECORD]
        if tid in (0, 0xFFFFFFFFFFFFFFFF) or len(rec) < RECORD:
            continue
        yield i, tid, t, rec


def name(rec, lang):
    def get(l, off, size):
        o = NAMES + l * 0x200 + off
        return rec[o:o + size].decode("utf-16-le", "replace").split("\0")[0].strip()
    for l in (lang, 1) + tuple(range(16)):
        short = get(l, 0, 0x80)
        if short:
            return short, get(l, 0x180, 0x80)
    return "", ""


def icon_rgb(rec):
    """48x48 RGB565, in 8x8 tiles whose pixels are in Z order -> rows of RGB bytes"""
    px = [[(0, 0, 0)] * 48 for _ in range(48)]
    data = rec[ICON48:ICON48 + 0x1200]
    i = 0
    for ty in range(0, 48, 8):
        for tx in range(0, 48, 8):
            for k in range(64):
                x = (k & 1) | (k >> 1 & 2) | (k >> 2 & 4)
                y = (k >> 1 & 1) | (k >> 2 & 2) | (k >> 3 & 4)
                v = data[i] | data[i + 1] << 8
                i += 2
                px[ty + y][tx + x] = ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
    return px


def png(path, px):
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in px)
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 48, 48, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    if len(argv) not in (4, 5):
        sys.exit(__doc__)
    idbt, idb, out = open(argv[1], "rb").read(), open(argv[2], "rb").read(), argv[3]
    lang = int(argv[4]) if len(argv) == 5 else 3
    os.makedirs(out, exist_ok=True)
    best = {}   # a title can have more than one slot: the newest counts
    for slot, tid, t, rec in entries(idbt, idb):
        if tid not in best or t >= best[tid][0]:
            best[tid] = (t, rec)
    with open(os.path.join(out, "titles.csv"), "w", encoding="utf-8") as f:
        for tid in sorted(best):
            short, publisher = name(best[tid][1], lang)
            f.write("%016X;%s;%s\n" % (tid, short, publisher))
            png(os.path.join(out, "%016X.png" % tid), icon_rgb(best[tid][1]))
    print("%d titles" % len(best))


if __name__ == "__main__":
    main(sys.argv)
