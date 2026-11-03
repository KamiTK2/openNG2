"""List guest functions that build PM4 type-3 packet headers (lis+ori/addi constants).
Usage: python -I tools/find_pm4.py [opcode_hex ...]"""
import json, os, re, sys, collections
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "project", "ng2", "generated", "default")
idx = json.load(open(os.path.join(ROOT, "notes", "func_index.json")))
want = {int(x, 16) for x in sys.argv[1:]}
starts = collections.defaultdict(list)
for k, v in idx.items():
    starts[v["file"]].append((v["line"], k))
res = collections.defaultdict(list)
for fn in starts:
    st = sorted(starts[fn]); j = 0; cur = None; lis = {}
    for n, l in enumerate(open(os.path.join(GEN, fn), encoding="utf-8", errors="replace"), 1):
        while j < len(st) and st[j][0] <= n:
            cur = st[j][1]; j += 1; lis = {}
        m = re.match(r"\t// lis (r\d+),(-?\d+)$", l)
        if m:
            lis[m.group(1)] = int(m.group(2)) & 0xFFFF; continue
        m = re.match(r"\t// (ori|addi) (r\d+),(r\d+),(-?\d+)$", l)
        if m and m.group(3) in lis:
            v = ((lis[m.group(3)] << 16) + int(m.group(4))) & 0xFFFFFFFF
            if v >> 30 == 3 and (v & 1 << 15) == 0:
                op = (v >> 8) & 0x7F
                if not want or op in want:
                    res[cur].append(f"{v:08X}")
for f in sorted(res, key=lambda k: int(k[4:], 16)):
    print(f, idx[f]["len"], " ".join(sorted(set(res[f]))))
