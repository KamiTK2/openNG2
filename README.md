# openNG2: Ninja Gaiden II (Xbox 360) native PC port

Goal: a native Windows build of the **original 2008 Xbox 360 Ninja Gaiden II**,
made by statically recompiling the game's PowerPC code to x64 C++. No game data
is included in this project or in anything it ships. **Every player supplies
their own dump.**

## What's in this folder

| Path | What it is |
| --- | --- |
| `Setup.bat` / `setup.ps1` | One-time installer for the whole toolchain (safe to re-run) |
| `env.ps1` | Written by setup. Dot-source it in each new terminal: `. .\env.ps1` |
| `game\` | **Your** NG2 dump goes here (never committed) |
| `project\` | The recomp project (ReXGlue app) is created here in Phase 2 |
| `tools\` | Ghidra, XEXLoaderWV, and the ReXGlue SDK source/build |
| `reference\ninjas-dawn\` | TheSaltTrader's existing ReXGlue NG2 recomp, kept for reference |
| `notes\findings.md` | Evidence ledger: what's known, how we know it, what's open |
| `skills\reverse-engineer-anything\` | The REA skill you supplied (`rea setup` also installs it into Claude Code) |

## The toolchain, and what each piece is for

- **ReXGlue SDK.** The core of the work. It turns `default.xex` into C++ and
  supplies a runtime (kernel, memory, threading, VFS) based on Xenia.
- **Ghidra 12.1 + XEXLoaderWV.** Loads the XEX so you can read the PowerPC code:
  finding function boundaries, jump tables, and what imports do.
- **REA (Reverse Engineer Anything).** The AI-agent reverse-engineering toolkit
  (`rea-agents` on npm). **Important limitation:** REA 6.8 has no XEX loader.
  Its Windows Ghidra mode only accepts x86/x64/ARM PE files, and its PowerPC
  support covers only 32-bit PPC ELF and GameCube DOL, on Linux/macOS hosts.
  That means REA **cannot read `default.xex` directly**. In this project it's
  used for:
  - analyzing **our own recompiled x64 `.exe`** (crashes, function matching,
    comparing builds, PE imports and load config)
  - the evidence-ledger workflow (observed / inferred / unknown) in `notes\`
  - any Windows-side artifacts (DLLs, shader binaries, packaged builds)
- **Visual Studio 2022 Build Tools + clang-cl, CMake, Ninja.** The compilers
  ReXGlue requires.
- **Node.js 24, Git, Python, Temurin JDK 21.** Prerequisites for REA, Ghidra and
  the build.

## Phase 0: install

1. Double-click **`Setup.bat`**. Approve the Windows UAC prompts. The Visual
   Studio download is about 10 GB.
2. If it says Visual Studio or npm isn't found yet, **reboot and run
   `Setup.bat` again**. It skips the steps that already finished.
3. Open a new terminal in this folder:
   ```powershell
   . .\env.ps1
   rea setup      # choose Claude Code, review the changes, approve
   rea doctor     # confirms REA, Ghidra and the JDK are ready
   ```

## Phase 1: dump and identify your copy

1. Extract your disc into `game\` (see `game\README.txt`).
2. Record the region, title update and `default.xex` SHA-256 in `notes\findings.md`.
3. In Ghidra: *File → Install Extensions*, add the XEXLoaderWV zip from
   `tools\ghidra-extensions`, restart, then import `game\default.xex`. Note the
   entry point, the base address, and the kernel/xam import list.

## Phase 2: decide on your starting point

**Option A (recommended): build on Ninja's Dawn.** `reference\ninjas-dawn` is
already a working ReXGlue recomp of NG2, with keyboard input, save import, DLC
support and a settings menu. Building it first gives you a known-good baseline,
and you can then improve rendering, fix bugs, or add features. Check its
licence before you redistribute anything based on it.

**Option B: start from scratch with ReXGlue.** Best for learning, but expect
weeks of work.
```powershell
. .\env.ps1
cd project
rexglue init --app_name ng2 --app_root .\ng2
# edit ng2\ng2_config.toml -> file_path = "..\\..\\game\\default.xex"
cd ng2
cmake --preset win-amd64-debug
cmake --build --preset win-amd64-debug --target ng2_codegen
cmake --build --preset win-amd64-debug
```
Unresolved functions are expected on the first codegen run. Work through them by:
- adding function boundaries and jump tables to `ng2_config.toml` (check each one in Ghidra)
- implementing missing kernel/xam imports
- using `--force` to get output anyway while you iterate

## Phase 3: make it a real port

Renderer (Xenos → D3D12/Vulkan, shader translation), XMA audio, XInput and
keyboard mapping, save paths, frame pacing/60 fps, and a first-run screen that
asks the player to point at their own dump. Use REA on the built `.exe` to
investigate crashes and regressions, and record the results in `notes\findings.md`.

## Legal

Only use a copy of NG2 you own. Don't commit, upload or distribute game files,
decrypted XEX output, or generated code that embeds game data.

## What this repository contains (and doesn't)

This repo holds only our own work: the ReXGlue app (`project/ng2/src`, manifest, config),
our changes to the ReXGlue SDK as a patch, scripts, and the research notes.

It does **not** contain, and must never contain:
- any game data: `default.xex`, the game's files, saves, or anything extracted from them;
- game-derived code: the recompiled C++ (`project/ng2/generated/`) or built executables;
- dumps, disassembly, screenshots or logs (`notes/logs/`, `ghidra-db/`, `snapshots/`);
- third-party tools (Ghidra, the ReXGlue SDK itself, the reference project).

`.gitignore` is deny-by-default: only allow-listed paths can be committed.

**You must supply your own legally obtained copy of Ninja Gaiden II (Xbox 360).** Put your
dump in `game\` (see `tools/god_extract.py` for Games on Demand packages).

## ReXGlue SDK patches

The port needs fixes and additions in the ReXGlue SDK (ring-buffer reset race, audio,
presenter frame generation / aspect / VSync, guest vblank timing, input filters, file-read
hook, ...; each is marked `NG2 FIX` / `NG2 ADDITION` / `NG2 DIAG` in the source). They live
in `patches/rexglue-sdk-v0.10.0.patch`, made against ReXGlue **v0.10.0**
(commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`):

```
git clone https://github.com/rexglue/rexglue-sdk.git tools/rexglue-sdk
cd tools/rexglue-sdk
git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
git submodule update --init --recursive
git apply ../../patches/rexglue-sdk-v0.10.0.patch
```

Then build the SDK from a VS 2022 x64 developer prompt (`cmake --preset win-amd64`, build the
RelWithDebInfo config, `cmake --install`), run codegen for `project/ng2` against your own
`default.xex`, and build the project.
