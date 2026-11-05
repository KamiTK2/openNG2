"""Regenerate patches/rexglue-sdk-v0.10.0.patch from the local SDK checkout (tracked
changes + untracked new files, submodules excluded). Usage: python -I tools/update_sdk_patch.py"""
import os, subprocess
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SDK = os.path.join(ROOT, "tools", "rexglue-sdk")
OUT = os.path.join(ROOT, "patches", "rexglue-sdk-v0.10.0.patch")
git = lambda *a: subprocess.run(["git", "-C", SDK, *a], check=True, capture_output=True).stdout
new = [p for p in git("ls-files", "--others", "--exclude-standard").decode().splitlines() if p]
if new:
    git("add", "-N", "--", *new)
try:
    diff = git("diff", "--binary", "--ignore-submodules=all", "HEAD")
finally:
    if new:
        git("reset", "-q", "--", *new)
open(OUT, "wb").write(diff)
print(f"{OUT}: {len(diff)} bytes, {len(new)} new files")
