"""Minimal STFS (LIVE/CON/PIRS) lister/extractor. Usage: stfs_extract.py <package> [<out dir>]

Lists the file table; with <out dir>, extracts files. Supports consecutive files and follows the
level-0 hash table chain for fragmented ones.
"""
import os
import struct
import sys

path = sys.argv[1]
out = sys.argv[2] if len(sys.argv) > 2 else None
d = open(path, "rb").read()
assert d[:4] in (b"LIVE", b"PIRS", b"CON "), d[:4]
header_size = struct.unpack_from(">I", d, 0x340)[0]
vd = d[0x379:0x379 + 0x24]
block_sep = vd[2]
ft_count = struct.unpack_from("<H", vd, 3)[0]
ft_block = vd[5] | vd[6] << 8 | vd[7] << 16
shift = 0 if (block_sep & 1) else 1  # female (1 hash table per level) vs male (2)
first = (header_size + 0xFFF) & 0xF000


def data_block(b):
    r = (((b + 0xAA) // 0xAA) << shift) + b
    if b < 0xAA:
        return r
    if b < 0x70E4:
        return r + (((b + 0x70E4) // 0x70E4) << shift)
    return (1 << shift) + r + (((b + 0x70E4) // 0x70E4) << shift)


def block_off(b):
    return first + (data_block(b) << 12)


def level0_hash_off(b):
    # Level-0 hash table backing block b.
    n = (b // 0xAA) * (0xAA + (1 << shift))
    if b // 0xAA:
        n += ((b // 0x70E4) + 1) << shift
    if b // 0x70E4:
        n += 1 << shift
    return first + (n << 12)


def next_block(b):
    e = level0_hash_off(b) + (b % 0xAA) * 0x18
    return int.from_bytes(d[e + 0x15:e + 0x18], "big")


print(f"header 0x{header_size:X} sep {block_sep} shift {shift} file table {ft_count} blocks @ {ft_block}")
entries = []
b = ft_block
for _ in range(ft_count):
    off = block_off(b)
    for i in range(0x1000 // 0x40):
        e = d[off + i * 0x40: off + (i + 1) * 0x40]
        flags = e[0x28]
        nlen = flags & 0x3F
        if not nlen:
            continue
        name = e[:nlen].decode("ascii", "replace")
        start = e[0x2F] | e[0x30] << 8 | e[0x31] << 16
        parent = struct.unpack_from(">h", e, 0x32)[0]
        size = struct.unpack_from(">I", e, 0x34)[0]
        entries.append((name, flags, start, parent, size))
    b = next_block(b)

def full(i):
    name, _, _, parent, _ = entries[i]
    return name if parent == -1 else full(parent) + "/" + name

for i, (name, flags, start, parent, size) in enumerate(entries):
    kind = "dir " if flags & 0x80 else "file"
    print(f"{kind} {full(i):50s} size {size:10d} start {start} {'consec' if flags & 0x40 else 'chain'}")
    if out and not flags & 0x80:
        dst = os.path.join(out, *full(i).split("/"))
        os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
        buf = bytearray()
        blk = start
        while len(buf) < size:
            o = block_off(blk)
            buf += d[o:o + 0x1000]
            blk = blk + 1 if flags & 0x40 else next_block(blk)
        open(dst, "wb").write(bytes(buf[:size]))
