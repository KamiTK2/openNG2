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
- **M6** in progress (#126-#130): gamma ramp, MSAA as one pixel per sample, menus, movies, New Game cutscene, Mission Mode. Movies work through the guest decoder and texture reloads. Later chapters not covered.
- **M7** internal resolution done (#127); then port features (FSR, frame generation, ultrawide).

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
