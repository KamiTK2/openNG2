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
- [x] Display mode, VSync, internal resolution 1x-3x (live), master volume
- [x] Monitor selection   - (reverted by user: Windowed / Borderless / Exclusive; keep Windowed / Fullscreen)   - [x] window size presets (windowed only), remember position/size
- (dropped: gamma / brightness: the game's own Screen Settings already has brightness/contrast)
- (removed by user: FPS counter row; the game always runs at 60. Frame rate is still logged every 5 s)
- [x] Mute in background   - [x] pause in background (XN_SYS_UI on focus loss, findings #41)
- [x] Save management: open save folder, back up saves   - [x] restore backup   - (dropped by user: import console save; the Cheats cover unlocks)
- [x] Controller: stick deadzone, invert camera X/Y, vibration strength (SDK input filter)   - (dropped: camera sensitivity, the game's Camera Settings has it)   - (dropped by user: remapping, the game has its own button config)

## Phase C: Medium
- [x] Anti-aliasing: FXAA / FXAA Extreme (live)   - [x] Upscaling: FSR 1.0 / CAS + Sharpness (live; helps only at 1x on large displays, findings #49)
- [x] Internal resolution and anti-aliasing apply live, no restart (GPU context rebuild, findings #50)
- [x] Framerate Cap 30 / 60 / 120 (Interpolation), default 60 (30 = display cap: NG2 is frame-locked, findings #30)
- [x] Keyboard & mouse: rebinding (Recomp Settings > Keyboard & Mouse), free mouse look with sensitivity / invert / auto-center (findings #51/#52)
      (dropped by user: mouse in menus / point and click; per-menu layout + selection RE for ~12 screens, too much work; the skull-marker cursor also crashed and was removed)   - [x] keyboard & mouse button prompts with the player's bindings (findings #54)   - [ ] Show Karma bind   - [ ] final-playthrough check: bow aiming with the mouse (camera mode unknown until the bow is obtained; the special-mode reset may need it exempted like first person)
- [ ] Audio: speaker setup (stereo/5.1), Music / SFX / Voice volumes if the mixer exposes them
- [x] Skip intro logos: System > Skip Intro (default Off) skips the NinjaVI movie and Team NINJA logo; title in ~4 s (findings #47)

## Phase D: Hard (game patches)
- [x] Ultrawide (Display > Aspect Ratio: Auto / 16:9 / 21:9 / 32:9, default Auto): Hor+ camera via the 16:9 constant, presenter stretch, HUD/menus/text/FMV kept at 16:9 (findings #31-#35)
      - [x] HUD pinned to the screen edges by render bucket (`ng2_hud_anchor`, default on); boss bar still to check
- [x] Aspect ratio selector + FOV slider (Display > Field of View 70-150 %), mouse wheel camera zoom (findings #56)
- [ ] Quality toggles: 16x anisotropic, disable motion blur / DoF / film grain, shadow resolution
- [x] Skip any cutscene (System > Skip Any Cutscene, default On): movies and real-time scenes, seen or not (findings #48)   - [x] reduce artificial load waits: System > Fast Loading (loading screens run unthrottled, findings #57)
      - [x] Shader precompile: the shipped manifest fills the shader / pipeline cache from the player's game image before launch, with a progress screen (findings #58); rebuild the manifest (tools/build_shader_manifest.py) after the final full playthrough
- [ ] Frame rate above 60 (interpolation or timing patches; speed-up risk, do last)
      - [x] Built-in frame generation 60->120: Graphics > Framerate Cap = 120 FPS (Interpolation); user: "feels great"

## Distribution: easy launcher (end goal, user 2026-11-01)
Players supply their own game; we never ship game data or game-derived code. A launcher:
- [ ] Require Title Update #2 (player-supplied TU_...G4 package, media 69F555CB): extract `default.xexp` next to default.xex; openNG2 is built from 0.0.2.4 only (findings #45/#46). Mission Mode works with it (user confirmed 2026-11-01)
- [ ] Pick the player's DLC packages too (copied to game\dlc; installed by the game on first launch, findings #43)
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


