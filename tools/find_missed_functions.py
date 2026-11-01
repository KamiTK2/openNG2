"""Find code addresses that are referenced indirectly but not registered as functions.

Usage: python -I tools/find_missed_functions.py <image dir> <generated/default dir> [exclude-hex ...]

<image dir> holds raw memory blocks from tools/ghidra-scripts/DumpImage.java, named
<start-hex>_<x|d>.bin (x = executable). Two reference sources are scanned:
  data  - aligned 32-bit words in non-executable blocks that point into code
  code  - `lis rX,hi` followed (within 64 insns) by `addi rY,rX,lo` / `ori rY,rX,lo`
          forming a code address (function pointers built at runtime)
A target is a candidate when it is not a registered entry, not an internal label in the
generated code, the word before it ends a function (blr, bctr, zero padding, or a `b`
that does not jump forward over the target), and it isn't zero.
Prints "target refs kind first_ref" lines.
"""
import glob
import os
import re
import struct
import sys


def load_image(image_dir):
    blocks = []
    for path in glob.glob(os.path.join(image_dir, "*_?.bin")):
        name = os.path.basename(path)
        start = int(name[:8], 16)
        with open(path, "rb") as f:
            blocks.append((start, name[9] == "x", f.read()))
    return sorted(blocks)


def word_at(blocks, addr):
    for start, _, data in blocks:
        if start <= addr and addr + 4 <= start + len(data):
            return struct.unpack_from(">I", data, addr - start)[0]
    return None


def starts_function(before, tgt):
    if before in (0x4E800020, 0x4E800420, 0x00000000):
        return True
    if (before >> 26) == 18 and (before & 3) == 0:  # b (no link, relative)
        disp = before & 0x03FFFFFC
        if disp & 0x02000000:
            disp -= 0x04000000
        # A short forward b over the target means the target is inside the same function
        # (e.g. an SEH __except resume block skipped by +0x18). A long forward b is a
        # tail call from the previous thunk (e.g. "subi r3,r3,4 ; b far"), so the target
        # still starts a function.
        return not (0 < disp <= 0x400 and tgt - 4 + disp > tgt)
    return False


def main():
    image_dir, gen_dir = sys.argv[1], sys.argv[2]
    exclude = {int(x, 16) for x in sys.argv[3:]}
    blocks = load_image(image_dir)
    code = [(s, d) for s, x, d in blocks if x]
    lo = min(s for s, _ in code)
    hi = max(s + len(d) for s, d in code)

    entries, labels = set(), set()
    with open(os.path.join(gen_dir, "ng2_init.cpp"), encoding="utf-8", errors="replace") as f:
        entries.update(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-F]{8}), \w+ \}", f.read()))
    for path in glob.glob(os.path.join(gen_dir, "ng2_recomp.*.cpp")):
        with open(path, encoding="utf-8", errors="replace") as f:
            labels.update(int(m, 16) for m in re.findall(r"^loc_([0-9A-F]{8}):", f.read(), re.M))

    refs = {}  # target -> [kind, count, first_ref]

    def add(tgt, kind, where):
        if not (lo <= tgt < hi) or tgt & 3:
            return
        r = refs.setdefault(tgt, [kind, 0, where])
        r[1] += 1

    for start, is_x, data in blocks:
        n = len(data) // 4
        words = struct.unpack_from(f">{n}I", data)
        if not is_x:
            for i, v in enumerate(words):
                if lo <= v < hi:
                    add(v, "data", start + i * 4)
            continue
        lis = {}  # reg -> (hi value, insn index)
        for i, w in enumerate(words):
            op = w >> 26
            rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:  # lis rD,imm
                lis[rd] = ((imm << 16) & 0xFFFFFFFF, i)
                continue
            if op in (14, 24) and ra in lis and i - lis[ra][1] <= 64:  # addi / ori
                base = lis[ra][0]
                if op == 14:
                    tgt = (base + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
                    dst = rd
                else:  # ori rA,rS,imm: dest is the `ra` field, source is `rd` field
                    if rd not in lis or i - lis[rd][1] > 64:
                        continue
                    tgt = lis[rd][0] | imm
                    dst = ra
                add(tgt, "code", start + i * 4)
                if dst in lis and dst != ra:
                    del lis[dst]
            elif w == 0x4E800020:  # blr ends the window
                lis.clear()

    out = []
    for tgt, (kind, count, where) in sorted(refs.items()):
        if tgt in entries or tgt in labels or tgt in exclude:
            continue
        at, before = word_at(blocks, tgt), word_at(blocks, tgt - 4)
        if not at or before is None or not starts_function(before, tgt):
            continue
        # MSVC emits switch jump tables inline right after the bctr; their first word is a
        # code address, not an instruction.
        if lo <= at < hi:
            continue
        out.append(f"{tgt:08X} {count:3d} {kind} {where:08X}")
    print("\n".join(out))
    print(f"# {len(out)} candidates ({len(entries)} entries, {len(labels)} labels)", file=sys.stderr)


if __name__ == "__main__":
    main()
