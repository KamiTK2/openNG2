# Native renderer: open questions

Read-only research, 2026-11-05. TU2 addresses. No game runs: evidence is static code (`tools/show_func.py`,
image floats from `ghidra-db/image-tu2`) and existing logs. `ng2_trace_gp.txt` = gameplay trace, 602 swaps.
Status per claim: observed / inferred / unknown.

## 1. GPU timing stamps

**Correction to `d3d_inline.md` section 1 and finding #120 (observed):** `sub_837EE0A8`, `sub_837EE2F0`
and `sub_837EE690` are not stamp readers. They read +1280 / +1300 on a different object, as counts of
index lists, and upload VS/PS float constants (`83760930`, `83760A18`, constants 71+, 75+, 96+, 100+,
108+, 112+). Call rates of 31k-100k in the trace (per object, not per frame) fit per-mesh lighting
constants (inferred). The offset match was a coincidence.

**Real readers (observed):** `sub_837EC388` (ring `renderer+1300`) and `sub_837EC478` (ring
`renderer+1296`), renderer = `0x859EEB64`, called once per frame by `sub_822F4010`.

| Ring | Written by | Meaning |
|---|---|---|
| `+1296` | `mftb` (`83833DA0`) in `sub_837EBDC8`: slot `[65+i]` before Swap, `[1+i]` at the end | CPU frame time (inferred) |
| `+1300` | GPU callback `837EB110` via `sub_8375FCE0`: type 0 = `[65+i]` before Swap, type 1 = `[1+i]` at the end | GPU frame time (inferred) |

Each ring has 64 slots (mask 0x3F) and an index at +0. Each reader walks back to the newest pair with
`[1+i] < [65+i]`, or returns 0.

`sub_822F4010` converts to percent of budget and publishes at `0x83C07200` (observed):

| Address | Value |
|---|---|
| `+0` | CPU % = delta / (timebase * 0.0166834) * f * 100 |
| `+4` | 6000 / CPU %, clamped to 60 |
| `+8` | 6000 / GPU %, clamped to 60 |
| `+12` | GPU %, same formula |

`f` comes from a table on the present interval: 1, 0.5, 0.333, 0 (inferred from the table setup).

**What the values drive (observed unless marked):**

| Reader | Test | Effect |
|---|---|---|
| `sub_82333BF8` | GPU % > 110 for 20 frames, or < 90 for 20 frames (with a bypass by scene state) | Fades float `0x83C11108` between 1 and 0 over 15 frames (`+4232` = state). `sub_82333218` multiplies a PS constant by it. The effect is unidentified |
| `sub_836402E0` | GPU % < 98 (`0x821B1CC4`) | Runs `8363DE88`, `8363ED90`, `8363EE58`. They ran 602 of 602 frames in the trace, so the test passed on the emulated path |
| `sub_822F2420` | (CPU % < 100 and GPU % > 150), or (`822F41B8` >= 100 and CPU % > 110) | Returns 1, stored at `0x83C07880`. A one-frame overrun flag (the next call clears it). `sub_822F16D0` tests it three times and skips rendering work when set (`836732C0`, `8233D740`, `8247EB08`, `83648BA8` and others), so this is a frame skip (inferred) |

So the game has a load-based quality fade and a frame skip. Which effect `0x83C11108` scales is unknown.

**MSAA (#101): not driven by the stamps (observed).** No reader above touches MSAA. `sub_822F0B40`
(0 = 1x, 1 = 2x, 2 = 4x) is called only from `sub_8365F640`, the movie function, matching #104.

**Native path.** M1 and #123 say GPU stamps report almost no GPU time. Static reading disagrees
(inferred, not measured). `sub_8375FCE0` runs both callbacks at once, so type 1 stamps at the end of
frame N's wrapper and type 0 in frame N+1's wrapper. The delta is therefore the CPU work between
presents, excluding the swap wait, so GPU % should roughly equal CPU %, not zero. Either way the
reported value is not GPU time.

**Consequences:**
- Below 90%, behaviour matches the emulated path as traced, which is the normal case on fast machines.
- A CPU-heavy scene over 110% would fade the effect, and over 150% could not reach the frame skip's first test (it needs CPU % < 100), for a CPU reason. A GPU-heavy frame never would trigger either.
- Any value of `0x83C07200+12` above 98 changes what runs, so don't report a raw number at 5x without
  checking which effects it turns off.

**Recommendation:** log `0x83C07200` +0..+12 and `0x83C11108` for one run on each path. Then report
the Vulkan timestamp query result: write a synthetic `[1+i]` / `[65+i]` pair whose delta equals the
measured GPU time, keeping the ring format. A cvar that pins GPU % to 50 is the safe default until
the effect is identified, because a PC port should not drop an effect on load the way a console did.

## 2. Movies

**Format (observed, from ASF headers of the 29 `.wmv` files, 213 MB):** 26 WMV3, 3 WVC1 (`NinjaVI_Left`,
`NinjaVI_Right`, one other). Audio tags 0x161 (WMA) and 0x162 (WMA Pro).

**Player (observed unless marked):**

| Function | Role |
|---|---|
| `sub_83660C30(index)` | Entry from movie scene `sub_83660E68`; routes to the two players (#47) |
| `sub_83660158` | Single stream player |
| `sub_836602D0` | Side-stream player (three streams), creates textures via `83759568` |
| `sub_8365F640` | Per-frame function; calls render, sets MSAA on stop via `sub_822F0B40` |
| `sub_83660D08` | Stop and release: `83888060`, `83887F58`, clears handle |
| `sub_83661218`, `sub_836614B0`, `sub_83661750` | Audio volume updates through `8388D3B8` |
| handle | `0x84C557FC` |

Decoder boundary (COM-style object at the handle, vtable; XDK names inferred, IXMedia2XmvPlayer shape):

| Function | Vtable | Role |
|---|---|---|
| `sub_83888C70` -> `sub_838885A8` | - | Create player from file path (r3=dev, r4=path, r5=out); nonzero = fail |
| `sub_83887F78` | +80 | RenderNextFrame (r4=0, r5=0): decodes and draws. Return goes to r18 |
| `sub_83888EE0` | +92 | Called once after setup (start, inferred) |
| `sub_8388D3B8` | +120 | Gets the audio object; `+92` of that sets volume |
| `sub_83887F88` | uses +116 | Sets the output rect |
| `sub_83888060`, `sub_83887F58` | - | Stop, release |

**How frames reach the screen (inferred):** the decoder draws directly with D3D on the current target,
through the 65 WMV shaders (64 use memexport, per #121), and uses multi-pass residual, deblock and
mocomp. The side-stream player routes to textures and `sub_83660FC8` quads. **Under the native path
this is untested, and the memexport shaders can't work** (no memexport in the native contract, #124).
Expect a black or broken movie, or a hang.

**Host decoder (observed):**
- SDK FFmpeg source tree has `libavcodec/vc1dec.c` and `libavformat/asfdec_f.c`, but the SDK build only
  compiles WMA Pro and `codec_list.c` lacks VC-1 and WMV3 (`thirdparty/CMakeLists.txt`). Nothing usable
  is built today.
- `xam_video.cpp` hits for XMV are unrelated to playback (not read in full; inferred).
- Windows Media Foundation is an alternative, but Windows-only, which conflicts with the Linux goal.

**Smallest clean replacement:** strong overrides at the decoder boundary, not in the game's player:
1. `83888C70` / `838885A8`: open with FFmpeg, return a fake guest object whose vtable points at host stubs.
2. `83887F78`: upload the next decoded frame, draw one native quad on the current target, return the
   same status codes, including end of file. **The EOF code must be read from the real
   implementation first.**
3. Audio: decode WMA to the SDK audio path. A silent first pass is acceptable, with a guest stub for
   the `8388D3B8` object.
4. Keep the MSAA call on stop (`sub_822F0B40`), since it runs in the game code unchanged.

Cheap first step: reuse the Skip Intro failure path (#47) in native mode, so movies skip cleanly and
the game doesn't hang. That unblocks the rest while the decoder is built, but it changes the MSAA
result, because a failed open keeps 1x (see section 4).

## 3. PM4 leftovers

Draw functions (`83762240`, `83761488`, and `83761E58`, `83761928` by the same shape) flush in this
order (observed): `837679C0` x2, `83768758`, `83767288`, `83767620` x8, `83767860`, `837676E0`,
`8375FB08`, packet.

Stores in each helper, excluding the ring (observed):

| Function | /frame | Native needs? |
|---|---|---|
| `837679C0` constants | 780 | No: shadows already in the device |
| `83767620` register blocks | 1730 | No |
| `83767860`, `83766FD0` fetch and shader load | 534 | No; only ring cursor +48 stored |
| `83767288` | 45 | No; ring only |
| `837676E0` | 1 | No; ring only |
| `83768568` | 972 | No; ring only |
| `83767088` | 529 | **Yes**: writes `+10560` (RB_DEPTH_INFO, per d3d_api) and `+10941`. Read what native depends on |
| `83768758` | 534 | **Partly**: calls `83768618` -> `83767B20`, the VS ucode patch, per declaration |
| `837683F8` | - | **Yes**: stores `+11824`, `+11832`, `+10942`, read by the patch path; also ring |
| `83768618` fence check | 150 | Compares `+64` with fences. Native fences complete at once, so it stays true |
| Dirty masks +0..+40 | - | Each stage clears them. Native must clear or leave them consistently |

**CPU cost (inferred upper bound, not measured).** Trace counts times static function length, one pass
of each body: 1.42M PPC instructions/frame across the whole D3D library, of which about 0.72M is the
draw flush (`83768758` 276k, `83762240` 130k, `83767620` 83k, `837679C0` 68k, `83767088` 68k, `83767860`
47k, `83768568` 43k). Copy loops can exceed one pass, and skipped branches go the other way. As a
guess this is a few tenths of a millisecond per frame. Measure with `diag_cpu_prof` scopes before
spending time on it.

**To remove PM4 entirely:**
1. Replace `83768758` with a native version: run the VS patch via `83768618` and `837683F8` state, and
   clear the dirty bits.
2. Replace the draw functions so they stop calling the flushers: keep `83767088`'s `+10560` update.
3. Replace `837679C0`, `83767620`, `83767860`, `837676E0`, `83767288`, `83768568` with no-ops once 1
   and 2 are done. They only write the ring, but all external writers must be checked first.
4. `8376A9F8` Swap still writes VdSwap's packet (`ng2_native_d3d.cpp`), and `83759AA8`, `83759C68` run
   ring work: review them last.
5. BeginVertices relies on the library ring pointer (#125): it needs real scratch space back before
   step 2, because the immediate data lives in the ring segment.

Then the idle command processor can be removed from the native path.

## 4. Remaining unknowns

**Predicated tiling (observed unless marked).** `sub_838048E8` runs every frame from `822F07C8` (602),
but only calls BeginTiling `83771E60` when object `*(renderer+1320)` field +16 is set; +20 requests a
toggle. `83771E60` has 0 calls in the gameplay trace. `sub_83804A98` runs every frame (602) with
EndTiling `837721D0` behind the same kind of gate. What sets +16 is unknown.

- `83772688` (called only from `838048E8`, after BeginTiling's clears): stops the ring, sets bit 64 in
  `+10940` and `+10941`, stores a tile count derived from `+10548` into `+12696`. Starts the recorded
  command segment replayed per tile (inferred).
- `83772918` (called only from `822F16D0`, gated by field +24, then cleared): leaves tiling mode,
  calling `83772A60`, `83772AE0`, `83772BC8` (inferred).

Native neither overrides them nor has run them. If tiling ever turns on, the replay relies on PM4.
**Check +16 at runtime after a movie has played**, since tiling exists to fit MSAA in EDRAM. Native
should make BeginTiling a plain Clear, EndTiling plain Resolves, and both others no-ops.

**Cutscenes vs gameplay, from `notes/logs/cs-fsi-census.log` (observed, 2 frames of the New Game
opening; mode 5 = depth only):**
- Scene surface is `11810460` (**2x MSAA**), 186 of the 260 RT binds. Gameplay under native has 0 MSAA surfaces.
- Depth-only passes: `0A020280` with depth `000002D0`, plus `10000410`, `08020208`. Cubic or shadow
  depth variants, and #113's EDRAM wrap case at 0x2D0.
- Composite at `14000500` (1x), no HUD pass, front buffer after it.
- 36-37 resolves per frame against 29, with 24 of the 74 resolves at `00800050` (80 wide).

**Largest risk from this section (observed + inferred):** **native tests have never seen MSAA,
because the test route skips the movie.** `sub_822F0B40(1)` runs when a movie ends. A real player who
watches any movie ends up on 2x, including the D3D12 path (#101, #104). Native ignores the sample
count, and the 2x reads of #111 and #108 (samples read as a double-size 1x surface) are not handled.
Cutscenes and post-movie gameplay are likely wrong until this is addressed.

**Not available in logs:** other chapters, Mission Mode, refraction passes, and a runtime confirmation
of who sets the tiling flag.

## Next steps, in order

1. Run native with the opening movie allowed (or call `sub_822F0B40(1)` in a test cvar), and implement
   2x as the `ng2_msaa_supersample` mapping did: render the MSAA surface as its double-size 1x surface.
2. Add a native route for the New Game opening cutscene (the census frames above are the reference).
3. Clean movie skip in native mode, then FFmpeg VC-1 + ASF build and boundary overrides.
4. Log `0x83C07200` on both paths, then synthesise timing stamps from Vulkan timestamps.
5. Remove the PM4 flushers after 1-3, with a CPU measurement before and after.
