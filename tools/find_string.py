"""Search the dumped XEX image (ghidra-db\\image) for a string as ASCII, UTF-16BE and UTF-16LE.

Usage: python -I tools/find_string.py <image dir> <text> [<text> ...]
Prints guest address, encoding, and a little surrounding context.
"""
import glob
import os
import sys


def main():
    image_dir = sys.argv[1]
    blocks = []
    for path in glob.glob(os.path.join(image_dir, "*_?.bin")):
        with open(path, "rb") as f:
            blocks.append((int(os.path.basename(path)[:8], 16), f.read()))
    for text in sys.argv[2:]:
        for enc in ("ascii", "utf-16-be", "utf-16-le"):
            needle = text.encode(enc)
            for start, data in blocks:
                i = data.find(needle)
                while i != -1:
                    ctx = data[max(0, i - 16): i + len(needle) + 32]
                    print(f"{start + i:08X} {enc:9} {text!r}  ctx={ctx!r}")
                    i = data.find(needle, i + 1)


if __name__ == "__main__":
    main()
