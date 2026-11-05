# XDK Direct3D API inventory, TU2 `0x83758000-0x83773000`

Read-only inventory for the truly native renderer. Addresses are TU2.

Per-frame counts come from `project/ng2/out/build/win-amd64-relwithdebinfo/ng2_trace_gp.txt` (NG2_TRACE_ALL, gameplay, 602 swaps; log `notes/logs/trace-gameplay.log`). A 0 means not called in that gameplay window only: menus, cutscenes and Mission Mode are not covered. External callers are direct `bl` / `b` from `notes/func_index.json`. Device and packet evidence is the generated disassembly (`tools/show_func.py`) and `ghidra-db/image-tu2`.

Status: **observed** = behaviour read in the code or trace. **inferred** = behaviour observed, XDK name assigned from it (there are no symbols). **unknown** = role not established.

## Summary

- 463 functions in the range: 454 D3D, 9 audio-like.
- **Replacement boundary: 195 entry points**: 93 called directly from outside, plus 102 Set functions reached only through the device table.
- Direct entry points outside the state tables: 84, of which 24 observed, 58 inferred, 2 unknown. The other 9 direct ones are state setters.
- All 111 Set functions have an observed register or fetch-field effect; 29 have no XDK name yet (20 render, 9 sampler).
- 111 getters have no direct caller and no consumer was found. Leave them unless a table reader appears.
- 148 internal helpers and diagnostics can stay unhooked.
- Gameplay calls per frame: 1156 `SetTexture`, 709 `SetVertexShader`, 496 `DrawIndexedVertices`, 385 `SetPixelShader`, 29 `Resolve`, 26 `BeginVertices`, 12 `DrawVertices`, 5 `Clear`, 1 `Swap`.

Calling convention: Xbox 360 PPC, args in r3-r10 and f1+, the rest on the stack, `r3` = device unless the row says otherwise. Device layout: finding #96.

## Device / lifetime

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `8375CEF8` | SetGammaRamp | r3=dev, r5=ramp, 1536 bytes compared with +14988; flag +16692 | 0 | 1 (822F16D0) | inferred |
| `8375E610` | Direct3D_CreateDevice | r7=params, r8=out device**; allocates 22528 bytes aligned 128; init in 837695D8; refcount +60, owner +10888 | 0 | 1 (837EBA30) | inferred |
| `8375E6F0` | Device AddRef | r3=dev; +60++ | 0 | 2 (838896B0, 83897EF0) | inferred |
| `8375E708` | Device Release | r3=dev; at 1 calls teardown 83769800 | 0 | 7 (837A3EF0, 837A3FA0, 8385BEC8, ...) | inferred |
| `8375E760` | AcquireThreadOwnership | r3=dev; writes owner +10888 | 0 | 2 (83897F58, 83899288) | inferred |
| `8375E7A0` | ReleaseThreadOwnership | r3=dev; eieio, clears +10888 | 0 | 2 (83897F58, 83899288) | inferred |
| `8375E7B0` | Suspend | r3=dev; notify callback(2), GPU sync 8375FF08, sets +10942 bit 1 | 0 | 1 (822F0BF8) | inferred |
| `8375E848` | Resume | r3=dev; notify callback(1), clears +10942 bit 1 | 0 | 1 (822F0BF8) | inferred |
| `83760050` | Reset | r3=dev, r4=present params; VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack | 0 | 2 (822F2218, 837EBDC8) | inferred |
| `837641A0` | Resource GetDevice | r3=res, r4=out; AddRef of global device pointer at 0x820007BC | 0 | 1 (8385C028) | inferred |
| `83765230` | GetDeviceCaps | r4=out; copies 304 bytes from static data | 0 | 1 (83852C40) | inferred |
| `83765248` | GetDisplayMode | r3=dev, r5=out; +13528, +13532, +21536, +13536 | 0 | 1 (83889098) | inferred |
| `837691E0` | create front/back buffers | r3=dev, r4=present params; VdQueryVideoMode, 83759568, 83759688 | 0 | 2 (822F2218, 837EBDC8) | inferred |
| `83769498` | release present buffers | r3=dev; unbinds RTs and depth, syncs, releases +14812 | 0 | 2 (822F2218, 837EBDC8) | inferred |

## State

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `8375C500` | SetScissorRect | r3=dev, r4=RECT*; stores +12668..+12680, intersects viewport +12640.. | 40 | 4 (836922B8, 83692320, 8369FBA8, ...) | observed |
| `8375C600` | SetViewport | r3=dev, r4=D3DVIEWPORT9*; four ints + MinZ/MaxZ floats; tail 83759F48 | 36 | 10 (822F07C8, 82330D68, 82330E98, ...) | inferred |
| `837613A0` | SetShaderGPRAllocation | r3=dev, r4=flags, r5=VS GPRs, r6=PS GPRs (0,0 means 64,64); +10920; emits the drain buffers (#97) | 47 | 14 (822F16D0, 82331AF0, 82332558, ...) | inferred |
| `83771CF0` | SetPredication | r3=dev, r4=mask; +12708 | 1 | 1 (838048E8) | inferred |

## Shaders

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `83760B00` | CreateVertexShader | r3=function blob; optional hook pointer at 0x84C3F0C0; alloc 83835940 | 0 | 20 (822F02E8, 82319C88, 82319CE8, ...) | inferred |
| `83760C10` | SetPixelShader (label corrected, #124) | r3=dev, r4=shader; +12684; copies register patch list into shadow | 709 | 34 (8231B388, 8233B7E8, 8282B748, ...) | observed |
| `83760EC8` | CreatePixelShader | r3=function blob; optional hook pointer at 0x84C3F0BC; parse 83760DD0 | 0 | 18 (822F02E8, 8231AD60, 8232E318, ...) | inferred |
| `83760FB8` | SetVertexShader (label corrected, #124) | r3=dev, r4=shader; +12688; sets shader dirty bit | 385.5 | 32 (8231B388, 8233B7E8, 8282B748, ...) | observed |
| `83761210` | SetVertexDeclaration | r3=dev, r4=decl; +11812; dirty bit 0x80000 in +16. Mostly inlined, see Risks | 0 | 3 (83888F28, 8388A4F8, 8388A808) | inferred |
| `83761318` | CreateVertexDeclaration | r3=element array (12 bytes each, stream 255 ends); 83761228 | 0 | 12 (822F02E8, 8231ADC0, 8233BE40, ...) | inferred |

## Constants

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `83760930` | SetVertexShaderConstantF | r3=dev, r4=start, r5=float4*, r6=count; +1920+16*start | 768 | 26 (8232EB58, 82330B10, 82331608, ...) | observed |
| `83760A18` | SetPixelShaderConstantF | r3=dev, r4=start, r5=float4*, r6=count; +6016+16*start | 84 | 10 (8363AEE0, 8363DE88, 8363E3F0, ...) | observed |

## Textures / resources

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `837584F0` | GetLevelDesc | r3=texture, r4=level, r5=desc* | 36 | 16 (8363A900, 8363ABF0, 8368B4A8, ...) | inferred |
| `83758B60` | lock level core | r3=tex, r4=type?, r5.. outs; 837586F0, 83763CF0 | 1.5 | 8 (8363CE10, 8364CD98, 8365F208, ...) | inferred |
| `83758C28` | LockRect | r3=tex, r6=out locked rect, r7=RECT*, r8=flags | 0 | 1 (837A46D8) | inferred |
| `83758D60` | XGSetTextureHeader-like | one caller, 83882350; many register args | 0 | 1 (83882350) | inferred |
| `837592C8` | GetLevelCount | r3=tex; (+44 >> 6 & 15) + 1 | 0 | 2 (838532B8, 83882650) | observed |
| `837592D8` | GetSurfaceLevel | r3=tex, r4=level; allocates 48-byte view, AddRef parent | 0 | 5 (836610C8, 83661EA0, 838532B8, ...) | inferred |
| `837593A8` | LockRect, level 0 entry | forces r4=0 then 83758C28 | 0 | 1 (8388A808) | observed |
| `837593C0` | UnlockRect | r3=tex; base +32, mip +48, then 83763F80 | 0 | 1 (8388A808) | inferred |
| `837593D8` | GetCubeMapSurface | r3=tex, r4=face, r5=level; allocates 48-byte view | 0 | 2 (838532B8, 838536E8) | inferred |
| `83759490` | tail to GetLevelDesc | b 837584F0 | 13 | 2 (838532B8, 83880EF0) | observed |
| `83759498` | tail to volume GetLevelDesc | b 837585F0 | 0 | 2 (838532B8, 83880EF0) | observed |
| `837594A0` | GetVolumeLevel | r3=tex, r4=level; allocates 32-byte view | 0 | 2 (838532B8, 838536E8) | inferred |
| `83759568` | CreateTexture | r3..r8 = W, H, D, levels, usage, format; 52-byte header + physical alloc | 0 | 17 (822F3708, 82331A80, 82332218, ...) | inferred |
| `837597F0` | Surface GetDesc | r3=surface, r4=desc; type 4 | 0 | 3 (8385BDE0, 8385C028, 83880EF0) | inferred |
| `83759888` | Surface LockRect | r3=surface; forwards to 83758C28 | 0 | 2 (8385BDE0, 8385C028) | inferred |
| `837598A8` | Volume GetDesc | r3=volume, r4=desc; type 16 | 0 | 2 (8385C520, 83880EF0) | inferred |
| `837598F0` | Volume LockBox | r3=volume; forwards to 83758CB8 | 0 | 1 (8385C520) | inferred |
| `83759910` | Surface / Volume Unlock | r3=view; parent +24, then 83763F80 | 0 | 3 (8385BDE0, 8385BEC8, 8385BFC8) | inferred |
| `83759928` | SetTexture | r3=dev, r4=slot, r5=texture; +(slot+3134)*4, fetch +1152+slot*24; fence tag | 1156 | 67 (8231B388, 8231B868, 8231C910, ...) | observed |
| `8375C680` | SetStreamSource | r3=dev, r4=stream, r5=vb, r6=offset, r7=stride, r8=dirty mask; +(stream+3113)*4 | 0 | 8 (82853130, 828562C0, 837E4658, ...) | inferred |
| `8375C7A0` | GetStreamSource | r3=dev, r4=stream, r5=out offset, r6=out stride | 0 | 2 (83812300, 83889228) | inferred |
| `8375C828` | SetIndices | r3=dev, r4=ib; +12428 | 497.1 | 10 (8232B448, 823395F0, 82853130, ...) | observed |
| `8375D088` | SetVertexFetchConstant | r3=dev, r4=constant, r5=vertex buffer, r6=offset; slot (95-r4)/3 | 21 | 2 (8232EB58, 83649650) | inferred |
| `83763CF0` | resource lock core | r3=res, r4=type; blocks on fences +8 / +12 via 8375FC08, r10=flags | 1.5 | 2 (83649C20, 8380D508) | inferred |
| `83763F80` | resource unlock core | r3=res, r4=addr, r5=?; atomic lock count, cache flush | 1.5 | 11 (8363CE10, 83649C20, 8364CD98, ...) | inferred |
| `837640A8` | Resource AddRef | r3=res; atomic +4 | 0 | 17 (8232EB58, 837A0548, 837A0758, ...) | observed |
| `83764120` | Resource Release | r3=res; atomic +4, frees via 83763AF0 at 0 | 1 | 40 (82330AB0, 83660D08, 836610C8, ...) | observed |
| `837641C8` | Resource GetType | r3=res; low nibble of +0 and dimension bits | 98 | 4 (838532B8, 83880EF0, 83881310, ...) | observed |
| `83764258` | CreateVertexBuffer | r3=length, r4=usage; 32-byte header, type 1 | 0 | 2 (83649BB8, 83889098) | inferred |
| `83764320` | VertexBuffer Lock | r3=vb, r5, r6=flags; 83763CF0 type 10 | 0 | 1 (83889228) | inferred |
| `83764370` | VertexBuffer Unlock | r3=vb; 83763F80 | 0 | 1 (83889228) | inferred |
| `83764380` | CreateIndexBuffer | r3=length, r4=usage, r5=format; 32-byte header, type 2 | 0 | 1 (83889098) | inferred |
| `83764430` | IndexBuffer Lock | r3=ib, r5, r6=flags; 83763CF0 type 12 | 0 | 1 (83889098) | inferred |
| `83764478` | IndexBuffer Unlock | r3=ib; 83763F80 | 0 | 1 (83889098) | inferred |
| `83764488` | Resource IsSet | r3=res, r4=dev; scans streams and textures by type | 0 | 1 (837EB910) | inferred |

## Render targets

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `83759688` | CreateRenderTarget / depth surface | r3=W, r4=H, r5=format, r6=multisample, r7=parameters; 48-byte surface; EDRAM heap 83764918, limit 2048 | 1 | 11 (822F0268, 822F02E8, 822F3708, ...) | inferred |
| `8375C8B8` | SetRenderTarget | r3=dev, r4=index, r5=surface; +(index+3108)*4 | 47 | 29 (822F07C8, 822F09D8, 822F16D0, ...) | observed |
| `8375CC20` | SetDepthStencilSurface | r3=dev, r4=surface; +12448, RB_DEPTH_INFO +10376, +10560 | 32 | 30 (822F07C8, 822F09D8, 822F16D0, ...) | observed |
| `8375E290` | Clear | r3=dev, r4=count, r5=rects, r6=flags, r7=color, f1=Z, r9=stencil | 5 | 21 (822F07C8, 822F09D8, 822F2340, ...) | observed |
| `83762CA8` | Resolve | r3=dev, r4=flags (low 3 bits RT index, 4 = depth), r5=src rect, r6=dest texture, r7=dest point, r8=level, r9=slice, r10=clear color, f1=clear Z, stack: stencil, params | 29 | 42 (8231B388, 8231C910, 82331328, ...) | observed |
| `83771E60` | BeginTiling | r3=dev, r5=count, r6=rects, r7=clear color, f1=Z, r9=stencil; +12740, +12748 | 0 | 1 (838048E8) | inferred |
| `837721D0` | EndTiling | r3=dev, r5 and r6 resolve rects, f1=Z; calls Resolve per tile | 0 | 1 (83804A98) | inferred |
| `83772688` | tiling-related ring segment switch | called from 838048E8 with BeginTiling | 0 | 1 (838048E8) | unknown |
| `83772918` | tiling-related segment end | called by 822F16D0 only | 0 | 1 (822F16D0) | unknown |

## Draw

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `83761488` | BeginVertices | r3=dev, r4=prim, r5=count, r6=stride; returns a pointer into the ring; draw packet written first (C0012201). End is inline, see Risks | 26 | 49 (8231B388, 8231C910, 82333218, ...) | observed |
| `83761928` | BeginIndexedVertices | r3=dev, r4=prim, r5, r6=vertices, r7=indices, r8=format flags, r9=stride | 0 | 4 (8282B390, 82858420, 8291D2B0, ...) | inferred |
| `83761E58` | DrawVertices | r3=dev, r4=prim, r5=start, r6=count; auto-index | 12 | 3 (83649650, 8380D7B0, 8388A808) | observed |
| `83762240` | DrawIndexedVertices, specialised | r3=dev, r6=start index, r7=index count; every caller passes r4=6, which the function ignores (prim folded to strip); C0032201 | 496.1 | 6 (8232B448, 823395F0, 82853130, ...) | observed |

## Present

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `8376A4C8` | pre-swap GPU sync | r3=dev, r4=mode; predication off, marker 83771728, CP wait, callback 8376A330 | 1 | 1 (837EBDC8) | inferred |
| `8376A9F8` | Swap | r3=dev, r4=front buffer texture, r5=params; VdSwap, VdGetSystemCommandBuffer, VdSetDisplayMode, 83770C48, 83771808 | 1 | 1 (837EBDC8) | observed |
| `8376B110` | Present-style blit to front buffer | r3=dev; saves viewport, RT0 = +14816, 8376A4C8, Resolve, restores | 0 | 2 (837A4430, 83899288) | inferred |

## Sync / command buffer

| Address | D3D role | Args / device fields | /frame | External callers | Status |
|---|---|---|---|---|---|
| `8375EF18` | command memory allocator | r3=dev, r4=dwords, r5=flags; returns segment pointer. Direct use by 83677DD0 and 837EBDC8 | 114 | 2 (83677DD0, 837EBDC8) | inferred |
| `8375FB08` | KickOff | r3=dev; closes ring segment, makes space, returns cursor (+48, +56) | 25 | 2 (822F16D0, 837EBDC8) | observed |
| `8375FC08` | BlockOnFence | r3=dev, r4=fence, r5=mode; +10896 GPU fence, +10908 CPU fence | 2 | 1 (837EBDC8) | inferred |
| `8375FCE0` | InsertCallback core | r3=dev, r4=cursor, r5=type, r6=callback, r7=ctx; returns cursor | 3 | 1 (837EBDC8) | inferred |
| `8375FF08` | BlockUntilIdle | r3=dev; fence, then spin on +11000 | 0 | 2 (822F2218, 837EBDC8) | inferred |
| `8375FFF8` | InsertCallback | r3=dev, r4=type, r5=callback, r6=ctx | 0 | 1 (8388A808) | inferred |
| `8376B258` | flush CPU cache range | r3=addr, r4=size; dcbf loop | 0 | 2 (83677DD0, 837EBDC8) | observed |

## Render state setters (table `0x83AA4FF8`, entries `{Get, Set, default}`)

`83768F70` loads the tables at `0x83AA4F80` and `0x83AA5468` while setting up the device. The engine state applier `sub_837EC7C8` (74 callers) jumps through `dev+64+state`; `837EC840` jumps through `dev+468+...` for samplers. States 0-36 use the stub pair `836A3860` / `837A16F0`, outside the range. Register effects are observed. `D3DRS_` names are inferred from those effects and the defaults.

| State | Set | Get | Default | /frame | Direct callers | Writes | Name |
|---|---|---|---|---|---|---|---|
| 40 | `8375A908` | `8375A940` | 0x1 | 44.9 | 1 | +11868, RB_DEPTHCONTROL | ZENABLE |
| 44 | `8375A978` | `8375A9A0` | 0x3 | 6 | 0 | RB_DEPTHCONTROL | ZFUNC |
| 48 | `8375A948` | `8375A968` | 0x1 | 1027 | 0 | RB_DEPTHCONTROL | ZWRITEENABLE |
| 52 | `8375A1E0` | `8375A200` | 0x0 | 7 | 0 | PA_SU_SC_MODE_CNTL | FILLMODE |
| 56 | `8375A1B0` | `8375A1D0` | 0x6 | 530 | 1 | PA_SU_SC_MODE_CNTL | CULLMODE |
| 60 | `8375A248` | `8375A2C8` | 0x0 | 532 | 1 | +11844, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | ALPHABLENDENABLE |
| 64 | `8375A5D8` | `8375A660` | 0x0 | 4 | 0 | +11844, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | SEPARATEALPHABLENDENABLE |
| 68 | `8375A708` | `8375A7A8` | 0xFFFFFFFF | 8.3 | 0 | RB_BLEND_GREEN, RB_BLEND_RED, RB_BLEND_BLUE, RB_BLEND_ALPHA | BLENDFACTOR |
| 72 | `8375A368` | `8375A3E8` | 0x1 | 537 | 0 | +11840, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | SRCBLEND |
| 76 | `8375A3F8` | `8375A478` | 0x0 | 537 | 0 | +11840, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | DESTBLEND |
| 80 | `8375A2D8` | `8375A358` | 0x0 | 537 | 0 | +11840, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | BLENDOP |
| 84 | `8375A4F8` | `8375A558` | 0x1 | 9 | 0 | +11840, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | SRCBLENDALPHA |
| 88 | `8375A568` | `8375A5C8` | 0x0 | 9 | 0 | +11840, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | DESTBLENDALPHA |
| 92 | `8375A488` | `8375A4E8` | 0x0 | 9 | 0 | +11840, RB_BLENDCONTROL0, RB_BLENDCONTROL1, RB_BLENDCONTROL2, RB_BLENDCONTROL3 | BLENDOPALPHA |
| 96 | `8375A210` | `8375A238` | 0x0 | 528 | 0 | RB_COLORCONTROL | ALPHATESTENABLE |
| 100 | `8375A670` | `8375A6A8` | 0x0 | 56 | 0 | RB_ALPHA_REF | ALPHAREF |
| 104 | `8375A6D8` | `8375A6F8` | 0x7 | 25.9 | 0 | RB_COLORCONTROL | ALPHAFUNC |
| 108 | `8375A9B0` | `8375A9E8` | 0x0 | 518.1 | 0 | +11872, RB_DEPTHCONTROL | STENCILENABLE |
| 112 | `8375A9F0` | `8375AA18` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | TWOSIDEDSTENCILMODE |
| 116 | `8375AA58` | `8375AA80` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | STENCILFAIL |
| 120 | `8375AA90` | `8375AAB8` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | STENCILZFAIL |
| 124 | `8375AAC8` | `8375AAE8` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | STENCILPASS |
| 128 | `8375AA28` | `8375AA48` | 0x7 | 4 | 0 | RB_DEPTHCONTROL | STENCILFUNC |
| 132 | `8375ABC8` | `8375ABE0` | 0x0 | 4 | 0 | RB_STENCILREFMASK | STENCILREF |
| 136 | `8375ABE8` | `8375AC00` | 0xFFFFFFFF | 4 | 0 | RB_STENCILREFMASK | STENCILMASK |
| 140 | `8375AC08` | `8375AC20` | 0xFFFFFFFF | 4 | 0 | RB_STENCILREFMASK | STENCILWRITEMASK |
| 144 | `8375AB28` | `8375AB50` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | CCW_STENCILFAIL |
| 148 | `8375AB60` | `8375AB88` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | CCW_STENCILZFAIL |
| 152 | `8375AB98` | `8375ABB8` | 0x0 | 4 | 0 | RB_DEPTHCONTROL | CCW_STENCILPASS |
| 156 | `8375AAF8` | `8375AB18` | 0x7 | 4 | 0 | RB_DEPTHCONTROL | CCW_STENCILFUNC |
| 160 | `8375AC28` | `8375AC40` | 0x0 | 4 | 0 | RB_STENCILREFMASK_BF | CCW_STENCILREF |
| 164 | `8375AC48` | `8375AC60` | 0xFFFFFFFF | 4 | 0 | RB_STENCILREFMASK_BF | CCW_STENCILMASK |
| 168 | `8375AC68` | `8375AC80` | 0xFFFFFFFF | 4 | 0 | RB_STENCILREFMASK_BF | CCW_STENCILWRITEMASK |
| 172 | `8375AC88` | `8375ACC8` | 0x0 | 4 | 0 | PA_CL_CLIP_CNTL, COHER_DEST_BASE_7 | CLIPPLANEENABLE |
| 176 | `8375AFC0` | `8375B010` | 0x3F800000 | 4 | 0 | +11884, PA_SU_POINT_SIZE | POINTSIZE |
| 180 | `8375B020` | `8375B068` | 0x3F800000 | 4 | 0 | +11888, PA_SU_POINT_MINMAX | POINTSIZE_MIN |
| 184 | `8375AFB0` | `8375AFB8` | 0x0 | 4 | 0 | +11876 | unknown |
| 188 | `8375B078` | `8375B0C0` | 0x42800000 | 4 | 0 | +11892, PA_SU_POINT_MINMAX | POINTSIZE_MAX |
| 192 | `8375AE60` | `8375AE80` | 0x1 | 4 | 0 | PA_SU_SC_MODE_CNTL | MULTISAMPLEANTIALIAS |
| 196 | `8375AE90` | `8375AEA8` | 0xFFFFFFFF | 4 | 0 | PA_SC_AA_MASK | MULTISAMPLEMASK |
| 200 | `8375ACD8` | `8375ACE8` | 0x0 | 4 | 0 | +11848 | SCISSORTESTENABLE |
| 204 | `8375ACF0` | `8375AD98` | 0x0 | 6 | 0 | PA_SU_POLY_OFFSET_FRONT_SCALE, PA_SU_POLY_OFFSET_BACK_SCALE, PA_SU_SC_MODE_CNTL | SLOPESCALEDEPTHBIAS |
| 208 | `8375ADB8` | `8375AE50` | 0x0 | 6 | 0 | PA_SU_POLY_OFFSET_FRONT_OFFSET, PA_SU_POLY_OFFSET_BACK_OFFSET, PA_SU_SC_MODE_CNTL | DEPTHBIAS |
| 212 | `8375AEB0` | `8375AEE8` | 0xF | 9 | 0 | +11852, RB_COLOR_MASK | COLORWRITEENABLE |
| 216 | `8375AEF0` | `8375AF28` | 0xF | 4 | 0 | +11856, RB_COLOR_MASK | COLORWRITEENABLE1 |
| 220 | `8375AF30` | `8375AF68` | 0xF | 4 | 0 | +11860, RB_COLOR_MASK | COLORWRITEENABLE2 |
| 224 | `8375AF70` | `8375AFA8` | 0xF | 4 | 0 | +11864, RB_COLOR_MASK | COLORWRITEENABLE3 |
| 228 | `8375B7B0` | `8375B7D8` | 0x1 | 4 | 0 | VGT_HOS_CNTL | TESSELLATIONMODE |
| 232 | `8375B740` | `8375B768` | 0x3F800000 | 4 | 0 | VGT_HOS_MIN_TESS_LEVEL | MINTESSELLATIONLEVEL |
| 236 | `8375B778` | `8375B7A0` | 0x3F800000 | 4 | 0 | VGT_HOS_MAX_TESS_LEVEL | MAXTESSELLATIONLEVEL |
| 240 | `8375B0D0` | `8375B330` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP0 |
| 244 | `8375B0F0` | `8375B340` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP1 |
| 248 | `8375B118` | `8375B350` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP2 |
| 252 | `8375B140` | `8375B360` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP3 |
| 256 | `8375B168` | `8375B370` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP4 |
| 260 | `8375B190` | `8375B380` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP5 |
| 264 | `8375B1B8` | `8375B390` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP6 |
| 268 | `8375B1E0` | `8375B3A0` | 0x0 | 4 | 0 | SQ_WRAPPING_0 | WRAP7 |
| 272 | `8375B200` | `8375B3B0` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP8 |
| 276 | `8375B220` | `8375B3C0` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP9 |
| 280 | `8375B248` | `8375B3D0` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP10 |
| 284 | `8375B270` | `8375B3E0` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP11 |
| 288 | `8375B298` | `8375B3F0` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP12 |
| 292 | `8375B2C0` | `8375B400` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP13 |
| 296 | `8375B2E8` | `8375B410` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP14 |
| 300 | `8375B310` | `8375B420` | 0x0 | 4 | 0 | SQ_WRAPPING_1 | WRAP15 |
| 304 | `8375B430` | `8375B470` | 0x1 | 7 | 1 | PA_CL_VTE_CNTL, PA_CL_CLIP_CNTL | VIEWPORTENABLE |
| 308 | `8375B480` | `8375B528` | 0x0 | 1037 | 0 | +12020, +28, RB_COLOR_INFO | unknown |
| 312 | `8375B530` | `8375B5D8` | 0x0 | 4 | 0 | +12024, +28, RB_COLOR1_INFO | unknown |
| 316 | `8375B5E0` | `8375B688` | 0x0 | 4 | 0 | +12028, +28, RB_COLOR2_INFO | unknown |
| 320 | `8375B690` | `8375B738` | 0x0 | 4 | 0 | +12032, +28, RB_COLOR3_INFO | unknown |
| 324 | `8375B7E8` | `8375B810` | 0x0 | 4 | 0 | PA_SU_VTX_CNTL | HALFPIXELOFFSET |
| 328 | `8375B820` | `8375B840` | 0x0 | 4 | 0 | PA_SU_SC_MODE_CNTL | PRIMITIVERESETENABLE |
| 332 | `8375B850` | `8375B870` | 0xFFFF | 4 | 0 | VGT_MULTI_PRIM_IB_RESET_INDX | PRIMITIVERESETINDEX |
| 336 | `8375B878` | `8375B898` | 0x0 | 4 | 0 | RB_COLORCONTROL | ALPHATOMASKENABLE |
| 340 | `8375B8A8` | `8375B8C8` | 0x87 | 4 | 0 | RB_COLORCONTROL | ALPHATOMASKOFFSETS |
| 344 | `8375B8E0` | `8375B908` | 0x40000000 | 4 | 0 | PA_CL_GB_HORZ_CLIP_ADJ | unknown |
| 348 | `8375B910` | `8375B938` | 0x40000000 | 4 | 0 | PA_CL_GB_VERT_CLIP_ADJ | unknown |
| 352 | `8375B940` | `8375B960` | 0x3F800000 | 4 | 0 | PA_CL_GB_HORZ_DISC_ADJ | unknown |
| 356 | `8375B968` | `8375B990` | 0x3F800000 | 4 | 0 | PA_CL_GB_VERT_DISC_ADJ | unknown |
| 360 | `8375BA30` | `8375BA50` | 0x0 | 4 | 0 | RB_HIZCONTROL | unknown |
| 364 | `8375BA60` | `8375BA80` | 0x0 | 4 | 0 | RB_HIZCONTROL | unknown |
| 368 | `8375BA90` | `8375BAB0` | 0x0 | 4 | 0 | RB_HIZCONTROL | unknown |
| 372 | `8375BAC0` | `8375BAD8` | 0x0 | 4 | 0 | RB_HIZCONTROL | unknown |
| 376 | `8375BAE0` | `8375BAE8` | 0x0 | 4 | 0 | +13580 | unknown |
| 380 | `8375BAF0` | `8375BB00` | 0x0 | 4 | 0 | +11844 | unknown |
| 384 | `8375B998` | `8375B9B8` | 0x2 | 4 | 0 | +11844 | unknown |
| 388 | `8375B9C8` | `8375BA20` | 0x2 | 25.9 | 0 | +11844 | unknown |
| 392 | `8375A828` | `8375A850` | 0x1 | 4 | 0 | PA_SC_LINE_CNTL | unknown |
| 396 | `8375A860` | `8375A8A0` | 0x3F800000 | 4 | 0 | PA_SU_LINE_CNTL | unknown |
| 400 | `8375A8D8` | `8375A8F8` | 0x1 | 4 | 0 | +22280 | unknown |

## Sampler state setters (table `0x83AA5468`, entries `{Get, Set, default}`)

Args: r3=dev, r4=sampler, r5=value. Fetch constant at `dev+1152+24*sampler`. Field positions are observed; names are inferred.

| State | Set | Get | Default | /frame | Direct callers | Field | Name |
|---|---|---|---|---|---|---|---|
| 0 | `8375C250` | `8375C288` | 0x0 | 101 | 0 | fetch dword0 [12:10] | ADDRESSU |
| 1 | `8375C2A0` | `8375C2D8` | 0x0 | 101 | 0 | fetch dword0 [15:13] | ADDRESSV |
| 2 | `8375C2F0` | `8375C328` | 0x0 | 80 | 0 | fetch dword0 [18:16] | ADDRESSW |
| 3 | `8375C1E0` | `8375C230` | 0x0 | 80 | 0 | fetch dword5 | BORDERCOLOR |
| 4 | `8375BCA0` | `8375BD60` | 0x0 | 164.9 | 55 | fetch dword3 [20:19] | MAGFILTER |
| 5 | `8375BB10` | `8375BBD0` | 0x0 | 164.9 | 55 | fetch dword3 [22:21] | MINFILTER |
| 6 | `8375BE30` | `8375BE70` | 0x2 | 83 | 0 | fetch dword3 [24:23] | MIPFILTER |
| 7 | `8375C040` | `8375C0A0` | 0x0 | 80 | 1 | fetch dword4 [21:12] | MIPMAPLODBIAS |
| 8 | `8375C0E0` | `8375C150` | 0x0 | 1160.1 | 4 | fetch dword4 [5:2] | MAXMIPLEVEL |
| 9 | `8375BF20` | `8375BF88` | 0x1 | 80 | 0 | fetch dword3 [27:25] | MAXANISOTROPY |
| 10 | `8375BD98` | `8375BE20` | 0x0 | 80 | 0 | +16, +11994 | unknown |
| 11 | `8375BC08` | `8375BC90` | 0x0 | 80 | 0 | +16, +11994 | unknown |
| 12 | `8375BE88` | `8375BF10` | 0x0 | 80 | 0 | +16, +11994 | unknown |
| 13 | `8375C160` | `8375C1D0` | 0xD | 81 | 1 | fetch dword4 [9:6] | MINMIPLEVEL |
| 14 | `8375C340` | `8375C380` | 0x0 | 80 | 0 | +20 | unknown |
| 15 | `8375BF98` | `8375BFF8` | 0x0 | 80 | 0 | +20 | unknown |
| 16 | `8375C398` | `8375C3D8` | 0x0 | 80 | 0 | +16 | unknown |
| 17 | `8375C3F0` | `8375C430` | 0x0 | 80 | 0 | +16 | unknown |
| 18 | `8375C448` | `8375C488` | 0x0 | 80 | 0 | +20 | unknown |
| 19 | `8375C4A0` | `8375C4E8` | 0x1 | 80 | 0 | +4 | unknown |

## Internal helpers: leave unhooked

No external caller. They go away when their public callers are replaced. Roles are by group.

| Group | Functions | Status |
|---|---|---|
| Ring / command memory | `8375E478` `8375E538` `8375E9E8` `8375EA50` `8375EA98` `8375EBB0` `8375EC58` `8375ED10` `8375F0B0` `8375F270` `8375F338` `8375F428` `8375F5D0` `8375F7C0` `8375F8A8` `8375FA30` `8375FF80` `83764578` `83764640` `8376E150` `8376E340` `83772BC8` | observed as a group (segments, INDIRECT_BUFFER, fences) |
| State flush at draw | `83766FD0` `83767088` `83767288` `83767620` `837676E0` `83767860` `837679C0` `83767B20` `83768130` `83768200` `837682B8` `837683F8` `83768568` `83768618` `83768758` `83760680` `83759C68` `83759AA8` `83759BD0` `83759F48` | observed: dirty masks to PM4 |
| Clear / resolve internals | `8375D158` `8375D448` `8375D698` `8375DC00` `8375E350` `83762660` `83762708` `83762850` `83762BD0` | inferred: helper draws for clear and resolve |
| Texture layout | `83758128` `837585F0` `837586F0` `83758CB8` `83759070` | inferred: size, pitch and header layout. Calls 83757988, 83757AA8 and 83757F38 below the range |
| Shader / declaration | `83760DD0` `83761188` `83761228` `83765270` `83768F70` | inferred |
| Allocators | `83763AF0` `83764708` `837647E0` `83764918` | inferred: deferred free, EDRAM heap |
| Device setup / teardown | `837695D8` `83769800` `83769538` `8375E8D0` `83760798` `837604B0` `83771628` `8376E500` `8376E650` `83771C08` | inferred: VdInitializeEngines, ring, worker thread, EDRAM retrain |
| Present internals | `8376A670` `8376A710` `8376A820` `8376CBD0` `8376CCF8` `8376CEC8` `8376C688` `8376C3C8` `8376B2F8` `8376B3F8` `8376B4E0` `8376B710` `8376BB58` `8376BCF8` `8376BDF0` `83765060` `83765140` `837652E0` `837659C8` `83764B20` `83764BD0` `83764E88` `83764F20` `8375CFC0` `83771728` `83771808` | inferred: scaler, gamma, swap commands |
| Diagnostics / PIX | `83769AA8` `83769B30` `83769BB0` `83769E18` `83769EC8` `8376A140` `8376D000` `8376D078` `8376D2E8` `8376D3A0` `8376D468` `8376D5A0` `8376D698` `8376D7A0` `8376DB18` `8376DCC0` `8376DFD8` `8376E0C8` `8376E820` `8376E838` `8376E850` `8376E888` `8376E8E0` `8376E928` `8376E940` `8376E958` `8376EC18` `8376ED28` `8376FAA8` `8376FB60` `8376FF08` `83770328` `83770C48` `83770D30` `83770D40` `83770E80` `83770EF0` `837713B8` `837713C0` `83771468` | observed for functions with strings (GPU hang report, PIX capture, xbperfview); others inferred from their callers |
| Callbacks / interrupts | `8375FE50` `8376A230` `8376A330` `83769590` `837695B0` `83769B08` `83769B18` | 8375FE50 CPU interrupt handler, observed by string, 4/frame. 8376A330 pre-swap callback, 1/frame. Others address-taken only |

## Not classified

None.

## Not D3D, inside the range

`83772C90`, `83772CF8`, `83772D60`, `83772DB8`, `83772E60`, `83772EC0`, `83772F18`, `83772F60`, `83772FB8`: these lock a global critical section (`0x83BD24B8`) and call through a vtable. Code from `0x83773000` imports `XAudio*` and `XMA*`, so these look like audio wrappers (inferred). The D3D library therefore ends near `0x83772C88`. They are excluded from the counts.

## Risks

1. **Inlined setters.** `SetVertexDeclaration` has 0 gameplay calls: 59 engine functions write `dev+11812` directly (observed in generated code). `SetStreamSource` also has 0 gameplay calls against 496 indexed draws per frame. How streams get bound in gameplay is not established yet: `SetVertexFetchConstant` runs only 21/frame, and 18 engine functions match a `dev+1770-1789` store, a pattern loose enough to include false positives. Hooking the listed functions alone will miss these paths.
2. **`EndVertices` is not a function.** Callers finish inline with `lwz r10,13428(dev); stw r10,48(dev)` (`sub_8231B388`). `BeginVertices` writes the draw packet first and returns a pointer into the ring. A native version must return scratch memory and submit later, with no End hook. It sets bit 0 of `dev+24`, a possible pending flag (unverified).
3. **The engine reads and writes device fields.** 12 engine functions touch `dev+10908` (fence), about 7 each touch the stream and texture tables, and `sub_839D3850` writes `+12684`, `+12688`, `+10376`, `+10372`. Some matches may be other structs with the same offsets. The replacement must keep the real ones valid.
4. **State goes through function pointers.** Render and sampler states reach the setters through pointers in the device, so call-site patching misses them. Replace the Set functions by strong definition, or fill the device table.
5. **Direct ring use.** `83677DD0` and `837EBDC8` call the allocator `8375EF18` and write commands themselves; `notes/d3d_ringwriters.txt` lists more. `837EBDC8` also inlines the `SetTexture` / `SetStreamSource` fence tagging.
6. **Resources are guest structs.** Engine code can read headers directly (for example `SetVertexFetchConstant` reads `+24` base and `+28` size), so host resources should be keyed from these headers.
7. **Coverage.** The trace is one gameplay window. `BeginTiling`, `EndTiling`, Lock paths, `83761928`, `83772688` and `83772918` have 0 counts there and may run elsewhere. `822F16D0`, `8388A808` and `83677DD0` have no direct callers, so they are reached indirectly.
