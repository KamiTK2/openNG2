# Engine code that bypasses callable D3D functions (TU2)

Scope: guest code outside the XDK D3D library, `0x83758000-0x83773000`. Read-only static analysis
of the generated disassembly (`project/ng2/generated/default`) plus trace counts. No game runs.
Status tags: observed / inferred / unknown. Per-frame rates come from
`ng2_trace_gp.txt` (602 swaps, Chapter 3 route): 30 of the 103 functions below were hit there, so
other scenes are not covered.

## Verdict

**The no-PM4 design is possible; nothing found makes it impossible.** Only one engine function
writes PM4 into the ring, and it is small. The much larger bypass is 100+ engine functions
writing the device's state shadow directly, which rules out implementing state setters as the
only source of truth, but not the design: **keep the guest device struct as the state store and
read it at draw time.**

What that costs (inferred): the shadow holds raw Xenos register values and fetch constants, so
the native layer must still interpret those formats (for example RB_*, PA_*, texture fetch words)
and translate Xenos ucode. There is no way around that with this inlining. It is format
translation, not PM4 / EDRAM emulation, but it should be stated openly against the "no Xenos
emulation" goal.

## Summary

| Class | Sites | Treatment | Status |
|---|---|---|---|
| Direct PM4 ring writer | 1 function (`sub_837EBDC8`) | Replace `sub_8375FCE0`; keep ring memory as a dummy | observed |
| Ring list false positives | 12 of 13 engine entries in `notes/d3d_ringwriters.txt` | None | observed |
| Inlined EndVertices (`dev+48 = dev+13428`) | 72 sites in 52 functions | No override; defer the draw | observed (sites), inferred (fix) |
| Inlined shadow writes + dirty bits | at least 99 functions | No override; read shadow at draw | observed (lower bound) |
| Engine calls to internal ring helper `sub_8375FB08` | 2 sites | Replace the callee | observed |
| Fence values read by engine (`dev+10908`, `+10912`) | 2 functions | Keep fields coherent | observed |
| GPU timing callback | 1 site | Run callback natively | observed (site), unknown (consumer effect) |
| Vd* / interrupts | All inside the library | Replace library functions; keep vblank | observed |
| Occlusion queries | None in the packet search | None | observed (static) |
| Memexport | 0 of 181 manifest shaders | None seen | observed for those shaders |
| CPU reads of resolved textures | None known | Keep a write-back fallback | inferred |

Coverage: device tracking follows pointers from D3D call arguments (mostly renderer object
`0x859EEB64` +92 = device) through registers and call arguments. It found 99 functions, but the
textual EndVertices match found 4 more it missed (`sub_837A47E8`, `sub_837A53C8`, `sub_838033A0`,
`sub_83803418`), so **99 is a lower bound** and those 4 may hold unlisted shadow writes.

## 1. PM4 ring writers

### `sub_837EBDC8` (present wrapper, 1 per frame): the only real one (observed)

Called from `sub_822F16D0`. Around `0x837EC170`: loads cursor `dev+48` / limit `dev+56`, calls
`sub_8375FB08` if over, calls `sub_8375FCE0(dev, cursor, 0, 0x837EB110, slot)`, stores the result
to `dev+48`, then takes a CPU timestamp, then calls `sub_8376A4C8` and swap `sub_8376A9F8`.

- `sub_8375FCE0` (in the library) emits register writes and a type-3 `C0043C01` (WAIT_REG_MEM),
  with a GPU callback; `0x837EB110` only stores a QPC value to `*r3`.
- Slots are in a stamp ring at renderer object `+1296` / `+1300`, read by `sub_837EE0A8`,
  `sub_837EE2F0`, `sub_837EE690` (reached from `sub_837F3928`, `sub_837F36B8`, `sub_837F3500`,
  `sub_837F3E98`).
- Same function also reads `dev+10908/+10912/+13500..13512/+16712`, which looks like the library's
  deferred-callback list (inferred).

**Recommendation: targeted override.** Replace `sub_8375FCE0` to return the cursor unchanged and
invoke the callback from the native frame-complete point. Leave `sub_837EBDC8` itself alone.

Risk (unknown): what the stamps drive. Finding #101 left open why NG2 picks 1x or 2x MSAA, with
measured GPU timing as a candidate. Fake stamps could change that decision.

### `notes/d3d_ringwriters.txt` engine entries (12 false positives, observed)

`sub_822F6A08`, `sub_8231A120`, `sub_823211D0`, `sub_82321788`, `sub_823EAA68`, `sub_8247D008`,
`sub_8364D158`, `sub_8364D258`, `sub_8371A0A8`, `sub_8387AF30`, `sub_839668E0`, `sub_839754D0`.
All touch +48/+52/+56 of objects that are not the device (for example `sub_82321788` initialises
them from arguments; `sub_8364D158` keeps buffer fields there; `sub_839668E0` is stack-based).
None contains a `stwu` append, and none receives a device pointer in the tracking. The list's
fixed-offset heuristic matched them. **No action.**

### Engine-built PM4 (observed)

- No real type-3 header constant outside the library, from `tools/find_pm4.py` output filtered
  by known opcodes, plus an `oris` scan (`0xC000` hits at `sub_83843CE0`, `sub_838450F8`,
  `sub_83986B48`, `sub_8386F0C0` were not inspected individually; those addresses sit among
  NTSTATUS-style code).
- Limit: type-0 headers are small ordinary constants and are not distinguishable statically. The
  absence of cursor appends above covers them for tracked code only.
- A data-section header scan gave 7 `C0003601` in the `0x83A90000` block and isolated hits in
  code. `0xC0003601` is also a float near -2.0; unknown which, none confirmed as packets.

### `sub_8375FB08` make-space (2 engine call sites, observed)

`sub_837EBDC8` above and `sub_822F16D0` (only when global `0x83C0783C` != 0, which also stores
`dev+10908` to `dev+10928` and `0x83C07850`). **Replace the callee** (it is a real function, so a
strong definition works).

## 2. Inlined device state

### Inlined EndVertices (observed: 72 sites, 52 functions)

```
lwz rA,92(renderer)   ; device
lwz rB,13428(rA)      ; end of the vertex allocation
stw rB,48(rA)         ; ring cursor = end
```

BeginVertices `sub_83761488` is a call (15,652 traced, about 26 per frame) but End has no call,
so a replaced Begin cannot see when vertices are finished.

**Recommendation: no engine override.** Native BeginVertices returns a pointer from a guest
scratch buffer, snapshots draw state, and queues the draw. Flush the queued draw at the next D3D
entry (draw, Clear, Resolve, swap), by which time the engine has filled the vertices. This is
inferred from the inspected sites (Begin, fill, End with no D3D call between); not all 72 were
inspected, so verify them during implementation. The inlined store to `dev+48` becomes harmless
once the ring is unused.

### Inlined state writes (observed: at least 99 functions)

Pattern: read-modify-write of a shadow field, then OR a bit into a dirty mask.

| Device field | Meaning | Status |
|---|---|---|
| +0..+32 (`ld`/`std`) | Dirty masks | observed |
| +1152 + 24n | Fetch constant slot n; engine patches bits, for example `rlwimi r10,r26,11,19,21` in `sub_83649650` | observed |
| +1872, +1896..+1916 | Fetch slots 30/31, vertex stream style updates | observed (writes), inferred (meaning) |
| +1920.. | VS float constants (`stfs`) | observed |
| +6016.. | PS float constants (`stfs`) | observed |
| +10112, +10120, +10136 | Register block 0x4900 with dirty mask +32 | observed |
| +11812 | Written from a global before draws, with a dirty bit in +16 | observed (writes), unknown (meaning) |
| +10460, +11840, +11856, +11860, +12436, +12440, +12540 | Accessed by engine | unknown (meaning) |

Consequences:
- **State setters cannot be the source of truth.** Native draw must read the shadow.
- Fetch patches modify existing words, so a replaced `SetTexture` must keep writing real fetch
  constant words to +1152. Recommendation: leave the library's setters running and replace only
  draw, clear, resolve, swap and ring functions.
- Dirty masks can be used as they are for native change tracking, cleared at each native draw as
  the library does.

## 3. GPU-facing kernel calls, fences, interrupts

All Vd* imports listed in `notes/d3d_region.txt` sit in the library, so they are replaced with it:

| Function | Imports | Status |
|---|---|---|
| `sub_83760050` | VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, VdSetSystemCommandBufferGpuIdentifierAddress | observed |
| `sub_837695D8` | VdInitializeEngines, VdSetGraphicsInterruptCallback | observed |
| `sub_83769800` | VdSetGraphicsInterruptCallback, VdSetSystemCommandBufferGpuIdentifierAddress | observed |
| `sub_8376A9F8` swap | VdGetSystemCommandBuffer, VdSwap, VdSetDisplayMode, VdPersistDisplay | observed |
| `sub_83771C08` | VdRetrainEDRAM, VdRetrainEDRAMWorker | observed |
| `sub_8376CCF8` | VdInitializeScalerCommandBuffer | observed |
| `sub_83771808` | VdEnableDisableClockGating; calls `sub_837659C8` (REG_TO_MEM `C0013E00`) | observed |

- Interrupt callback: `sub_8375FE50` (`SetInterruptCallback(8375FE50, 40001E80)`,
  `notes/logs/trace-gameplay.log`). Vblank interrupts can stay host-driven; **CP interrupts from
  PM4 disappear, so everything waiting on them must be signalled natively** (inferred).
- Fences: `sub_8375F7C0` emits `C0025800` EVENT_WRITE_SHD using `dev+10908` and an address derived
  from `dev+10896`; `sub_8375FB08` reads through `dev+10896` (observed). No tracked engine function
  reads `dev+10896` directly, but `sub_822F16D0` and `sub_837EBDC8` read `dev+10908`. A native
  replacement must keep the counter advancing and the completed value at `*(dev+10896)` coherent,
  or waits may hang (inferred).
- WAIT_REG_MEM helper `sub_83763CF0` has engine callers `sub_83649C20` and `sub_8380D508`:
  callable, replace in the library. Purpose unknown.

## 4. GPU-written memory read by the CPU

- Occlusion queries: no `0x5B` (EVENT_WRITE_ZPD) or `0x23` (VIZ_QUERY) packet constants found by
  `tools/find_pm4.py` (observed, static). `0x5A` (EVENT_WRITE_EXT) `C0015A01` appears in
  `sub_83761488`, `sub_83761928`, `sub_83761E58`, `sub_83762240`; what uses it is unknown.
- Memexport: 0 of the 181 shaders in `project/ng2/shaders/shader_manifest.bin` have a memory
  `alloc` (81 VS allocate types 1 and 2, 100 PS type 2). Observed for that list only; it comes
  from a playthrough cache (finding #58), so shaders outside it are unknown.
- Resolved textures: the game renders correctly without `readback_resolve`, which crashes (#17),
  so a required CPU read is unlikely (inferred, tested scenes only). Keep a write-back-on-demand
  fallback. No static proof.

## Engine functions with bypasses

Columns: PPC instruction count; calls per frame in the trace ("0 / not hit" means not hit on the
Chapter 3 route); bypass classes. `fetch[...]` lists slots.

| Function | Size | /frame | Bypass |
|---|---|---|---|
| `sub_822F16D0` | 755 | 0 / not hit | regs:10888,10908,10928 |
| `sub_8231B388` | 321 | 0 / not hit | dirty fetch[10] PSc regs:11812,13428 EndVertices x2 |
| `sub_8231B868` | 454 | 0 / not hit | dirty fetch[11,12] VSc PSc |
| `sub_8231C910` | 884 | 1.0 | dirty fetch[10] PSc regs:11812,13428 EndVertices x4 |
| `sub_8232B448` | 344 | 0 / not hit | dirty fetch[30,31] VSc |
| `sub_8232EB58` | 1396 | 20.0 | dirty fetch[16] PSc regs:12540 |
| `sub_82331328` | 176 | 0 / not hit | dirty fetch[0] regs:10120 |
| `sub_82331AF0` | 219 | 0 / not hit | dirty VSc PSc |
| `sub_82333218` | 657 | 1.0 | dirty fetch[] PSc regs:11812,13428 EndVertices x3 |
| `sub_823395F0` | 320 | 1.0 | dirty fetch[31] VSc regs:11812 |
| `sub_8233AD18` | 97 | 134.0 | dirty regs:10112 |
| `sub_8233C470` | 320 | 0 / not hit | dirty fetch[0] VSc regs:11812,13428 EndVertices x1 |
| `sub_8233CF00` | 269 | 0 / not hit | dirty fetch[0] VSc |
| `sub_82349598` | 591 | 0 / not hit | dirty fetch[] regs:13428 EndVertices x1 |
| `sub_8234C6E0` | 564 | 0 / not hit | dirty fetch[] VSc regs:11812,13428 EndVertices x1 |
| `sub_824864E0` | 226 | 0 / not hit | dirty fetch[3] PSc regs:11840 |
| `sub_825097E0` | 66 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_826F05C0` | 205 | 0 / not hit | dirty fetch[] regs:11812,13428 EndVertices x1 |
| `sub_8282A510` | 227 | 0 / not hit | dirty fetch[] VSc regs:11812,13428 EndVertices x1 |
| `sub_8282B390` | 243 | 0 / not hit | dirty PSc regs:11812,13428 EndVertices x1 |
| `sub_8282B748` | 326 | 0 / not hit | dirty fetch[0,1,2] |
| `sub_82851CE8` | 69 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_82851DF8` | 63 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_82852A08` | 388 | 0 / not hit | dirty fetch[1,2,3] |
| `sub_82853130` | 83 | 0 / not hit | dirty fetch[31] regs:11812 |
| `sub_828562C0` | 34 | 0 / not hit | dirty regs:11812 |
| `sub_828566E0` | 124 | 0 / not hit | dirty fetch[1] |
| `sub_82857A70` | 564 | 0 / not hit | dirty fetch[0,1] PSc regs:10112,11812 |
| `sub_82858420` | 105 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_82859A90` | 631 | 0 / not hit | dirty fetch[] regs:11812,13428 EndVertices x1 |
| `sub_8291D2B0` | 112 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_83069C78` | 243 | 0 / not hit | dirty fetch[] VSc regs:11812,13428 EndVertices x1 |
| `sub_83637900` | 670 | 0 / not hit | dirty fetch[0,1,2] PSc regs:11812,13428 EndVertices x1 |
| `sub_836382E0` | 1167 | 0 / not hit | dirty fetch[0,1] PSc regs:11812,13428 EndVertices x2 |
| `sub_83639BE8` | 444 | 0 / not hit | dirty fetch[0] regs:10112 |
| `sub_8363A900` | 200 | 2.0 | dirty fetch[0] |
| `sub_8363ABF0` | 200 | 2.0 | dirty fetch[0] |
| `sub_8363AEE0` | 439 | 4.0 | dirty fetch[0] PSc regs:10112,11812 |
| `sub_8363B580` | 295 | 2.0 | dirty PSc regs:13428 EndVertices x1 |
| `sub_8363BA18` | 300 | 2.0 | dirty PSc regs:13428 EndVertices x1 |
| `sub_8363C2A0` | 652 | 0 / not hit | dirty fetch[0,1,2] PSc regs:11812,13428 EndVertices x1 |
| `sub_8363DE88` | 366 | 1.0 | dirty fetch[0,1] regs:11812,13428 EndVertices x1 |
| `sub_8363E3F0` | 646 | 4.0 | dirty fetch[0,1,2] PSc regs:11812,13428 EndVertices x1 |
| `sub_8363EE58` | 699 | 1.0 | dirty fetch[0,1,2] PSc regs:11812,13428 EndVertices x1 |
| `sub_8363F8B0` | 292 | 1.0 | dirty fetch[0] PSc regs:10460,11812,11856,11860,12436,12440,13428 EndVertices x1 |
| `sub_8363FD18` | 389 | 1.0 | dirty fetch[0,1] PSc regs:10460,11812,11856,11860,12436,12440,13428 EndVertices x1 |
| `sub_836403C8` | 936 | 1.0 | dirty fetch[0,1,2] PSc regs:11812,13428 EndVertices x2 |
| `sub_83641A28` | 693 | 0 / not hit | dirty fetch[1] regs:11812,13428 EndVertices x3 |
| `sub_836425A0` | 612 | 0 / not hit | dirty fetch[0,1] regs:11812,13428 EndVertices x1 |
| `sub_83642FF0` | 571 | 0 / not hit | dirty fetch[0,1] regs:10112,11812,13428 EndVertices x1 |
| `sub_83643C80` | 937 | 0 / not hit | dirty fetch[0,1] PSc regs:10112,11812,13428 EndVertices x2 |
| `sub_83645708` | 2651 | 0 / not hit | dirty fetch[0] regs:11812,13428 EndVertices x6 |
| `sub_83648078` | 649 | 1.0 | dirty fetch[0,1] PSc regs:11812,13428 EndVertices x1 |
| `sub_83649650` | 375 | 1.0 | dirty fetch[0,1] regs:11812 |
| `sub_8364A1A8` | 383 | 0 / not hit | dirty fetch[0] regs:11812,13428 EndVertices x1 |
| `sub_8364A770` | 265 | 0 / not hit | dirty fetch[0] regs:11812,13428 EndVertices x1 |
| `sub_8364ACE0` | 678 | 0 / not hit | dirty fetch[0,1] PSc regs:10112,11812,13428 EndVertices x1 |
| `sub_8364BE70` | 382 | 0 / not hit | dirty fetch[0] regs:11812,13428 EndVertices x1 |
| `sub_8364C438` | 356 | 0 / not hit | dirty fetch[0] regs:11812,13428 EndVertices x2 |
| `sub_8365F640` | 752 | 0 / not hit | dirty fetch[0,1,2] regs:11812,13428 EndVertices x3 |
| `sub_83661218` | 167 | 0 / not hit | dirty PSc regs:11812,13428 EndVertices x1 |
| `sub_836614B0` | 169 | 0 / not hit | dirty PSc regs:11812,13428 EndVertices x1 |
| `sub_83661750` | 169 | 0 / not hit | dirty PSc regs:11812,13428 EndVertices x1 |
| `sub_83673D78` | 195 | 0 / not hit | dirty PSc |
| `sub_83674418` | 106 | 0 / not hit | dirty fetch[0] |
| `sub_836769A8` | 410 | 0 / not hit | dirty fetch[10,11] PSc |
| `sub_836779A8` | 289 | 0 / not hit | dirty fetch[1,2,3] |
| `sub_8367A418` | 279 | 0 / not hit | dirty VSc |
| `sub_8367ACF0` | 582 | 0 / not hit | dirty fetch[] PSc regs:11812,13428 EndVertices x2 |
| `sub_8367BCE0` | 247 | 0 / not hit | dirty fetch[16,17] VSc |
| `sub_8367C5E8` | 165 | 0 / not hit | dirty fetch[3] |
| `sub_8368B4A8` | 235 | 1.0 | dirty PSc |
| `sub_8368E4F0` | 77 | 0 / not hit | dirty regs:11812,13428 EndVertices x1 |
| `sub_83717C18` | 88 | 0 / not hit | dirty VSc |
| `sub_837189B0` | 86 | 0 / not hit | dirty VSc |
| `sub_8371F940` | 90 | 0 / not hit | dirty PSc regs:10136 |
| `sub_83724130` | 266 | 1.0 | dirty fetch[] |
| `sub_837A45A0` | 29 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_837A4610` | 51 | 0 / not hit | regs:13428 EndVertices x1 |
| `sub_837A47E8` | 5 | 2.0 | EndVertices x1 |
| `sub_837A53C8` | 205 | 0 / not hit | EndVertices x1 |
| `sub_837A7028` | 79 | 0 / not hit | dirty |
| `sub_837A7158` | 61 | 0 / not hit | dirty |
| `sub_837A7240` | 38 | 0 / not hit | dirty |
| `sub_837E42B8` | 124 | 1065.0 | dirty |
| `sub_837E4658` | 81 | 480.0 | dirty fetch[31] regs:11812 |
| `sub_837EBDC8` | 391 | 1.0 | dirty regs:10908,10912,10928,10941,11812,13500,13504 |
| `sub_83801B10` | 94 | 0 / not hit | dirty regs:11812,13428 EndVertices x1 |
| `sub_838021E0` | 120 | 9.0 | dirty VSc regs:11812,13428 EndVertices x1 |
| `sub_83802BF8` | 172 | 0 / not hit | dirty VSc |
| `sub_83802E88` | 170 | 0 / not hit | dirty VSc |
| `sub_83803110` | 172 | 0 / not hit | dirty VSc |
| `sub_838033A0` | 31 | 0 / not hit | EndVertices x1 |
| `sub_83803418` | 33 | 0 / not hit | EndVertices x1 |
| `sub_8380D7B0` | 94 | 11.0 | dirty fetch[31] |
| `sub_8380DE20` | 97 | 15.1 | dirty fetch[] |
| `sub_8380DF90` | 209 | 0 / not hit | dirty |
| `sub_8380F108` | 181 | 0 / not hit | dirty fetch[0] VSc regs:11812 |
| `sub_8380F5B0` | 183 | 11.0 | dirty fetch[0] PSc regs:11812 |
| `sub_8380F860` | 72 | 0 / not hit | dirty VSc regs:11812 |
| `sub_83810E18` | 64 | 15.1 | dirty regs:10112 |
| `sub_83811090` | 68 | 15.1 | dirty fetch[31] regs:11812 |
| `sub_8388A808` | 260 | 0 / not hit | dirty fetch[0,1,2] |
