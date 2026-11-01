"""Find guest addresses whose value follows an expected delta pattern across snapshots.

Usage: python -I tools/memdiff.py <deltas> <dump1.bin> <dump2.bin> ... [--width 1|2|4] [--max 40]
  deltas: comma list of expected value differences relative to dump1, one per dump,
          e.g. "0,1,2,1" for down, down, up.
Dumps come from the app's ng2_dumpmem trigger: repeated [u32 LE start][u32 LE size][bytes].
Values are read big-endian (guest order). Only pages that differ between dumps are scanned.
"""
import struct
import sys

PAGE = 0x10000


def load(path):
    ranges = []
    with open(path, "rb") as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            start, size = struct.unpack("<II", hdr)
            ranges.append((start, f.read(size)))
    return ranges


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    width = int(sys.argv[sys.argv.index("--width") + 1]) if "--width" in sys.argv else 4
    limit = int(sys.argv[sys.argv.index("--max") + 1]) if "--max" in sys.argv else 40
    if "--width" in sys.argv:
        args.remove(str(width))
    if "--max" in sys.argv:
        args.remove(str(limit))
    deltas = [int(x) for x in args[0].split(",")]
    dumps = [load(p) for p in args[1:]]
    assert len(deltas) == len(dumps), "need one delta per dump"
    fmt = {1: ">B", 2: ">H", 4: ">I"}[width]
    hits = []
    for ri, (start, base) in enumerate(dumps[0]):
        datas = [d[ri][1] for d in dumps]
        for page in range(0, len(base), PAGE):
            chunks = [d[page:page + PAGE] for d in datas]
            if all(c == chunks[0] for c in chunks[1:]):
                continue
            for off in range(0, len(chunks[0]) - width + 1, width):
                v0 = struct.unpack_from(fmt, chunks[0], off)[0]
                ok = True
                if "--linear" in sys.argv:
                    # Any step s != 0: value_k == v0 + deltas[k] * s for all k.
                    v1 = struct.unpack_from(fmt, chunks[1], off)[0]
                    step = v1 - v0
                    if step == 0 or deltas[1] == 0 or abs(step) > 0x4000:
                        continue
                    step //= deltas[1]
                    for k in range(2, len(chunks)):
                        if struct.unpack_from(fmt, chunks[k], off)[0] != v0 + deltas[k] * step:
                            ok = False
                            break
                    if ok:
                        hits.append((start + page + off,
                                     [struct.unpack_from(fmt, c, off)[0] for c in chunks]))
                    continue
                for k in range(1, len(chunks)):
                    if struct.unpack_from(fmt, chunks[k], off)[0] != v0 + deltas[k]:
                        ok = False
                        break
                if ok and v0 < 64:
                    hits.append((start + page + off, [struct.unpack_from(fmt, c, off)[0] for c in chunks]))
    for addr, vals in hits[:limit]:
        print(f"{addr:08X}  {vals}")
    print(f"# {len(hits)} candidates (width {width})")


if __name__ == "__main__":
    main()
