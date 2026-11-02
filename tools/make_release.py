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
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Single version source (also built into the launcher and the game for the update check).
# Publish as GitHub release tag "v<VERSION>" with openNG2-<VERSION>.zip attached: the update
# check (project/ng2/src/update_check.cpp) looks for exactly that.
VERSION = open(os.path.join(ROOT, "VERSION"), encoding="utf-8").read().strip()
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "dist", "openNG2")
SDK = os.path.join(ROOT, "tools", "rexglue-sdk", "out", "install", "win-amd64")
PROJECT = os.path.join(ROOT, "project", "ng2")
LAUNCHER = os.path.join(ROOT, "launcher", "out", "openNG2.exe")

# Never ship anything game-derived.
EXCLUDE_SRC = {"ng2_trace_all.cpp", "ng2_trace_all.h"}  # generated from the game's function list


# Third-party notices for everything shipped in the release (SDK DLLs + codegen tool, launcher).
# (library, licence file under the SDK's thirdparty/ folder, or None + an inline notice)
THIRD_PARTY = [
    ("SDL3", "sdl3/LICENSE.txt", None),
    ("{fmt}", "fmt/LICENSE", None),
    ("spdlog", "spdlog/LICENSE", None),
    ("FFmpeg (libavcodec, libavutil; LGPL 2.1 build with a reduced codec list; source: "
     "https://ffmpeg.org - rebuildable from the ReXGlue SDK source)", "FFmpeg/COPYING.LGPLv2.1", None),
    ("libmspack (LGPL 2.1; source: https://github.com/kyz/libmspack)", "libmspack/libmspack/COPYING.LIB", None),
    ("xxHash", "xxHash/LICENSE", None),
    ("Dear ImGui", "imgui/LICENSE.txt", None),
    ("toml++", "tomlplusplus/LICENSE", None),
    ("utfcpp", "utfcpp/LICENSE", None),
    ("Tracy", "tracy/LICENSE", None),
    ("o1heap", "o1heap/LICENSE", None),
    ("SIMDe", "simde/COPYING", None),
    ("aes_128", "aes_128/LICENSE", None),
    ("DES (crypto)", "crypto/des/LICENSE", None),
    ("CLI11 (codegen tool)", "cli11/LICENSE", None),
    ("Inja (codegen tool)", "inja/LICENSE", None),
    ("glslang", "glslang/LICENSE.txt", None),
    ("SPIRV-Tools", "spirv-tools/LICENSE", None),
    ("SPIRV-Headers", "spirv-headers/LICENSE", None),
    ("Vulkan-Headers", "vulkan-headers/LICENSE.md", None),
    ("Vulkan-Loader", "vulkan-loader/LICENSE.txt", None),
    ("Vulkan Memory Allocator", "vulkan-memory-allocator/LICENSE.txt", None),
    ("stb", None, "stb (Sean Barrett): public domain / MIT, see https://github.com/nothings/stb"),
    ("tiny-AES-c", None, "tiny-AES-c (kokke): The Unlicense, see https://github.com/kokke/tiny-AES-c"),
    ("RenderDoc in-application API header", None,
     "renderdoc_app.h, Copyright (c) Baldur Karlsson: The MIT License, see https://github.com/baldurk/renderdoc"),
    ("disruptorplus", None, "disruptorplus (Lewis Baker): MIT License, see https://github.com/lewissbaker/disruptorplus"),
    ("DirectX Shader Compiler", None,
     "DirectX Shader Compiler (Microsoft): University of Illinois/NCSA Open Source License, "
     "see https://github.com/microsoft/DirectXShaderCompiler"),
    ("AMD FidelityFX FSR 1.0 and CAS (compiled shaders)", None,
     "Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.\n\n"
     "Permission is hereby granted, free of charge, to any person obtaining a copy of this software and "
     "associated documentation files (the \"Software\"), to deal in the Software without restriction, including "
     "without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell "
     "copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the "
     "following conditions:\n\nThe above copyright notice and this permission notice shall be included in all "
     "copies or substantial portions of the Software.\n\nTHE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF "
     "ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A "
     "PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY "
     "CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF "
     "OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE."),
]


def write_notices(path):
    sdk_src = os.path.join(ROOT, "tools", "rexglue-sdk")
    parts = [
        "openNG2 " + VERSION + " - third-party notices",
        "=" * 60,
        "",
        "openNG2 includes software from the projects below. Ninja Gaiden II itself is not included;",
        "it belongs to Koei Tecmo and is built from the player's own copy.",
        "",
    ]
    def section(title, text):
        parts.extend(["", "-" * 60, title, "-" * 60, "", text.strip(), ""])
    section("ReXGlue SDK (includes code derived from the Xenia project)",
            open(os.path.join(sdk_src, "LICENSE"), encoding="utf-8", errors="replace").read())
    for name, rel, inline in THIRD_PARTY:
        if rel:
            f = os.path.join(sdk_src, "thirdparty", rel)
            if not os.path.exists(f):
                sys.exit("missing licence file: " + f)
            section(name, open(f, encoding="utf-8", errors="replace").read())
        else:
            section(name, inline)
    section("Build tools downloaded by the launcher (not included in this release)",
            "LLVM (Apache 2.0 with LLVM exceptions), CMake (BSD 3-clause), Ninja (Apache 2.0), "
            "xwin (MIT / Apache 2.0), and Microsoft's Visual C++ libraries and Windows SDK (downloaded "
            "from Microsoft after the player accepts Microsoft's licence terms).")
    with open(path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(parts) + "\n")


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
            f"openNG2 {VERSION} - Ninja Gaiden II (Xbox 360) for Windows\n"
            "====================================================\n\n"
            "Run openNG2.exe and add your own files:\n"
            "  Required: the game (Games on Demand package C1200E9B558D04454FA6 + its .data folder,\n"
            "            a disc image .iso, or an extracted game folder with default.xex)\n"
            "  Required: Title Update #2, TU_1A461UL_000000G000000.00000000000G4\n"
            "  Optional: the DLC packages (Mission Mode and the three costume packs)\n\n"
            "The launcher checks the files, downloads a compiler, rebuilds the game for Windows from\n"
            "your copy and gives you ng2.exe. No game files are included in this download.\n"
            "Keep the payload folder next to openNG2.exe.\n\n"
            "Updates: the launcher and the game check GitHub for a newer openNG2 release (one\n"
            "anonymous request; switch it off in the launcher or in Recomp Settings > System).\n"
            "Update now replaces the launcher and rebuilds the game from the files already\n"
            "installed - nothing to add again. Saves and settings are kept.\n\n"
            "Ninja Gaiden II belongs to Koei Tecmo; use only a copy you own.\n"
            "Third-party licences: THIRD-PARTY-NOTICES.txt\n"
            "Project page: https://github.com/KamiTK2/openNG2\n")

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

    write_notices(os.path.join(OUT, "THIRD-PARTY-NOTICES.txt"))
    total = sum(os.path.getsize(os.path.join(b, f)) for b, _, fs in os.walk(OUT) for f in fs)
    print(f"release folder: {OUT} ({total / 1e6:.0f} MB)")
    # Release archive: openNG2-<version>.zip with the folder inside.
    zip_path = os.path.join(os.path.dirname(OUT), f"openNG2-{VERSION}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, _, files in os.walk(OUT):
            for fn in files:
                full = os.path.join(base, fn)
                z.write(full, os.path.join("openNG2", os.path.relpath(full, OUT)))
    print(f"release zip: {zip_path} ({os.path.getsize(zip_path) / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
