"""Index the recompiled guest code (project/ng2/generated/default/*.cpp).

Writes a JSON: {func: {"file","line","calls":[...],"imm":[lis hi values]}}.
Usage: python -I tools/func_index.py [out.json]
Query helpers: python -I tools/func_index.py --callers sub_X | --callees sub_X | --range lo hi
"""
import json, os, re, sys, glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "project", "ng2", "generated", "default")
CACHE = os.path.join(ROOT, "notes", "func_index.json")

DEF = re.compile(r"^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|REX_FUNC)\((\w+)\)")
CALL = re.compile(r"^\t(sub_[0-9A-F]+|__imp__\w+)\(ctx, base\);")
INDIRECT = re.compile(r"PPC_CALL_INDIRECT_FUNC|REX_CALL_INDIRECT")


def build():
    idx = {}
    for path in sorted(glob.glob(os.path.join(GEN, "ng2_recomp.*.cpp"))):
        cur = None
        with open(path, encoding="utf-8", errors="replace") as f:
            for n, line in enumerate(f, 1):
                m = DEF.match(line)
                if m:
                    cur = {"file": os.path.basename(path), "line": n, "calls": [], "ind": 0, "len": 0}
                    idx[m.group(1)] = cur
                    continue
                if cur is None:
                    continue
                if line.startswith("\t// ") and not line.startswith("\t// ---"):
                    cur["len"] += 1
                m = CALL.match(line)
                if m:
                    if m.group(1) not in cur["calls"]:
                        cur["calls"].append(m.group(1))
                elif INDIRECT.search(line):
                    cur["ind"] += 1
    with open(CACHE, "w") as f:
        json.dump(idx, f)
    return idx


def load():
    if os.path.exists(CACHE):
        with open(CACHE) as f:
            return json.load(f)
    return build()


def addr(name):
    return int(name[4:], 16) if name.startswith("sub_") else 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a or a[0] == "--build":
        idx = build(); print(len(idx), "functions ->", CACHE); sys.exit()
    idx = load()
    if a[0] == "--callers":
        for k, v in sorted(idx.items()):
            if a[1] in v["calls"]:
                print(k, v["len"])
    elif a[0] == "--callees":
        v = idx[a[1]]; print(v["file"], v["line"], "len", v["len"], "ind", v["ind"]); print(" ".join(v["calls"]))
    elif a[0] == "--range":
        lo, hi = int(a[1], 16), int(a[2], 16)
        for k in sorted(idx, key=addr):
            if lo <= addr(k) < hi:
                v = idx[k]; print(k, v["len"], "ind", v["ind"], "calls", " ".join(v["calls"][:12]))
