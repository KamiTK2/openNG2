"""Dump UTF-16BE strings (and raw hex) from a file region.

Usage: python -I tools/dump_utf16.py <file> <start-hex> <end-hex> [--hex]
Lists each NUL-terminated UTF-16BE string with its offset.
"""
import sys


def main():
    path, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
    with open(path, "rb") as f:
        f.seek(start)
        data = f.read(end - start)
    if "--hex" in sys.argv:
        for i in range(0, len(data), 16):
            row = data[i:i + 16]
            print(f"{start + i:08X}  {row.hex(' ')}")
        return
    i = 0
    while i + 1 < len(data):
        j = i
        chars = []
        while j + 1 < len(data):
            c = (data[j] << 8) | data[j + 1]
            j += 2
            if c == 0:
                break
            chars.append(c)
        if chars and all(0x20 <= c < 0xD800 or c in (0x0A, 0x0D) for c in chars):
            print(f"{start + i:08X}  len {len(chars):3}  {''.join(map(chr, chars))!r}")
            i = j
        else:
            i += 2


if __name__ == "__main__":
    main()
