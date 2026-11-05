# Native renderer inventory: resources, shaders, pass graph, Xenos dependencies

Date: 2026-11-04. Scope: what a truly native renderer must load and render with no Xenos emulation.
Read-only inventory of the TU2 image, the game data, existing logs and the SDK. No game was run.

Status tags: **observed** = read directly from data or logs, **inferred** = reasoned from evidence,
**unknown** = not established. Scripts were throwaway, in the session scratchpad; each result below
can be reproduced from the named input.

Coverage limits that apply throughout:
- Pass graph evidence is the title screen and the Chapter 1 courtyard (`notes/logs/frame-d3d12.log`,
  a 1x MSAA session, see #104), plus cutscene notes in #113. Other chapters are not surveyed.
- The shader cache is one playthrough (`544307D5.xsh`, last written 2026-11-02 20:51), so it shows
  what ran, not everything that can run.
- PM4 histograms use only the two census swaps' dumps still on disk (`ng2_ib_18924_*.bin`).

## 1. Resources

### 1.1 Asset containers

| Item | Finding | Evidence | Status |
|---|---|---|---|
| Archives | `*.ng2` are concatenated assets with no compression at container level: 23 files incl. 4 DLC packages. Model/texture payloads parse in place | `game/*.ng2`, `game/dlc/*`; XPR2 census | observed |
| Asset pair | `GMD\0` header followed at +0x1000 by an `XPR2` bundle; 1:1 in char, rtm, chapters. `other.ng2` has 562 XPR2 vs 150 GMD (UI etc.) | signature counts per file | observed |
| Other headers | `chr_dat2` also appears (start of `char.ng2`, `cmn.ng2`); `string_pack` in cmn/other. `chr_dat2` and GMD layouts not decoded | hex dumps | observed (presence), unknown (layout) |
| XPR2 layout | Big-endian: `'XPR2'`, u32 header size, u32 data size, u32 count, then 16-byte entries (FourCC, offset, size, name offset). Offsets are relative to bundle +12; data starts at bundle + 12 + header size | `s_chap_01.ng2` at 0x1000 | observed |
| XPR2 census | 1,953 bundles in total, 5.3 GB of data: 52,506 TX2D, 37,195 VBUF, 35,457 IBUF, 5,356 USER. Loader also tests a TXCM tag; none on disk | parse of all `.ng2` + DLC; `sub_837F5AD8` | observed |
| USER entries | Engine metadata inside the XPR2 (per model 4 in the sampled bundles). Layout not decoded; expected to hold material / mesh tables | `s_chap_01.ng2` | inferred (purpose), unknown (layout) |
| Loader | `sub_837F5AD8(xpr)` walks entries by tag; textures go to `sub_83882650`, which adds the data base to the base address (fetch word +32) and mip address (+48), i.e. `XGOffsetBaseTextureAddress`. `sub_838826B8` is the matching generic resource path (dispatches on resource type via `sub_837641C8`) | `tools/show_func.py`; ng2_recomp.222.cpp:23512 | observed (code), inferred (names) |

Hook point for a native loader: `sub_837F5AD8` sees every static texture, vertex buffer and index
buffer once, with its header, so host resources can be created there instead of inferring them
from register writes. **inferred**

### 1.2 D3D resource headers (as stored on disk and used in memory)

| Type | Layout | Evidence | Status |
|---|---|---|---|
| Common | +0 Common (low bits type: 1 VB, 2 IB, 3 texture), +4 RefCount, +8 Fence, +12 ReadFence, +16 Identifier, +20 BaseFlush (`FFFF0000`) | TX2D/VBUF/IBUF dumps | observed (values), inferred (names) |
| Texture, 52 bytes | +24 MipFlush, +28..+51 six-dword `xe_gpu_texture_fetch_t`, big-endian. `SetTexture` `sub_83759928` reads exactly +28..+48 | XPR2 entries; `show_func` | observed |
| Vertex buffer, 32 bytes | +24 `xe_gpu_vertex_fetch_t` dword 0 (dword address + type 3), +28 dword 1 (endian, size in dwords) | VBUF dumps | observed |
| Index buffer, 32 bytes | +24 address, +28 size in bytes; Common `0x20000002` in the sample (high bit presumably 32-bit indices) | IBUF dumps | observed (fields), inferred (bit) |

### 1.3 Static textures: what load has to do

All 52,505 parsed textures:

| Property | Values | Native handling |
|---|---|---|
| Dimension | 2D only. No cube or volume texture on disk | `VK_IMAGE_TYPE_2D` |
| Format | DXT1 29,246; DXT4_5 18,046; k_8_8_8_8 2,837; k_8 2,364; DXT2_3 9; k_2_10_10_10_AS_16_16_16_16 3 | BC1, BC3, RGBA8/BGRA8, R8, BC2, RGB10A2. Six formats cover the whole game |
| Tiling | 100% tiled | Untile at load, once, on CPU or in a load-time compute pass |
| Endian | 1 (8-in-16) 47,301; 2 (8-in-32) 2,840; 0 (none) 2,364 | Swap at load. Matches BCn / 8888 / k_8 respectively |
| Mips | Packed mip tail on 47,307; max level up to 11 | Port the SDK's packed-mip addressing, or precompute a table per header |
| Sign | All zero: no signed, biased or gamma textures | Nothing to emulate |
| Swizzle | `0x688` (47,301), `0x60A` (2,840), `0xA00` (2,364) | Fixed `VkComponentMapping` per format |
| Sizes | Mostly power of two (128 is the largest group at 18,233), with some NPOT such as 147 | Host handles NPOT |

Status: **observed** for all rows (decoded from headers with `xenos.h` field layouts); native column
is **inferred**. Fetch words also carry per-texture sampler defaults, which the engine may override.

Not established: how the engine creates *runtime* textures (`D3DDevice_Create*`, movie frames, UI
atlases). That belongs to the D3D API inventory. **unknown**

### 1.4 Static geometry

| Item | Finding | Status |
|---|---|---|
| Vertex data | Big-endian in VBUF payloads; sampled fetch constant endian = 2 (8-in-32) | observed |
| Layouts | 59 distinct vfetch layouts in the 91 cached VS, stride 2-15 dwords, position 32_32_32_FLOAT then packed attributes | observed |
| Attribute formats (game VS) | 32_32_32_32_FLOAT, 16_16_FLOAT, 32_32_32_FLOAT, 32_32_FLOAT, 10_11_11, 8_8_8_8, 16_16_16_16_FLOAT, 32_FLOAT | observed |
| Packing | 10_11_11 and 11_11_10 are Xenos layouts with component order as in `spirv_translator_fetch.cpp`:190-203; 16-bit floats use extended range, the SDK notes it treats them as GLSL half | observed |
| Conversion | Swap dwords at load, then attributes map to host formats. `10_11_11` has no direct Vulkan format (signed/unsigned depends on the fetch) so needs a decode in the VS or a rewrite at load | inferred |
| Dynamic geometry | `BeginVertices` `sub_83761488` and friends write vertices into guest memory per frame (#96); no static header exists for these | observed (call), inferred (no header) |

### 1.5 Render targets and runtime surfaces

Render targets are EDRAM surfaces plus a resolve destination, not textures. Each row of section 3
must become a host image created up front (or lazily keyed by destination address + format + size).
The only persistent identity the game has for them is the resolve destination address. **inferred**

## 2. Shaders

### 2.1 Where they live and how many

| Item | Finding | Evidence | Status |
|---|---|---|---|
| Location | All compiled into the XEX, region `0x8200AC98-0x82240AC0`. None in game data: zero container magics across every `.ng2` and DLC | scan of `ghidra-db/image-tu2/82000600_d.bin` + game data | observed |
| Container | Magic `102A1100` pixel (237), `102A1101` vertex (426): 663 containers. u32 header size `a`, u32 program size `b`. Ucode ends at container + a + b and starts at +a or +a+0x40 (VS literal block). Header holds a D3DX constant table with names | same | observed |
| **Unique ucode** | **621: 556 game (347 VS, 209 PS) + 65 WMV decoder (64 VS, 1 PS)** | same | observed |
| Seen at runtime | 191 (91 VS, 100 PS) in the shader cache, i.e. about a third of the game set. 178 byte-identical to a container, 13 not | `544307D5.xsh` | observed |
| Pipelines | 349 ROV records (154 VS/PS pairs), 395 RTV records | `.xpso` caches | observed |

Offline conversion must therefore target all 556 from the image, not the 181-entry manifest
(`project/ng2/shaders/shader_manifest.bin`), which only lists what one playthrough reached.
The image is the player's own, so the same "read from your copy" rule as `shader_precompile.cpp`
still works: convert on first run, ship no game code. **inferred**

### 2.2 Runtime vfetch patching (blocks naive offline conversion)

70 of the 347 game VS contain vfetch instructions with format 0 (539 instructions), and 8 of the 13
non-identical cached shaders differ from their container *only inside vfetch instructions*. So the
containers hold placeholder fetches, and something fills in format, offset and stride at bind time
against the vertex declaration. Most likely the XDK vertex shader bind. **observed** (diffs),
**inferred** (who patches)

Consequences: those 70 VS cannot be converted from the image alone; each needs (VS, declaration)
pairs. Either hook the patch and translate after it, or implement the patch and enumerate
declarations from the assets. Of the other 5 non-identical shaders, 3 are in the image outside the
parsed container layout and 2 (1,548 and 1,437 dword VS) differ in 34 and 13 words outside vfetch.
Their origin is **unknown**.

### 2.3 What the game ucode uses

Counts are shaders out of 347 VS / 209 PS, excluding WMV.

| Feature | VS | PS | Status / note |
|---|---|---|---|
| vfetch | 347 | - | observed |
| vfetch with non-r0 index | 40 | - | observed; see below |
| Vertex textures | 17 | - | observed; `GroundHeightSampler`, `HeightSampler`, `WeightSampler`, `ExpansionSampler`, `MeltAreaSampler`. All 17 use unnormalized coordinates and `setTexLOD` |
| Conditional jumps on bool constants | 276 | 69 | observed; ubershaders, see `ShadowFlg`, `FogFlg`, `PointLightBool`. Static branches at draw time |
| Predicated CF | 303 | 94 | observed; plain ALU predicates, translate directly |
| `kill` | 0 | 86 | observed; alpha test implemented in shader (`AlphaRef`, `AlphaTest`) |
| Loops | 1 | 2 | observed |
| Relative constant addressing | 3 | 3 | observed; almost none |
| `mova` (a0) | 0 | 0 | observed |
| memexport | **0** | - | observed; the 64 memexport VS are all WMV |
| Cube / 3D fetch, cube op | 0 | 0 | observed |
| Point size export | 0 | - | observed; point sprites use constant size |
| oDepth | - | 2 | observed; `0x821AFF68`, `0x821B0068` (`DepthBufferSampler`) |
| MRT | - | 5 use oC1, 4 oC2 | observed; matches the three-target luminance pass |
| Interpolators | up to 10 | - | observed |

**Skinning goes through vfetch, not constants.** Cached VS fetch slot 94 at stride 12 (three vec4s,
a 3x4 matrix) with computed index registers r1 / r2 / r6, and no cached VS uses a0 or relative constants (3 uncached game VS use relative constants).
`BlendMtx0..3` in the constant tables are therefore probably not constant arrays but values
consumed by these fetches. **observed** (fetch form), **inferred** (bone palette)

Stream slots: 95 = stream 0, 94 = second stream or matrix palette, down to 90. **observed** (slots),
**inferred** (mapping).

### 2.4 Constant layout

Registers `c0-c255` VS at reg 0x4000, PS at 0x4400; bool and loop constants at 0x4900; fetch
constants at 0x4800 (#96). Per-draw shader literals come via LOAD_ALU_CONST. Constant table names in
every container give a stable name to register map, so a native D3D layer can set constants by
slot with no name lookup, or dump a reflection table offline. **observed**

Typical VS names: `WorldViewProj`, `WorldMtx`, `ViewProj`, `CamPos`, `Fog*`, `DirLight*`,
`PointLight*`, `SpotLight`, `ShadowMat_WLPB`, `Scroll0-3`. Typical PS: `TextureSampler*`,
`ShadowMapSamp`, `ShadowData`, `NormalSampler`, `MM2Alpha*`, `AlphaRef`. Post-processing names
match the pass graph: `g_fMiddleGray`, `g_fBloomMod`, `g_avSampleOffsets`, `g_fElapsedTime`.

### 2.5 Using the SDK translator offline

`SpirvShaderTranslator` (`pipeline/shader/spirv_translator*.cpp`) already turns ucode into SPIR-V
with no GPU present, and its Modification key is small. What it assumes, and what has to change:

| Assumption in the SDK path | Evidence | Native change |
|---|---|---|
| vfetch reads guest memory from an SSBO via fetch constants, swapping endian in shader | set `kDescriptorSetSharedMemoryAndEdram`; `spirv_translator_fetch.cpp` | Bind each stream as a buffer (device address or SSBO per slot) and rebase; or emit real vertex attributes for r0-indexed fetches. Keep buffer form for slot 94 palette fetches |
| Texture swizzle and signs from system constants | `texture_swizzles`, `texture_swizzled_signs` | Fixed per format, use view swizzle |
| `exp_adjust` etc. read from fetch constants at runtime | #103 | Keep (cheap) or bake |
| EDRAM ROV output in FSI mode; host RT mode exists | `edram_*` constants, `spirv_translator_rb.cpp` | Host RT mode only. Delete FSI use |
| Index fetch through `vertex_index_load_address` for unsupported types | system constants | Use `gl_VertexIndex` |
| Texture bindings by fetch slot, resolved per translation | descriptor sets for textures | Static layout per shader |

Key per pipeline: the Modification carries `interpolator_mask` (needs VS/PS pairing),
`dynamic_addressable_register_count`, `output_point_parameters`, `vertex_kill_and`, UCP count. Pairs
are known only for the 154 seen. For unseen shaders, compile VS with all exported interpolators
and PS with its input mask, or translate at load. **inferred**

Keeping the translator means shaders still run as "ucode semantics in SPIR-V". That is conversion
at build / first run, not GPU emulation, and it is the same model as Unleashed Recompiled's XenosRecomp.
The whole-pipeline cost is an expected translation of 556 shaders; time not measured. **inferred**

## 3. Per-frame pass graph (Chapter 1 courtyard, 1x session)

1,680 draws including 24-draw D3D drains, which do not exist in a native renderer. Formats: f0 8888,
f3 7e3, f4 k_16_16, f12 7e3-as-16161616, f14 k_32_FLOAT; depth f1 D24FS8, f0 D24S8.
Status of the table: **observed** from `frame-d3d12.log` swap 16496 / 16556 and `tex-d3d12.log`.
Purpose names are **inferred** from formats and #99 / #103.

| # | Pass | Target (pitch, format) | Draws | Resolve to | Read by |
|---|---|---|---|---|---|
| 1 | Clears | Scene MRT, 1x then 4x view | 2 | - | - |
| 2 | Shadow depth | pitch 560, depth D24FS8 base 0, depth-only | 8 | `1C984000` k_24_8_FLOAT 512x512 | pass 3 |
| 3 | Shadow moments + blur | k_16_16 RT, same base | 2 | `1C984000` k_16_16, twice | Scene shading (`ShadowMapSamp`) |
| 4 | Main scene opaque | 1120, 7e3 + D24FS8 (reverse Z) | ~430 | - | - |
| 5 | Scene transparent / effects | 1120, f12 on same surface | 78 | - | - |
| 6 | Scene resolves | same | - | depth `1EB91000` 1120x584 k_24_8_FLOAT; colour `1F775000` k_16_16_16_16_FLOAT | 7; `1EB91000` loaded in cutscenes |
| 7 | Downsample | 320, 7e3 at base 0x206 | 1 | `1D1B0000` 280x146 FP16 | 8, 9 |
| 8 | Luminance chain | 80, 3 x k_32_FLOAT MRT (bases 206, 20A, 20E) | 5 | 64, 16, 4, 1, 1 px, 3 each; final set `1EB4A000/9/8` | Tone map next frame (adaptation) |
| 9 | Bloom | 320 and 240, 8888 at base 0x206 | 5 | `1D20A000` 282x148, `1D24C000` 186x146, `1D26A000` 186x97 (x3), `1EB30000` 186x97 | 10 |
| 10 | Tone map / composite | 1120, 7e3 base 0 | 1 | `1CE10000` 8888 swapped | 11 |
| 11 | Upscale to 1280x720 | 1280, 8888 base 0 | 1 | - | - |
| 12 | HUD | 1280, 8888, D24S8 at base 0x2D0 | 11+ | `1F045000` / `1F3DD000` front buffers | Display |

Cutscenes differ: 36 resolves (#113), and a depth-only pass at base 0x2D0 sized over the whole EDRAM.
A second `1CE10000` resolve at another size exists in some sessions (#116). Other effects use
`ScreenSampler`, `RefractTexture`, `BackBufTexSampler`, `DistSampler`, `ColorBufferSampler`, which
need a scene copy mid-frame. Not present in the courtyard capture; **unknown** where they sample from.

### EDRAM aliasing and the native equivalent

| Aliasing | Evidence | Native equivalent | Status |
|---|---|---|---|
| Scene buffer 1x <-> 4x/2x: game clears via a 4x-view of half pitch, and in MSAA sessions draws in 4x/2x and reads samples as 1x pixels | #102, #111, #117; surf `11800460` vs `08C20230` | One 1120x584 image. Issue MSAA draws at double resolution along sampled axes, as `ng2_msaa_supersample` does. Keeping the game on the 1x session (#104) removes the case entirely | observed; recommendation inferred |
| Depth and stencil written in MSAA view, read in 1x view, needed for lit scene | #108 | Same single image | observed (needed), inferred (stencil lighting cause) |
| Base 0x206 re-bound as 7e3, k_32_FLOAT, 8888 while also the depth base | #102, section 3 | Separate images per pass. Nothing reads across formats through EDRAM | inferred from resolve order; verify no draw reads prior contents |
| Luminance targets not cleared by the game, depend on prior contents being wiped through clears-via-transfer | #102: white image with no transfers | Explicit clear on bind, or load op CLEAR for those passes | observed (symptom), inferred (fix) |
| Colour and depth sharing bases | #99, #113 | Depth and colour are separate objects | observed |
| Whole-EDRAM depth surface overlapping the colour image | #113 | Moot: no shared memory | observed (problem) |
| `1C984000` holds k_24_8_FLOAT then k_16_16 | section 3 | Two images. Texture lookup must pick by pass/format, not address alone. Snapshot key already does (#116) | observed |

Key rule: a resolve is a copy from a host image to a host texture, keyed by (destination, format,
size). `1D26A000` is resolved three times per frame and is read between them; the third read must
see the second resolve, so ordering must follow the draw stream. **inferred**

CPU reads of resolved data: none found in this inventory; not searched. **unknown**, and needs a
check before dropping the guest memory write.

## 4. Xenos behaviours

| Behaviour | NG2 use | Evidence | Native plan | Status |
|---|---|---|---|---|
| 7e3 render target | Main scene and composite | RT f3, #99 | `R16G16B16A16_SFLOAT` or `B10G11R11_UFLOAT`. Clamp to [0, 32). Image will not match 7e3 banding exactly, blending is in a different precision | observed; plan inferred |
| f12 7e3-as-16 | 78 scene draws | RT f12, `xenos.h`:313-315 | Same image | observed; plan inferred |
| k_16_16 fixed -32..32 RT | Shadow moments | RT f4, #103 | `R16G16_SNORM` with scale, or `R16G16_SFLOAT`. Shader export scale must be handled; on read `exp_adjust` handles it | observed; plan inferred |
| D24FS8 reverse Z | Scene depth; z func GEQUAL (`dc 700766`) | depth f1, dctl | `D32_SFLOAT_S8`. Depth readback as float24 values: shaders read k_24_8_FLOAT as float | observed; plan inferred |
| Resolve format / swap | 7e3 -> FP16, 7e3 -> 8888, red/blue swap | #109 | Blit / compute at the same point | observed |
| Half-pixel offset | **None needed.** `PA_SU_VTX_CNTL` = 5 in all sampled writes: `pix_center` 1 (.5 centres), round-to-even | PM4 dumps; `xenos.h`:850-864 | Nothing | observed |
| Gamma | No gamma render target or texture seen: no f1 RT, all texture signs 0. PWL gamma not needed. Display output gamma not checked | census, frame log | sRGB handled in final present | observed (RT, static textures), unknown (display) |
| Tessellation | None: `VGT_OUTPUT_PATH_CNTL` 0, `VGT_HOS_CNTL` 0, all pipelines plain vertex | PM4; `.xpso` host type 0 | Nothing | observed for sampled scenes |
| Primitive types | Tri strip 495, rect list 35+, quad list 36, point list rare; primitive restart on (290 of 349) | PM4; `.xpso` strip_cut, gs | Rect / quad expansion via index gen or VS. Rect lists are mostly fullscreen passes, and clears vanish natively | observed; plan inferred |
| Vfetch quirks | Mini fetch reuse, computed index fetch, runtime patching, BE formats | 2.2, 2.3 | Section 2.5 | observed |
| MSAA sample positions | Only matter in MSAA sessions, where samples are read as pixels | #111, #117 | Avoided by the single 1x image; sample order mapping known | observed |
| Interpolation | `SQ_CONTEXT_MISC` 4: centres only in 513 of 579 writes, centroids + centres in 24, centroids only in 42. All tfetch location bit 0 | PM4, ucode | Default interpolation | observed |
| HiZ | `hiz_pitch` set in RB_SURFACE_INFO. No visible output effect, emulator ignores it | surf values | Ignore | observed (ignored by SDK) |
| Predicated tiling | None: `SET_BIN_SELECT FFFFFFFF` only | PM4 | Nothing | observed |
| Occlusion queries | No EVENT_WRITE_ZPD in dumps; other paths not checked | PM4 | Stub if D3D API inventory shows calls | observed (dumps), unknown (scenes) |
| memexport | Not in any game shader. Only XDK WMV decoder (`Shader_Residuals*`, `Shader_Deblock*`, `Shader_Mocomp*`), path `...wmv\decoder_c9e\dxva_pk\...` | container strings | Replace movie playback with a host decoder (29 `.wmv` files). Cached set has 0 of these shaders, so GPU decode has not been seen running | observed |
| D3D drains | 2/3 of draws | #97, #100 | Gone with the emulated ring | observed |
| Alpha test | In shader via `kill`; SDK alpha test also present | 2.3 | Check register for residual use | observed (kill), unknown (register) |
| Blend, stencil, cull, bias | Standard. Stencil enabled in 25 of 395 RTV pipelines, depth bias in 17 | `.xpso` | Map to Vulkan dynamic state | observed |

## 5. Hardest dependencies, in order

1. **Vertex path.** Runtime vfetch patching (70 VS), matrix palettes read by computed-index vfetch
   (40+ VS), and big-endian packed attributes. Shader translation needs a real vertex model.
2. **Render target semantics.** Luminance chain relies on clear-via-transfer side effects (#102),
   lighting reads depth/stencil across views in MSAA sessions (#108). Forcing the 1x session removes
   the view switch but changes what the game asked for; the user's visuals must be compared.
3. **Format fidelity.** 7e3 and -32..32 fixed-point targets have no direct host format, and the tone
   mapper depends on them (black-scene bugs in #99 / #103 came from this chain).
4. **Coverage.** About 2/3 of game shaders have never run under test; scene-specific effects
   (vertex-texture grass and melt, refraction) are unverified.
5. **Movies.** WMV decode must be replaced at the player level.

## 6. Open items

- Decode `GMD`, `USER` and `chr_dat2` layouts (vertex declarations per mesh live somewhere there).
- Find the vfetch patch function; confirm it is XDK vertex shader bind.
- Origin of the two large non-identical VS (2.2).
- Runtime texture creation paths and CPU reads of resolved textures.
- Cutscene and other-chapter pass graphs; mid-frame scene copies for refraction.
- Display gamma ramp.
