"""Extract the XDVDFS file tree from an Xbox 360 Games-on-Demand (GOD) package.

Usage: python -I tools/god_extract.py <GOD header file> <output dir>
The <header>.data\\DataNNNN part files must sit next to the header file.

GOD part layout (per Data file): 1 master hash block, then up to 0xCB subparts,
each = 1 sub-hash block + 0xCC data blocks (0x1000 bytes per block).
"""
import os
import struct
import sys

BLOCK = 0x1000
BLOCKS_PER_SUBPART = 0xCC
SUBPARTS_PER_PART = 0xCB
BLOCKS_PER_PART = BLOCKS_PER_SUBPART * SUBPARTS_PER_PART
SECTOR = 0x800


class GodStream:
    def __init__(self, header_path):
        data_dir = header_path + ".data"
        names = sorted(n for n in os.listdir(data_dir) if n.startswith("Data"))
        self.parts = [open(os.path.join(data_dir, n), "rb") for n in names]

    def read(self, offset, size):
        out = bytearray()
        while size > 0:
            blk, within = divmod(offset, BLOCK)
            part, rem = divmod(blk, BLOCKS_PER_PART)
            sub, b = divmod(rem, BLOCKS_PER_SUBPART)
            phys = BLOCK + sub * (BLOCK * (BLOCKS_PER_SUBPART + 1)) + BLOCK + b * BLOCK + within
            n = min(size, BLOCK - within)
            f = self.parts[part]
            f.seek(phys)
            chunk = f.read(n)
            if len(chunk) != n:
                raise IOError(f"short read at logical 0x{offset:X} (part {part})")
            out += chunk
            offset += n
            size -= n
        return bytes(out)


def walk(god, dir_sector, dir_size, out_dir, rel, stats):
    table = god.read(dir_sector * SECTOR, dir_size)
    seen = set()
    stack = [0]
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        left, right, sector, size, attr, nlen = struct.unpack_from("<HHIIBB", table, off)
        if left == 0xFFFF and right == 0xFFFF:
            continue
        name = table[off + 14 : off + 14 + nlen].decode("ascii")
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)
        path = os.path.join(out_dir, name)
        relp = rel + "/" + name
        if attr & 0x10:
            os.makedirs(path, exist_ok=True)
            if size:
                walk(god, sector, size, path, relp, stats)
        else:
            with open(path, "wb") as f:
                pos, end = sector * SECTOR, sector * SECTOR + size
                while pos < end:
                    n = min(end - pos, 8 * 1024 * 1024)
                    f.write(god.read(pos, n))
                    pos += n
            stats[0] += 1
            stats[1] += size


def main():
    header, out_dir = sys.argv[1], sys.argv[2]
    god = GodStream(header)
    vd = god.read(0x10000, SECTOR)
    if vd[:20] != b"MICROSOFT*XBOX*MEDIA":
        sys.exit("XDVDFS volume descriptor not found at 0x10000")
    root_sector, root_size = struct.unpack_from("<II", vd, 0x14)
    os.makedirs(out_dir, exist_ok=True)
    stats = [0, 0]
    walk(god, root_sector, root_size, out_dir, "", stats)
    print(f"extracted {stats[0]} files, {stats[1] / 2**30:.2f} GiB -> {out_dir}")


if __name__ == "__main__":
    main()
