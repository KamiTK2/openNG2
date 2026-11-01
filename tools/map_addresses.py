"""Map guest code addresses from one XEX build to another (e.g. 1.0 -> Title Update #2).

Usage: python -I tools/map_addresses.py <old image dir> <new image dir> <addr-hex> [...]
       python -I tools/map_addresses.py <old> <new> @file      (one hex address per line; # comments)

Image dirs hold <start-hex>_<x|d>.bin blocks (Ghidra DumpImage.java or the TU image split).
For each old address, the instructions from there on are masked so build-dependent fields drop
out (b/bl/bc displacements, lis immediates, and every 16-bit immediate of addi/ori/loads/stores
whose base register was loaded by lis in the window), then searched in the new code. The window
grows from 8 to 64 instructions until the match is unique. Prints:
    old new delta window status
status: unique | ambiguous(n) | none.
       python -I tools/map_addresses.py <old> <new> --data <addr-hex> [...]
maps data addresses instead: finds old code that builds the address (lis + addi/load/store),
maps that code position, and decodes the address the new code builds at the same spot.  Addresses that are not function starts work too (the
window simply starts mid-function), but prefer function entries.
"""
import glob
import os
import struct
import sys

LOADSTORE = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
             52, 53, 54, 55, 58, 62}
STORES = {36, 37, 38, 39, 44, 45, 52, 53, 54, 55, 62}


def load(image_dir):
    blocks = []
    for path in glob.glob(os.path.join(image_dir, "*_?.bin")):
        name = os.path.basename(path)
        blocks.append((int(name[:8], 16), name[9] == "x", open(path, "rb").read()))
    return sorted(blocks)


def words(blocks, addr, n):
    for start, _, data in blocks:
        if start <= addr and addr + 4 * n <= start + len(data):
            return list(struct.unpack_from(">%dI" % n, data, addr - start))
    return None


def mask(insns):
    out = []
    lis_regs = set()
    for w in insns:
        op = w >> 26
        rt = (w >> 21) & 31
        ra = (w >> 16) & 31
        m = w
        if op == 18:              # b / bl
            m = w & 0xFC000003
        elif op == 16:            # bc
            m = w & 0xFFFF0003
        elif op == 15:            # addis / lis
            m = w & 0xFFFF0000
            if ra == 0:
                lis_regs.add(rt)
        elif op in (14, 24, 25) or op in LOADSTORE:  # addi / ori / oris / loads & stores
            if ra in lis_regs or (op in (24, 25) and rt in lis_regs):
                m = w & 0xFFFF0000
        out.append(m)
        # a register written by anything other than lis/addi from lis stops being "an address"
        if op not in (15, 14, 24) and rt in lis_regs and op not in LOADSTORE:
            lis_regs.discard(rt)
    return out


def loose(w):
    """Position-independent superset mask: drops every displacement / 16-bit immediate that
    mask() might drop, so candidates can be found by substring search and then verified."""
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op == 16:
        return w & 0xFFFF0003
    if op in (14, 15, 24, 25) or op in LOADSTORE:
        return w & 0xFFFF0000
    return w


def build_index(blocks):
    """Loosely masked new code per executable block, as bytes, for fast substring search."""
    idx = []
    for start, is_x, data in blocks:
        if not is_x:
            continue
        n = len(data) // 4
        ws = struct.unpack_from(">%dI" % n, data, 0)
        idx.append((start, struct.pack(">%dI" % n, *map(loose, ws)), ws))
    return idx


def find_all(idx, old_ws):
    pat = struct.pack(">%dI" % len(old_ws), *map(loose, old_ws))
    hits = []
    for start, buf, ws in idx:
        k = buf.find(pat)
        while k != -1:
            if k % 4 == 0:
                # Re-verify with the query's own lis tracking at this exact position.
                cand = list(ws[k // 4:k // 4 + len(old_ws)])
                if mask(cand) == mask(old_ws):
                    hits.append(start + k)
            k = buf.find(pat, k + 1)
    return hits


def agree(old, new, a, b, limit=256):
    """Number of leading instructions that match (masked) between old@a and new@b."""
    for n in (limit, 128, 64, 32, 16, 8):
        wa, wb = words(old, a, n), words(new, b, n)
        if wa and wb:
            ma, mb = mask(wa), mask(wb)
            k = 0
            while k < n and ma[k] == mb[k]:
                k += 1
            return k
    return 0


def similarity(old, new, a, b, n=64):
    wa, wb = words(old, a, n), words(new, b, n)
    if not wa or not wb:
        return 0.0
    ma, mb = mask(wa), mask(wb)
    return sum(x == y for x, y in zip(ma, mb)) / n


def is_func_start(blocks, addr):
    w = words(blocks, addr - 4, 1)
    return w is not None and w[0] in (0x4E800020, 0x4E800420, 0) or (
        w is not None and (w[0] >> 26) == 18 and not w[0] & 1)


def simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def data_refs(blocks, target, limit=12):
    """(lis_addr, use_addr) pairs in executable blocks that build `target`."""
    hi = ((target + 0x8000) >> 16) & 0xFFFF
    refs = []
    for start, is_x, data in blocks:
        if not is_x:
            continue
        n = len(data) // 4
        ws = struct.unpack_from(">%dI" % n, data, 0)
        for i, w in enumerate(ws):
            if (w >> 26) != 15 or (w >> 16) & 31 or (w & 0xFFFF) != hi:
                continue
            rx = (w >> 21) & 31
            for j in range(i + 1, min(i + 48, n)):
                u = ws[j]
                op = u >> 26
                if (op == 14 or op in LOADSTORE) and (u >> 16) & 31 == rx:
                    if ((hi << 16) + simm(u)) & 0xFFFFFFFF == target:
                        refs.append((start + 4 * i, start + 4 * j))
                        break
                # Stop once rx is overwritten (stores don't write their rS operand).
                if (u >> 21) & 31 == rx and op not in STORES:
                    break
            if len(refs) >= limit:
                return refs
    return refs


def map_data(old, new, idx, target):
    votes = {}
    for lis_a, use_a in data_refs(old, target):
        # Map a window that starts a few instructions before the lis.
        for back in (0, 4, 8, 16):
            ws = words(old, lis_a - 4 * back, 16 + back)
            if not ws:
                continue
            hits = find_all(idx, ws)
            if len(hits) != 1:
                continue
            nl = hits[0] + 4 * back
            nu = nl + (use_a - lis_a)
            wl, wu = words(new, nl, 1)[0], words(new, nu, 1)[0]
            if (wl >> 26) == 15:
                v = (((wl & 0xFFFF) << 16) + simm(wu)) & 0xFFFFFFFF
                votes[v] = votes.get(v, 0) + 1
            break
    return sorted(votes.items(), key=lambda kv: -kv[1])


def main():
    old, new = load(sys.argv[1]), load(sys.argv[2])
    if len(sys.argv) > 3 and sys.argv[3] == "--data":
        idx = build_index(new)
        for a in sys.argv[4:]:
            t = int(a, 16)
            votes = map_data(old, new, idx, t)
            print("%08X -> %s" % (t, ", ".join("%08X (%d refs, %+X)" % (v, c, v - t)
                                              for v, c in votes) or "no mapped refs"))
        return
    addrs = []
    for a in sys.argv[3:]:
        if a.startswith("@"):
            for line in open(a[1:]):
                line = line.split("#")[0].strip()
                if line:
                    addrs.append(int(line, 16))
        else:
            addrs.append(int(a, 16))
    idx = build_index(new)
    results = {}
    for a in addrs:
        result = None
        for n in (8, 12, 16, 24, 32, 48, 64):
            ws = words(old, a, n)
            if ws is None:
                result = (None, n, "not-in-old-code")
                break
            hits = find_all(idx, ws)
            if len(hits) == 1:
                result = (hits[0], n, "unique")
                break
            if not hits:
                result = (None, n, "none")
                break
            result = (None, n, "ambiguous(%d)" % len(hits))
        results[a] = result
    # Fuzzy fallback: use the delta of the nearest uniquely mapped address and look around it
    # for the most similar function start.
    good = sorted((a, r[0] - a) for a, r in results.items() if r[0] is not None)
    for a in addrs:
        tgt, n, status = results[a]
        if tgt is not None:
            print("%08X %08X %+8X %3d %-12s agree=%d" % (a, tgt, tgt - a, n, status,
                                                         agree(old, new, a, tgt)))
            continue
        best = None
        near = sorted(good, key=lambda g: abs(g[0] - a))[:4]
        for _, delta in near:
            for d in range(-0x400, 0x404, 4):
                b = a + delta + d
                if not is_func_start(new, b):
                    continue
                sim = similarity(old, new, a, b)
                if best is None or sim > best[1]:
                    best = (b, sim)
        if best and best[1] >= 0.5:
            print("%08X %08X %+8X %3s %-12s fuzzy sim=%.0f%%" % (a, best[0], best[0] - a, "", status,
                                                                 best[1] * 100))
        else:
            print("%08X %-8s %8s %3d %s" % (a, "-", "", n, status))


if __name__ == "__main__":
    main()
