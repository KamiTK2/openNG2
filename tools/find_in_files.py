"""Stream-search files for strings as ASCII / UTF-16BE / UTF-16LE.

Usage: python -I tools/find_in_files.py <dir> <text> [<text> ...]
Prints file, offset, encoding (first 20 hits per pattern per file).
"""
import os
import sys

CHUNK = 64 * 1024 * 1024


def main():
    root = sys.argv[1]
    needles = []
    for t in sys.argv[2:]:
        for enc in ("ascii", "utf-16-be", "utf-16-le"):
            needles.append((t, enc, t.encode(enc)))
    overlap = max(len(n[2]) for n in needles)
    for dirpath, _, files in os.walk(root):
        for name in sorted(files):
            path = os.path.join(dirpath, name)
            hits = {}
            with open(path, "rb") as f:
                base, tail = 0, b""
                while True:
                    chunk = f.read(CHUNK)
                    if not chunk:
                        break
                    buf = tail + chunk
                    off0 = base - len(tail)
                    for t, enc, n in needles:
                        i = buf.find(n)
                        while i != -1:
                            key = (t, enc)
                            hits.setdefault(key, [])
                            pos = off0 + i
                            if len(hits[key]) < 20 and pos not in hits[key]:
                                hits[key].append(pos)
                            i = buf.find(n, i + 1)
                    tail = buf[-overlap:]
                    base += len(chunk)
            for (t, enc), offs in hits.items():
                print(f"{os.path.relpath(path, root)}  {enc:9} {t!r}  x{len(offs)}  first: "
                      + " ".join(f"{o:X}" for o in offs[:5]))


if __name__ == "__main__":
    main()
