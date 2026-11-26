#!/usr/bin/env python3
"""Get the 3DS Activity Log (PlayHistory.dat, Pedometer.dat) out of a NAND backup - read only.

    python3 extract_playhistory.py nand <sysnand.bin> <boot9.bin> <out-folder>
    python3 extract_playhistory.py save <ptm-save-file> <out-folder>

`nand`: a GodMode9 SysNAND backup (it carries the console's essential data, OTP included,
so boot9.bin is the only other thing needed). Needs `pip install pyctr pyfatfs`.
`save`: the PTM system save itself (CTRNAND data/<id0>/sysdata/00010022/00000000), for
example copied with GodMode9.

Writes to <out-folder>:
  PlayHistory.dat, Pedometer.dat  the files as they are in the save
  playhistory.bin                 the play history in archive format (oldest entry first,
                                  see docs/activity-log-archive.md)
Nothing is ever written to the backup.

The save container (DISA, DIFI/IVFC/DPFS, SAVE file system) is read as described on
3dbrew ("DISA and DIFF", "Savegames"); the code is written for this one job.
"""
import hashlib
import os
import struct
import sys

PTM_SAVE = "sysdata/00010022/00000000"
RING = 0x11D28          # entries PlayHistory.dat can hold
ENTRY = 12


def fail(msg):
    sys.exit("error: " + msg)


# ---------------------------------------------------------------- DISA container

def dpfs_select(selector, pair, block):
    """the active copy of a DPFS level: bit i of the selector picks copy 0 or 1 of block i"""
    out = bytearray()
    size = len(pair[0])
    pos = 0
    word = 0
    while pos < size:
        bits, = struct.unpack_from("<I", selector, word * 4)
        for i in range(32):
            if pos >= size:
                break
            n = min(block, size - pos)
            out += pair[(bits >> (31 - i)) & 1][pos:pos + n]
            pos += n
        word += 1
    return bytes(out)


def unwrap_partition(desc, part):
    """DIFI descriptor + partition -> the inner image (IVFC level 4), hash-checked"""
    (magic, ver, ivfc_off, ivfc_size, dpfs_off, dpfs_size, hash_off, hash_size,
     ext_l4, l1_sel, ext_l4_off) = struct.unpack_from("<IIQQQQQQBB2xQ", desc, 0)
    if magic != 0x49464944 or ver != 0x10000:
        fail("not a DIFI descriptor")
    iv = struct.unpack_from("<IIQ" + "QQI4x" * 4, desc, ivfc_off)
    if iv[0] != 0x43465649:
        fail("not an IVFC descriptor")
    levels = [(iv[3 + 3 * i], iv[4 + 3 * i], 1 << iv[5 + 3 * i]) for i in range(4)]
    dp = struct.unpack_from("<II" + "QQI4x" * 3, desc, dpfs_off)
    if dp[0] != 0x53465044:
        fail("not a DPFS descriptor")
    d = [(dp[2 + 3 * i], dp[3 + 3 * i], 1 << dp[4 + 3 * i]) for i in range(3)]
    master = desc[hash_off:hash_off + hash_size]

    def pair(off, size):
        return (part[off:off + size], part[off + size:off + 2 * size])

    l1 = pair(d[0][0], d[0][1])[l1_sel]
    l2 = dpfs_select(l1, pair(d[1][0], d[1][1]), d[1][2])
    active = dpfs_select(l2, pair(d[2][0], d[2][1]), d[2][2])

    def level(i):
        off, size, _ = levels[i]
        if i == 3 and ext_l4:
            return part[ext_l4_off:ext_l4_off + size]
        return active[off:off + size]

    # each level holds the SHA-256 of the blocks of the next one. Blocks the system never
    # wrote do not match; they are filled with 0xDD (as 3ds-save-tool does) and counted.
    hashes = master
    bad = 0
    for i in range(4):
        data = bytearray(level(i))
        block = levels[i][2]
        for b in range(0, len(data), block):
            chunk = bytes(data[b:b + block])
            want = hashes[b // block * 32:b // block * 32 + 32]
            if len(want) < 32:
                break
            if hashlib.sha256(chunk + b"\0" * (block - len(chunk))).digest() != want:
                data[b:b + block] = b"\xDD" * len(chunk)
                bad += 1
        hashes = bytes(data)
    if bad:
        print("note: %d blocks without a valid hash (never written, or damaged)" % bad)
    return hashes


def read_disa(raw):
    """DISA file -> (SAVE image, data image or None)"""
    (magic, ver, count, sec_off, pri_off, table_size, a_desc_off, a_desc_size, b_desc_off,
     b_desc_size, a_off, a_size, b_off, b_size, active, table_hash) = struct.unpack_from(
        "<III4xQQQQQQQQQQQB3x32s", raw, 0x100)
    if magic != 0x41534944 or ver != 0x40000:
        fail("not a DISA save")
    table_off = pri_off if active == 0 else sec_off
    table = raw[table_off:table_off + table_size]
    if hashlib.sha256(table).digest() != table_hash:
        fail("partition table hash does not match")
    save = unwrap_partition(table[a_desc_off:a_desc_off + a_desc_size], raw[a_off:a_off + a_size])
    data = None
    if count == 2:
        data = unwrap_partition(table[b_desc_off:b_desc_off + b_desc_size], raw[b_off:b_off + b_size])
    return save, data


# ---------------------------------------------------------------- SAVE file system

def list_root(save, data_given):
    magic, ver, fs_off = struct.unpack_from("<IIQ", save, 0)
    (_, block, _, _, _, _, fat_off, fat_size, data_off, data_count) = struct.unpack_from(
        "<IIQI4xQI4xQI4xQI4x", save, fs_off)
    data = data_given if data_given is not None else save[data_off:data_off + data_count * block]
    fat = [struct.unpack_from("<II", save, fat_off + i * 8) for i in range(fat_size + 1)]

    def chain(start):
        cur = start + 1
        while cur:
            u, v = fat[cur]
            last = fat[cur + 1][1] & 0x7FFFFFFF if v & 0x80000000 else cur
            for i in range(cur, last + 1):
                yield i - 1
            cur = v & 0x7FFFFFFF

    def read(start, size):
        out = bytearray()
        for b in chain(start):
            if len(out) >= size:
                break
            out += data[b * block:(b + 1) * block]
        return bytes(out[:size])

    if data_given is None:
        _, _, _, f_index, f_count, _ = struct.unpack_from("<III4xIII4x", save, fs_off + 0x48)
        files = read(f_index, f_count * block)
    else:
        _, _, f_off, _ = struct.unpack_from("<QI4xQI4x", save, fs_off + 0x48)
        files = save[f_off:]
    count, = struct.unpack_from("<I", files, 0)   # entry 0: how many entries are used
    out = {}
    for i in range(1, count):
        parent, name, nxt, start, size, _, _ = struct.unpack_from("<I16sI4xIQII", files, i * 0x30)
        name = name.split(b"\0")[0].decode("ascii", "replace")
        if parent == 1 and name:
            out[name] = read(start, size) if size else b""
    return out


# ---------------------------------------------------------------- NAND backup

def ptm_save_from_nand(nand_path, boot9_path):
    try:
        from pyctr.crypto.engine import CryptoEngine
        from pyctr.type.nand import NAND
        from pyfatfs.PyFat import PyFat
        from pyfatfs.PyFatFS import PyFatFS
        from fs.base import FS
    except ImportError:
        fail("for a NAND backup: pip install pyctr pyfatfs")
    nand = NAND(nand_path, crypto=CryptoEngine(boot9=boot9_path))
    fp = nand.open_ctr_partition()

    class Ctr(PyFatFS):   # PyFatFS on an open (decrypting) file instead of a path
        def __init__(self, f):
            FS.__init__(self)
            self.fs = PyFat(encoding="ascii", offset=0)
            self.fs.set_fp(f)
            self.fs.parse_header()
            self.fs._parse_fat()
            self.preserve_case = False
            self.lazy_load = True
            self.utc = False
            self.encoding = "ascii"

    ctr = Ctr(fp)
    for id0 in ctr.listdir("/data"):
        p = "/data/%s/%s" % (id0, PTM_SAVE)
        if ctr.exists(p):
            return ctr.readbytes(p)
    fail("no PTM save in this NAND")


# ---------------------------------------------------------------- output

def linear_history(dat):
    """PlayHistory.dat -> entries oldest first (the file is a ring: start index, count)"""
    start, count = struct.unpack_from("<II", dat, 0)
    out = bytearray()
    for i in range(count):
        pos = 8 + ((start + i) % RING) * ENTRY
        out += dat[pos:pos + ENTRY]
    return bytes(out), start, count


def main(argv):
    if len(argv) == 5 and argv[1] == "nand":
        raw, out = ptm_save_from_nand(argv[2], argv[3]), argv[4]
    elif len(argv) == 4 and argv[1] == "save":
        raw, out = open(argv[2], "rb").read(), argv[3]
    else:
        sys.exit(__doc__)
    os.makedirs(out, exist_ok=True)
    save, data = read_disa(raw)
    files = list_root(save, data)
    print("files in the PTM save:", ", ".join("%s (%d bytes)" % (k, len(v)) for k, v in files.items()))
    for name in ("PlayHistory.dat", "Pedometer.dat"):
        if name in files:
            open(os.path.join(out, name), "wb").write(files[name])
    if "PlayHistory.dat" not in files:
        fail("no PlayHistory.dat")
    hist, start, count = linear_history(files["PlayHistory.dat"])
    from archive_format import write_archive
    write_archive(os.path.join(out, "playhistory.bin"), hist)
    first = struct.unpack_from("<I", hist, 8)[0] >> 4
    last = struct.unpack_from("<I", hist, len(hist) - 4)[0] >> 4
    print("play history: %d entries (ring start %d), minutes %d .. %d since 2000" % (count, start, first, last))


if __name__ == "__main__":
    main(sys.argv)
