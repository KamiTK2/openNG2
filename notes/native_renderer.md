# openNG2 native Vulkan renderer: design notes

Status (2026-11-04): Vulkan renders gameplay, menus and cutscenes correctly with the native
options on (they are in `project/ng2/ng2.toml`); D3D12 ROV stays the default and is unaffected.
Branch `native-vulkan-renderer`, started 2026-11-03.

To try Vulkan: add `gpu_backend = "vulkan"` to `ng2.toml` next to ng2.exe (or pass
`--gpu_backend=vulkan`). Frame generation works on Vulkan too (#114); the 30 fps cap repeats every other frame on Vulkan (cadence not measured yet).

5x gameplay GPU time (#106-#112, #117): D3D12 ROV 8.7-9.1 ms; Vulkan 19.97 ms baseline ->
16.25 ms with `native_render_targets` + `native_resolves` -> 10.5-10.9 ms with
`ng2_msaa_supersample` (MSAA surfaces stored as the 1x surface the game reads them as; on in
ng2.toml). 4x: 10.9 -> 9.0 ms.
Evidence lives in `notes/findings.md` (#96 onwards); this file is the plan.

## Goal

Render NG2 with Vulkan without emulating the Xenos GPU's EDRAM:
- faster on weak GPUs (no ROV / interlock, no EDRAM dumps, no resolve -> guest memory -> texture reload chain),
- the same renderer later runs on Linux (Vulkan only, no D3D12),
- opt-in (cvar) until it matches the emulated renderer, which stays the default.

## What we learned (findings #96-#98)

1. NG2 links the XDK Direct3D library (TU2 `0x83758000-0x83773000`) with link-time code
   generation: library functions are specialised per call pattern (DrawIndexedVertices
   `sub_83762240` has the primitive type folded to triangle strip) and some are inlined into
   the engine. Hooking every D3D entry point (the Unleashed Recompiled way) would need every
   specialised copy found and kept in sync: fragile.
2. The D3D device is a GPU register shadow + dirty masks. Every draw's full state reaches
   the GPU as plain register writes, shader loads (IM_LOAD from memory), constant loads
   (LOAD_ALU_CONST) and a DRAW_INDX packet. The register stream *is* the D3D state.
3. ~2/3 of the GPU's draws are D3D pipeline drains (24 one-point draws after each GPR split
   change, static IB `1F037980`), not rendering.
4. The emulated cost is in EDRAM: ROV for every pixel, ~30 resolves per frame that write guest
   memory then get re-untiled by the texture cache (findings #61-#63, #82: texture loads +
   uploads ~2.3 ms of a 4.8 ms GPU frame at 1x).

## Architecture (chosen)

Keep the command stream as the *source of state* (cheap, on the GPU thread, game-agnostic,
covers every D3D path incl. inlined ones), but replace everything EDRAM-shaped:

```
guest D3D (unchanged) --PM4--> command processor (register file, packets)
                                   |
                                   v
                     NativeRenderer (Vulkan, new)
                       - render targets: host images keyed by
                         (EDRAM base, pitch, format, MSAA); no EDRAM buffer, no ROV,
                         native blending / depth
                       - resolve: image copy RT -> "resolved texture" keyed by guest
                         address + format + size; texture fetches of that address use
                         the host image directly (no guest memory write, no untile)
                       - drains: draws with no colour/depth effect are skipped
                       - shaders: SDK SPIR-V translator (host render target mode)
                       - textures / vertex data: SDK texture cache + shared memory
                         (static data still comes from guest memory)
                       - present: SDK Vulkan presenter (ImGui, Recomp Settings)
```

Game-specific knowledge (D3D map, #96) is used for diagnostics and for the cases the
stream can't express (e.g. CPU reads of a resolved texture: write back on demand).

## Milestones

1. [x] Vulkan backend builds and runs (emulated) - `--gpu_backend=vulkan` (#98)
2. [x] Emulated Vulkan path renders gameplay (black 3D was an SPIR-V exp_adjust bug, fixed, #103)
3. [~] Native render targets behind `--native_render_targets=true`: step 1 (no EDRAM transfers except clears / MSAA switches) correct on D3D12 and Vulkan (#102, #103)
4. [~] Native resolves -> host textures: step 1 on Vulkan (`--native_resolves=true`, 1x colour RTs, blit snapshot -> texture copy), scene buffer via MSAA unpack compute (#111); -2.0 ms at 5x; depth and k_16_16 not yet
5. [ ] Drain skipping, per-frame stats vs the emulated path
6. [ ] Scene-by-scene correctness: title, menus, gameplay, cutscenes, HUD, effects
7. [~] Port features on Vulkan: internal resolution, FSR / CAS, FXAA work; frame generation ported (#114); half-rate cap and ultrawide not checked
8. [ ] Linux build

## Where the time goes (5x Vulkan, #107)

A 5x gameplay frame is ~21 ms on Vulkan: EDRAM ownership transfers 8.5 ms (mostly the 1x <-> 4x
MSAA aliasing of the scene buffer and its depth/stencil), resolve -> texture copies 5.1 ms,
resolve/untile compute 3.0 ms, uploads 1.3 ms, the game's own draws 3.7 ms. NG2 renders its
scene into a 4x MSAA surface and reads the samples back as a 2x-wide/2x-tall 1x surface. The next big
step is storing 4x MSAA surfaces as supersampled 1x images so both views share one layout
(no transfers, and the scene-buffer resolve becomes a plain copy).

Done in #117 (`ng2_msaa_supersample`): MSAA draws are issued into the EDRAM-equivalent 1x surface
at twice the resolution along the sampled axes, so no MSAA host images exist and the MSAA <-> 1x
transfers are gone (5x 16.2 -> 10.5 ms). What remains above D3D12 ROV is to be measured again
(resolve -> texture copies, uploads).

## Testing

- `--gpu_backend=vulkan` selects the Vulkan backend (SDK cvar `gpu_backend`).
- Unattended gameplay route (Chapter 3 start) via `tools/menu_probe.ps1` with test settings
  (1x, windowed) swapped in and restored afterwards.
- `--ng2_ib_census=N` + `tools/pm4_decode.py` show what the GPU receives in a frame.
