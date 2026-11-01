"""Print big-endian words from a memdump (tools\\memdiff.py format) around an address.

Usage: python -I tools/peek.py <dump.bin> <start-hex> <end-hex>
Marks words that look like pointers into the XEX image (0x82000000-0x85A80000).
"""
import struct
import sys


def main():
    path, lo, hi = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    with open(path, "rb") as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            start, size = struct.unpack("<II", hdr)
            data = f.read(size)
            if start <= lo < start + size:
                for a in range(lo & ~3, hi, 16):
                    words = [struct.unpack_from(">I", data, a - start + i)[0] for i in range(0, 16, 4)]
                    tags = ["*" if 0x82000000 <= w < 0x85A80000 else " " for w in words]
                    print(f"{a:08X}  " + "  ".join(f"{w:08X}{t}" for w, t in zip(words, tags)))
                return


if __name__ == "__main__":
    main()
