# Truly native renderer: design

Branch `native-d3d-renderer`. Replaces `notes/native_renderer.md`, which describes the emulated
Vulkan path the user rejected. Evidence: findings #119-#121 and the three inventories in this folder.

## Rule

No PM4 decoding, no EDRAM model, no resolve to guest memory, no render target or texture cache
keyed on EDRAM, in the native path. Anything that needs one of those is a design failure to report,
not something to work around quietly. D3D12 stays the default and the baseline every result is
compared against. The native path is opt-in (`ng2_native_d3d`, default off).

Still Xenos-aware, by necessity: the guest device struct holds raw register values and fetch
constants, shaders are Xenos ucode, and textures are tiled. These are translated, once, as data
formats. That is the Unleashed Recompiled model.

## Shape

```
engine code (recompiled, unchanged)
  |  state: library setters + inlined shadow writes, all land in the guest device struct
  v
guest D3D device struct (kept as the source of truth; never reaches a GPU as PM4)
  |  replaced entry points read it at draw time
  v
native layer (project/ng2/src/native/)
  draw / begin-vertices / clear / resolve / swap / ring + fence functions
  -> pipeline cache   (state hash -> VkPipeline; shaders translated from ucode)
  -> surfaces         (D3D surface pointer -> VkImage, no EDRAM addresses)
  -> resolve          (image copy into the VkImage of the destination D3D texture)
  -> textures         (fetch constant -> untiled, byte-swapped VkImage, loaded once)
  -> geometry         (guest VB/IB bytes -> host buffers, endian swap in the vertex path)
  v
Vulkan, presented through the SDK presenter (ImGui and Recomp Settings keep working)
```

## Replaced functions

| Group | Functions | Native behaviour |
|---|---|---|
| Draw | `83762240` DrawIndexedVertices, `83761E58` DrawVertices, `83761488` BeginVertices, `83761928` BeginIndexedVertices | The library function runs first (it flushes dirty state into the device and patches vertex shaders; its packets are never read), then a Vulkan draw is recorded from the device. `83761928` is not drawn yet |
| Immediate vertices | 72 inline Ends have no hook | The draw is recorded at Begin; the bytes are copied to the heap at the next D3D entry point |
| Targets | `8375E290` Clear, `83762CA8` Resolve | Vulkan clear, image copy |
| Present | `8376A9F8` Swap | The library's Swap still runs for its frame bookkeeping; afterwards present, then wait for the flip (the only frame pacing left, #123) |
| Ring | `8375F428` segment kick, `8375FCE0` InsertCallback, `837613A0` SetShaderGPRAllocation | Kick dropped (the one place that reaches the primary ring), callbacks run at once on the calling thread, drains disappear. `8375FB08` KickOff stays: it recycles segment memory |
| Fences | `8375F7C0` emit fence, `8375FF08` BlockUntilIdle | The fence and read position are stored in the write-back block when emitted, so `dev+10908` and `*(dev+10896)` stay coherent and BlockOnFence `8375FC08` needs no change. Real host fences join in when M2 submits work |

Everything else in the library stays and acts as a state store. Setter dirty masks are ignored.
The library's internal helpers can still write into command segment memory; those segments are
never submitted or read.

## Milestones

Each ends in a run through `tools/native_test.ps1`, reported against D3D12.

- **M0** cvar and scaffolding: with it off, nothing changes (same log, same frame time).
- **M1** done (#123): boots to gameplay with an empty ring, 60 fps, no fence hangs.
- **M2** done (#124, #125): shaders from the XEX to SPIR-V, running on real data.
- **M3** done (#122, #125): textures load from fetch constants; menus and HUD correct.
- **M4** done (#125): render targets and resolves, the pass graph renders, gameplay frame on screen.
- **M5** working in the tested scene (#125): patched vfetch, bone palettes, skinned characters.
- **M6** in progress (#126-#135): gamma ramp, MSAA as one pixel per sample, menus, movies, New Game cutscene, six Mission Mode levels. Open: story chapters past 1, Mission Mode flame blur, the 2x depth clear mechanism.
- **M7** done on the tested scenes (#127, #132, #133): live internal resolution, FXAA, FSR, CAS, frame generation, ultrawide; library draw functions and the backend context gone. Open: 30 fps cap cadence not measured, fullscreen picture not captured by a tool.

## Known risks

- GPU timing stamps from `sub_837EBDC8`: what reads them is unknown and they may drive the MSAA
  choice (#101). First approach: keep real host timestamps, not constants.
- Stream binding in gameplay is unknown (0 `SetStreamSource` calls). Must be solved for M2:
  read the vertex fetch constants from `dev+1152`.
- 70 VS with placeholder vfetches need (VS, declaration) pairs.
- 7e3 and -32..32 targets have no direct host format: use R16G16B16A16_SFLOAT and match in
  the translated shader output.
- Luminance chain depends on clear-via-transfer side effects (#102): must be made explicit clears.
- About 2/3 of the game shaders have never been run under test.

## Default renderer (2026-11-05, #149)

Native Vulkan is the default: `ng2.toml` sets `gpu_backend = "vulkan"` and `ng2_native_d3d = true`. Recomp Settings > Graphics > Renderer writes `gpu_backend` to the player's settings and applies at the next start; a file without the key gets Vulkan. A PC with no Vulkan 1.3 device starts on D3D12. The shader pack is game-derived, so releases can't ship it: `EnsureShaderPack` builds it from the loaded image before the game starts, and shaders it can't hold are translated at bind time. After a translator change, bump `kPackVersion` so players' packs rebuild.

## Status for the morning (2026-11-05, findings up to #147)

To run it (now the default): `--gpu_backend=vulkan --ng2_native_d3d=true`. D3D12 is unaffected with `--gpu_backend=d3d12`.

**Done**
- Game runs with no PM4 reaching the command processor and no EDRAM: gameplay, menus, movies, the New Game cutscene, Mission Mode.
- Image against D3D12: courtyard 0.41-0.50/255, post-movie 2x session 0.65/255, 4x 0.66/255.
- Performance at true 7x (the fair test; D3D12 is capped at 7x by the SDK, so its earlier "8x" was 7x, #148): native 6.07 ms at 60 fps, 40% GPU, 144 W; D3D12 16.71 ms at 56 fps, 100% GPU, 225 W. Native at 8x: 7.4 ms, 60 fps, 47% GPU. Native at 10x: 12-22 ms, 59-60 fps, 8.2-10.2 GB VRAM. At equal clocks in a light scene: 1.37 ms against 5.39 ms. At 5x the same 60 fps takes 87 W against 220 W.
- All shaders work: 634 precompiled, anything else is translated on first use, so a missing shader no longer drops geometry.
- All 52,505 disk textures load; signed formats, sampler settings, reload and freeing are in.
- FSR, CAS, FXAA, ultrawide, frame generation, live resolution change, the Recomp Settings pane.
- River in the New Game cutscene: matches D3D12 by eye on a frozen frame, with the same camera. The cause was swapped Clear flags: 0x10 is depth, 0x20 is stencil (#146).
- 2x depth clear: no workaround left. The same flag fix is the true rule (#146).
- 30 fps cap: even, 33.33 ms on average with 0 of 240 frames off by more than 2 ms, same as D3D12 (#147).
- Validation layers clean on menu, gameplay and cutscene. 3-minute runs clean in gameplay and cutscene.

**Partly done**
- Story coverage: 44 of 58 stage ids load and render on both renderers with no misses and no dropped draws. They can't be reported by chapter (#145): only stage 9 = Ch. 1 and stage 0xB = Ch. 3 are established. These runs start a stage with the player falling in, so they are not a playthrough.
- PM4 removal: the draw functions are gone; `sub_83768758` and the library's Swap still write small packets into memory nothing reads (0.06 ms per frame), and the idle command processor thread remains. Removing them means reimplementing vertex shader patching.
- VRAM: 2936 MB against D3D12 2662 MB at 4x; 4975 MB against 5003 MB at true 7x.

**Not done or unknown**
- Fullscreen on native has not been seen in a capture: screenshots are black on any Vulkan fullscreen. Only the 60/120 present rates are confirmed.
- Mission Mode flame blur: no repeatable comparison exists, so it isn't known whether it matches.
- River in the post-movie 2x session: viewed on native (run mv-nat), bridge and water render with a red reflection under the post; D3D12 was on a different shot at that moment, so it is not compared.
- 14 stage ids stall on both renderers (#145): unknown whether they are unused.
- Cutscenes are compared by eye: their camera can differ between renderers at the same frame number, and falling petals always differ.
