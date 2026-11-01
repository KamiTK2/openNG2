# openNG2

A native Windows PC port of **Ninja Gaiden II** (Xbox 360, 2008), made by statically
recompiling the game's PowerPC code to x64 with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

**No game data is included.** You need your own copy of the game.

## Features

- Runs the full game: menus, saves, gameplay, cutscenes, audio
- Mission Mode and DLC costumes (with your own Title Update #2 and DLC)
- Ultrawide (21:9 / 32:9) with the HUD pinned to the screen edges
- 120 FPS frame generation, up to 3x internal resolution, FXAA
- **Recomp Settings** in the game's own Options menu and in the in-game (Back) menu:
  display, graphics, audio, controls, save backup/restore, Skip Intro, Skip Any Cutscene,
  cheats (unlock difficulties / costumes)

## What you need

- Your dump of the Xbox 360 disc (`default.xex` and game files, media ID `69F555CB`)
- **Title Update #2** for that disc (`TU_1A461UL_000000G000000.00000000000G4`)
- Optional: your DLC packages (Mission Mode, costumes)
- Windows 10/11, Visual Studio 2022 Build Tools (clang-cl), CMake, Ninja, Python

## Build

1. Run `Setup.bat` once to install the toolchain.
2. Put your game files in `game\` and your DLC packages in `game\dlc\`.
3. Extract the title update next to `default.xex`:
   `python tools\stfs_extract.py <TU file> game` (creates `game\default.xexp`).
4. Get ReXGlue v0.10.0 and apply our SDK patch:
   ```
   git clone https://github.com/rexglue/rexglue-sdk.git tools/rexglue-sdk
   cd tools/rexglue-sdk
   git checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
   git submodule update --init --recursive
   git apply ../../patches/rexglue-sdk-v0.10.0.patch
   ```
   Build and install it from a VS 2022 x64 prompt (`cmake --preset win-amd64`,
   RelWithDebInfo, `cmake --install`).
5. Build the game: `project\ng2\build.cmd`. Run `ng2.exe` from
   `project\ng2\out\build\win-amd64-relwithdebinfo` with `--game_data_root=<path to game>`.

An easy launcher that does all of this from your files is planned (`notes/roadmap.md`).

## Repository

| Path | Contents |
| --- | --- |
| `project/ng2/` | The app: hooks, Recomp Settings, codegen manifest |
| `patches/` | Our changes to the ReXGlue SDK |
| `tools/` | Scripts (address mapping, STFS/GOD extraction, test runs) |
| `notes/` | `findings.md` (research ledger) and `roadmap.md` |

## Legal

Only use a copy of Ninja Gaiden II you own. This repository never contains game files,
saves, recompiled code or built executables, and you must not share them.
