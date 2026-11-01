# NG2 recomp: modern PC port roadmap

User wishlist from the 2026-10-31 questionnaire (everything below was selected).
Difficulty: Easy = runtime/app setting, Medium = runtime work or small game patch,
Hard = reverse-engineering + game patches.

## Phase A: Menu integration (done; settings save to `Documents\ng2\recomp_settings.toml`)
- [x] "Recomp Settings" entry in Options (user's choice over the main menu; replaces
      Copyright Information)
- [x] Settings live inline in the Options right-hand pane, game-style rows (no popup panel):
      A enters the pane, Up/Down rows, Left/Right values, B back to the list
- (dropped by user: copyright text page)
- [x] In-game menu entry (Back in gameplay): NINJA CINEMA row -> Recomp Settings, opens a full-screen panel (findings #42)

## Phase B: Easy settings
- [x] Display mode, VSync, internal resolution 1x-3x (restart), master volume
- [x] Monitor selection   - (reverted by user: Windowed / Borderless / Exclusive; keep Windowed / Fullscreen)   - [x] window size presets (windowed only), remember position/size
- (dropped: gamma / brightness: the game's own Screen Settings already has brightness/contrast)
- (removed by user: FPS counter row; the game always runs at 60. Frame rate is still logged every 5 s)
- [x] Mute in background   - [x] pause in background (XN_SYS_UI on focus loss, findings #41)
- [x] Save management: open save folder, back up saves   - [x] restore backup   - [ ] import console save
- [x] Controller: stick deadzone, invert camera X/Y, vibration strength (SDK input filter)   - (dropped: camera sensitivity, the game's Camera Settings has it)   - [ ] remapping

## Phase C: Medium
- [x] Anti-aliasing: FXAA / FXAA Extreme (restart)   - [ ] CAS sharpening / FSR
- [x] Framerate Cap 30 / 60 / 120 (Interpolation), default 60 (30 = display cap: NG2 is frame-locked, findings #30)
- [ ] Keyboard & mouse: full rebinding, mouse camera + sensitivity
- [ ] Audio: speaker setup (stereo/5.1), Music / SFX / Voice volumes if the mixer exposes them
- [ ] Skip intro logos

## Phase D: Hard (game patches)
- [x] Ultrawide (Display > Aspect Ratio: Auto / 16:9 / 21:9 / 32:9, default Auto): Hor+ camera via the 16:9 constant, presenter stretch, HUD/menus/text/FMV kept at 16:9 (findings #31-#35)
      - [x] HUD pinned to the screen edges by render bucket (`ng2_hud_anchor`, default on); boss bar still to check
- [ ] Aspect ratio selector + FOV slider (shares work with ultrawide)
- [ ] Quality toggles: 16x anisotropic, disable motion blur / DoF / film grain, shadow resolution
- [ ] Skip seen cutscenes / reduce artificial load waits
- [ ] Frame rate above 60 (interpolation or timing patches; speed-up risk, do last)
      - [x] Built-in frame generation 60->120: Graphics > Framerate Cap = 120 FPS (Interpolation); user: "feels great"

## Distribution: easy launcher (end goal, user 2026-11-01)
Players supply their own game; we never ship game data or game-derived code. A launcher:
- [ ] Pick the dump (GOD package, extracted folder or default.xex); verify the SHA-256 /
      title ID / media ID (notes/findings.md target table) and extract (tools/god_extract.py).
- [ ] Get the toolchain without manual setup: fetch ReXGlue v0.10.0 + apply
      patches/rexglue-sdk-v0.10.0.patch (or ship our prebuilt SDK runtime DLLs, which contain no
      game code), plus a portable clang/LLVM, CMake and Ninja, so no Visual Studio install is needed.
- [ ] Run codegen against the player's xex, build ng2.exe, copy runtime DLLs + ng2.toml.
- [ ] Progress UI, logs, retry; later: updates and launching the game from the launcher.
- Open questions: build time / disk (the recompiled C++ is ~560 MB, a full build takes minutes);
  whether the Windows SDK headers/libs can be fetched portably.

## Cheats (user request 2026-11-01)
- [x] Recomp Settings > Cheats: Unlock Difficulties, Unlock Costumes (apply after restart; nothing written to the save)

## Not selected
- In-game button prompt style swap (PlayStation/Switch icons)


