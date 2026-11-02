"""Assemble the openNG2 release folder (launcher + payload). Contains no game data or code.

Usage: python -I tools/make_release.py [out_dir]   (default: dist/openNG2)

Layout:
  openNG2.exe                      the launcher (launcher/out/openNG2.exe; build it first)
  README.txt
  payload/project/ng2/             our project: CMakeLists.txt, ng2.toml, ng2_manifest.toml, src/,
                                   config/, shaders/ (shader manifest: hashes + addresses only) and
                                   generated/rexglue.cmake (the SDK's build template)
  payload/sdk/                     ReXGlue SDK install, Release + RelWithDebInfo only: bin/
                                   (rexglue.exe codegen + runtime DLLs), include/, lib/, share/
The launcher copies payload/project into <install>/build, runs the codegen + compile against
the player's own game files, and installs ng2.exe (findings #67).
"""
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "openNG2")
SDK = os.path.join(ROOT, "tools", "rexglue-sdk", "out", "install", "win-amd64")
PROJECT = os.path.join(ROOT, "project", "ng2")
LAUNCHER = os.path.join(ROOT, "launcher", "out", "openNG2.exe")

# Never ship anything game-derived.
EXCLUDE_SRC = {"ng2_trace_all.cpp", "ng2_trace_all.h"}  # generated from the game's function list


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def is_debug_variant(name, names):
    stem, ext = os.path.splitext(name)
    if stem.endswith("rd"):
        return False
    return stem.endswith("d") and (stem[:-1] + ext) in names


def main():
    if not os.path.exists(LAUNCHER):
        sys.exit("Build the launcher first (launcher/out/openNG2.exe).")
    if os.path.exists(OUT):
        shutil.rmtree(OUT)
    copy(LAUNCHER, os.path.join(OUT, "openNG2.exe"))
    with open(os.path.join(OUT, "README.txt"), "w", encoding="utf-8") as f:
        f.write(
            "openNG2 - Ninja Gaiden II (Xbox 360) for Windows\n"
            "================================================\n\n"
            "Run openNG2.exe and add your own files:\n"
            "  Required: the game (Games on Demand package C1200E9B558D04454FA6 + its .data folder,\n"
            "            a disc image .iso, or an extracted game folder with default.xex)\n"
            "  Required: Title Update #2, TU_1A461UL_000000G000000.00000000000G4\n"
            "  Optional: the DLC packages (Mission Mode and the three costume packs)\n\n"
            "The launcher checks the files, downloads a compiler, rebuilds the game for Windows from\n"
            "your copy and gives you ng2.exe. No game files are included in this download.\n"
            "Keep the payload folder next to openNG2.exe.\n")

    # Project.
    proj_out = os.path.join(OUT, "payload", "project", "ng2")
    for name in ("CMakeLists.txt", "ng2.toml", "ng2_manifest.toml"):
        copy(os.path.join(PROJECT, name), os.path.join(proj_out, name))
    for sub in ("src", "config", "shaders"):
        for base, _, files in os.walk(os.path.join(PROJECT, sub)):
            for name in files:
                if name in EXCLUDE_SRC:
                    continue
                src = os.path.join(base, name)
                copy(src, os.path.join(proj_out, os.path.relpath(src, PROJECT)))
    copy(os.path.join(PROJECT, "generated", "rexglue.cmake"), os.path.join(proj_out, "generated", "rexglue.cmake"))

    # SDK (no Debug variants).
    sdk_out = os.path.join(OUT, "payload", "sdk")
    for sub in ("include", "share", "licenses"):
        if os.path.isdir(os.path.join(SDK, sub)):
            shutil.copytree(os.path.join(SDK, sub), os.path.join(sdk_out, sub))
    # SDL3 installs its package files in <prefix>/cmake (the SDK config needs them).
    for base, _, files in os.walk(os.path.join(SDK, "cmake")):
        for fn in files:
            if not fn.endswith("-debug.cmake"):
                s = os.path.join(base, fn)
                copy(s, os.path.join(sdk_out, os.path.relpath(s, SDK)))
    for sub in ("bin", "lib"):
        names = set(os.listdir(os.path.join(SDK, sub)))
        for name in sorted(names):
            src = os.path.join(SDK, sub, name)
            if os.path.isdir(src):
                if name == "cmake":
                    for base, _, files in os.walk(src):
                        for fn in files:
                            if fn.endswith("-debug.cmake"):
                                continue
                            s = os.path.join(base, fn)
                            copy(s, os.path.join(sdk_out, sub, os.path.relpath(s, os.path.join(SDK, sub))))
                continue
            if is_debug_variant(name, names) or name.endswith(".pdb"):
                continue
            copy(src, os.path.join(sdk_out, sub, name))

    total = sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(OUT) for f in fs)
    print(f"release folder: {OUT} ({total / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
