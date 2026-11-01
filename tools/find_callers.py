"""List `bl`/`b` call sites of guest functions in an image dir, with the containing function.

Usage: python -I tools/find_callers.py <image dir> <target-hex> [...]
The containing function is approximated by scanning back to the previous blr/bctr/padding
(prints its start and the call offset in instructions).
"""
import glob
import os
import struct
import sys


def main():
    blocks = []
    for path in glob.glob(os.path.join(sys.argv[1], "*_x.bin")):
        name = os.path.basename(path)
        data = open(path, "rb").read()
        blocks.append((int(name[:8], 16), struct.unpack_from(">%dI" % (len(data) // 4), data)))
    targets = {int(a, 16) for a in sys.argv[2:]}
    for start, ws in sorted(blocks):
        for i, w in enumerate(ws):
            if (w >> 26) != 18:
                continue
            disp = w & 0x03FFFFFC
            if disp & 0x02000000:
                disp -= 0x04000000
            tgt = (disp if w & 2 else start + 4 * i + disp) & 0xFFFFFFFF
            if tgt not in targets:
                continue
            j = i
            while j > 0 and ws[j - 1] not in (0x4E800020, 0x4E800420, 0):
                j -= 1
            kind = "bl" if w & 1 else "b "
            print("%08X %s %08X  in ~%08X +%d" % (start + 4 * i, kind, tgt, start + 4 * j, i - j))


if __name__ == "__main__":
    main()
