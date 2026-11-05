"""Print a recompiled guest function's PPC disassembly (from the generated comments).
Usage: python -I tools/show_func.py sub_X [--c]   (--c also prints the C++ lines)"""
import json, os, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "project", "ng2", "generated", "default")
idx = json.load(open(os.path.join(ROOT, "notes", "func_index.json")))
for name in [a for a in sys.argv[1:] if not a.startswith("--")]:
    v = idx[name]
    print(f"===== {name} ({v['file']}:{v['line']})")
    with open(os.path.join(GEN, v["file"]), encoding="utf-8", errors="replace") as f:
        lines = f.readlines()[v["line"]:]
    for l in lines:
        if l.startswith("DEFINE_REX_FUNC") or l.startswith("}") and not l.startswith("}\t"):
            break
        if l.startswith("loc_"):
            print(l.rstrip())
        elif l.startswith("\t// "):
            print("   ", l[4:].rstrip())
        elif "--c" in sys.argv:
            print("        ", l.strip())
