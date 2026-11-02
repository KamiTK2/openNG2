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
- [x] Launcher `openNG2.exe` (launcher/, findings #68): native ImGui/D3D11 GUI; the player adds the game (GOD package C1200E9B558D04454FA6 + .data, .iso or extracted folder; default.xex SHA-256 checked), Title Update #2 (TU_1A461UL_000000G000000.00000000000G4; Title Update #3 and other games rejected with the reason) and optionally a DLC folder (the four packages listed by name); drag and drop or Browse
- [x] Toolchain without Visual Studio: LLVM 19.1.5, CMake, Ninja (pinned SHA-256) + Microsoft CRT / Windows SDK via xwin after the player accepts the licence; kept in <install>uild-tools (findings #67)
- [x] Extract the game, apply the TU, add DLC, codegen + build, install ng2.exe + DLLs into the install folder (ng2.exe finds game\ next to itself), remove the build files; VC++ runtime installed if missing; Play / Open folder / desktop shortcut
- [x] Release folder: tools/make_release.py -> dist/openNG2 (launcher + payload: our project sources + SDK, no game data); icon tools/make_icon.py
- [ ] Before release: licence review of the SDK's bundled third-party libraries (ffmpeg etc.) and a LICENSE / third-party notices file; test on a clean PC without Visual Studio; GitHub release upload
- [ ] Later: updates, and launching the game from the launcher

## Version 1.1: native renderer (user 2026-11-02)
Replace NG2's statically linked Xbox Direct3D layer with a D3D12 implementation (as Unleashed Recompiled does) instead of emulating the Xenos GPU: no PM4 interpretation, no EDRAM / ROV emulation, lower per-draw cost (weak machines). Estimate ~30-60 h of active work (findings #59-#63 for current costs):
- [ ] Feasibility (2-4 h): find the D3D functions, how NG2 uses them, whether it writes GPU commands directly
- [ ] Shaders: translate the 175 Xenos shaders to D3D12 ahead of time (Xenos -> HLSL translator)
- [ ] Core renderer: textures (tiled formats), vertex formats, constants, render states, render targets, resolves
- [ ] Scene-by-scene correctness (1120x584 scene buffer, effects, cutscenes, HUD)
- [ ] Port features: internal resolution, ultrawide, FSR, frame generation, HUD pinning, prompts

## Cheats (user request 2026-11-01)
- [x] Recomp Settings > Cheats: Unlock Difficulties, Unlock Costumes (apply after restart; nothing written to the save)

## Not selected
- In-game button prompt style swap (PlayStation/Switch icons)


