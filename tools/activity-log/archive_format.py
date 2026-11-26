#!/usr/bin/env python3
"""The Activity Log archive (playhistory.bin) - reading, writing, merging.
Format: docs/activity-log-archive.md.

    python3 archive_format.py merge <older.bin> <newer.bin> <out.bin> [<clock.txt> <clock-out.txt>]
    python3 archive_format.py info <archive.bin>

`merge` puts two archives (or two snapshots, e.g. an old NAND backup and today's 3DS)
together: the older one completely, then what the newer one has after the point where
the two overlap. If they do not overlap, a gap entry goes between them.
Entries in front of the newer one move its entry numbers: give the newer one's clock.txt
to get one with the numbers moved to match.
"""
import struct
import sys

MAGIC = b"3DSPLAY\0"
VERSION = 1
ENTRY = 12
HEADER = struct.Struct("<8sII")          # magic, version, entry size
GAP_TID = (0xFFFFFFFF, 0xFFFFFFFE)       # tid high, low of a gap entry
GAP_TYPE = 15
OVERLAP = 32                              # entries that must match to join two snapshots


def write_archive(path, entries):
    with open(path, "wb") as f:
        f.write(HEADER.pack(MAGIC, VERSION, ENTRY))
        f.write(entries)


def read_archive(path):
    raw = open(path, "rb").read()
    magic, ver, size = HEADER.unpack_from(raw, 0)
    if magic != MAGIC or size != ENTRY:
        sys.exit("error: %s is not a playhistory archive" % path)
    body = raw[HEADER.size:]
    return body[:len(body) // ENTRY * ENTRY]   # a cut-off last entry is ignored


def gap_entry(minutes):
    return struct.pack("<III", GAP_TID[0], GAP_TID[1], (minutes << 4) | GAP_TYPE)


def join(older, newer):
    """older + the part of newer after their overlap (both oldest first); also returns by how
    many places the entries of `newer` moved"""
    n = min(OVERLAP, len(older) // ENTRY)
    if n == 0:
        return newer, 0
    tail = older[-n * ENTRY:]
    # the newest place in `newer` where the tail of `older` appears, on an entry boundary
    pos = newer.rfind(tail)
    while pos != -1 and pos % ENTRY:
        pos = newer.rfind(tail, 0, pos + len(tail) - 1)
    if pos != -1:
        cut = pos + len(tail)
        return older + newer[cut:], (len(older) - cut) // ENTRY
    if not newer:
        return older, 0
    first = struct.unpack_from("<I", newer, 8)[0] >> 4
    return older + gap_entry(first) + newer, len(older) // ENTRY + 1


def move_clock(lines, shift):
    """clock.txt lines with their entry numbers moved by `shift`"""
    out = []
    for line in lines:
        f = line.split()
        if len(f) >= 2 and f[0] in ("sprung", "ende", "gestellt") and f[1].lstrip("-").isdigit():
            f[1] = str(int(f[1]) + shift)
            line = " ".join(f) + "\n"
        out.append(line)
    return out


def info(entries):
    n = len(entries) // ENTRY
    games = set()
    gaps = 0
    for i in range(n):
        hi, lo, inf = struct.unpack_from("<III", entries, i * ENTRY)
        if (hi, lo) == GAP_TID and inf & 15 == GAP_TYPE:
            gaps += 1
        elif hi != 0xFFFFFFFF:
            games.add((hi, lo))
    first = struct.unpack_from("<I", entries, 8)[0] >> 4 if n else 0
    last = struct.unpack_from("<I", entries, len(entries) - 4)[0] >> 4 if n else 0
    return n, len(games), gaps, first, last


def minutes_text(m):
    import datetime
    return (datetime.datetime(2000, 1, 1) + datetime.timedelta(minutes=m)).strftime("%Y-%m-%d %H:%M")


def main(argv):
    if len(argv) in (5, 7) and argv[1] == "merge":
        out, shift = join(read_archive(argv[2]), read_archive(argv[3]))
        write_archive(argv[4], out)
        if len(argv) == 7:
            with open(argv[6], "w") as f:
                f.writelines(move_clock(open(argv[5]).readlines(), shift))
        print("entries of the newer one moved by %d" % shift)
        argv = [argv[0], "info", argv[4]]
    if len(argv) == 3 and argv[1] == "info":
        n, g, gaps, first, last = info(read_archive(argv[2]))
        print("%d entries, %d titles, %d gaps, %s .. %s" % (n, g, gaps, minutes_text(first), minutes_text(last)))
        return
    sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
