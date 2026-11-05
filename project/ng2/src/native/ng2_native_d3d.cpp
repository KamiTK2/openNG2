// Truly native renderer (notes/native/design.md): the D3D entry points.
//
// With ng2_native_d3d off every override below forwards to the recompiled XDK function, so the
// default D3D12 path is untouched. With it on, no command buffer ever reaches the GPU front end:
// the segment kick is dropped, GPU fences and callbacks complete at once, and draws, clears,
// resolves and swaps go to native_renderer.cpp, which reads state from the guest device struct.
//
// The draw functions no longer run: only the part of the state flush with an effect beyond the
// ring is kept (NativeFlush). What still writes command segment memory, which nothing reads, is
// sub_83768758's shader load packets and the library's Swap.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "native_renderer.h"

REXCVAR_DEFINE_BOOL(ng2_native_d3d, false, "NG2",
                    "Truly native renderer: no PM4, no command processor (needs gpu_backend=vulkan)");

namespace {

// Guest D3D device fields (findings #96, notes/native/d3d_api.md).
constexpr uint32_t kDevCursor = 48;           // command segment write cursor
constexpr uint32_t kDevGpuBlock = 10896;      // GPU write-back block: +0 fence, +4 read position
constexpr uint32_t kDevFence = 10908;         // next fence value
constexpr uint32_t kDevFenceTag = 14904;      // low two bits go into the read position word
constexpr uint32_t kDevFlipRequests = 16548;  // pre-swap callbacks seen
constexpr uint32_t kDevFlips = 16552;         // flips done (callback or vblank handler)

constexpr uint32_t kDevIndexBuffer = 12428;   // guest index buffer object

// Time in the recompiled library draw functions, for the PM4 removal (design.md).
std::atomic<uint64_t> g_library_draw_ticks{0};
struct DrawTimer {
  LARGE_INTEGER t0;
  DrawTimer() { QueryPerformanceCounter(&t0); }
  ~DrawTimer() {
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_library_draw_ticks.fetch_add(uint64_t(t1.QuadPart - t0.QuadPart), std::memory_order_relaxed);
  }
};

struct Stats {
  std::atomic<uint64_t> swaps{0}, draws{0}, begins{0}, clears{0}, resolves{0}, kicks{0}, fences{0},
      callbacks{0}, drops{0}, flip_timeouts{0};
} g_stats;

bool NativeOn() {
  static const bool on = [] {
    if (!REXCVAR_GET(ng2_native_d3d)) return false;
    const std::string backend = rex::cvar::GetFlagByName("gpu_backend");
    if (backend != "vulkan") {
      REXLOG_ERROR("NG2 NATIVE: ng2_native_d3d needs gpu_backend=vulkan (got {}), staying off",
                   backend);
      return false;
    }
    REXLOG_INFO("NG2 NATIVE: on, no PM4 reaches the command processor");
    return true;
  }();
  return on;
}

uint8_t* Host(uint32_t guest) {
  return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(guest);
}
uint32_t Load(uint32_t guest) { return rex::memory::load_and_swap<uint32_t>(Host(guest)); }
void Store(uint32_t guest, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(Host(guest), value);
}

// The GPU stalls on the swap until the vblank handler flips, and the fence on the next swap makes
// the CPU wait for that: this is the game's frame pacing. Nothing stalls now, so wait here until
// the flips caught up with the swaps. The timeout covers a stopped vblank.
void WaitForFlip(uint32_t dev) {
  const int64_t limit = int64_t(GetTickCount64()) + 100;
  while (Load(dev + kDevFlips) != Load(dev + kDevFlipRequests)) {
    if (int64_t(GetTickCount64()) > limit) {
      g_stats.flip_timeouts.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    SwitchToThread();
  }
}

// The percentages sub_822F4010 publishes from the frame stamps, and the load-driven fade they steer
// (notes/native/open_questions.md section 1).
void LogFrameLoad() {
  const auto f = [](uint32_t v) { float o; std::memcpy(&o, &v, 4); return o; };
  REXLOG_INFO("NG2 NATIVE load cpu% {:.1f} gpu% {:.1f} fade {:.3f} skipflag {}", f(Load(0x83C07200)),
              f(Load(0x83C07200 + 12)), f(Load(0x83C11108)), Load(0x83C07880));
}

void LogStats() {
  const uint64_t swaps = g_stats.swaps.load();
  if (swaps % 120 != 0) return;
  REXLOG_INFO(
      "NG2 NATIVE swaps={} draws={} begins={} clears={} resolves={} kicks_dropped={} fences={} "
      "callbacks={} other_pm4_drops={} flip_timeouts={}",
      swaps, g_stats.draws.load(), g_stats.begins.load(), g_stats.clears.load(),
      g_stats.resolves.load(), g_stats.kicks.load(), g_stats.fences.load(),
      g_stats.callbacks.load(), g_stats.drops.load(), g_stats.flip_timeouts.load());
  LogFrameLoad();
  {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    static uint64_t last_ticks = 0;
    const uint64_t ticks = g_library_draw_ticks.load();
    REXLOG_INFO("NG2 NATIVE library draw cpu {:.3f} ms/frame",
                double(ticks - last_ticks) * 1000.0 / double(frequency.QuadPart) / 120.0);
    last_ticks = ticks;
  }
  ng2::native::RendererLogStats(swaps);
}

}  // namespace

#define NG2_IMP(addr) extern "C" REX_FUNC(__imp__sub_##addr)

NG2_IMP(83762240);
NG2_IMP(83768758);
NG2_IMP(83761E58);
NG2_IMP(83761488);
NG2_IMP(83761928);
NG2_IMP(8375E290);
NG2_IMP(83762CA8);
NG2_IMP(8376A9F8);
NG2_IMP(8375FB08);
NG2_IMP(8375FCE0);
NG2_IMP(837613A0);
NG2_IMP(8375FC08);
NG2_IMP(8375FF08);
NG2_IMP(8375F428);
NG2_IMP(8375F7C0);

// ---- Draws ----------------------------------------------------------------------------------

namespace {

uint64_t Load64(uint32_t guest) { return rex::memory::load_and_swap<uint64_t>(Host(guest)); }
void Store64(uint32_t guest, uint64_t value) { rex::memory::store_and_swap<uint64_t>(Host(guest), value); }
uint8_t Load8(uint32_t guest) { return *Host(guest); }
void Store8(uint32_t guest, uint8_t value) { *Host(guest) = value; }

// The library's draw prologue minus the ring (notes/native/open_questions.md section 3).
// sub_83768758 is kept: it picks the per-declaration vertex shader patch (sub_83768618), and updates
// shadows the renderer reads (+10560 through sub_83767088, +10580, +12692). Dirty masks are cleared
// exactly where the library clears them; nothing else reads them.
void NativeFlush(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  const uint64_t state = Load64(dev + 16);
  Store64(dev + 0, 0);
  Store64(dev + 8, 0);
  if (state & 0x1E0000) {
    ctx.r3.u64 = dev;
    ctx.r4.u64 = state;
    __imp__sub_83768758(ctx, base);
  }
  Store64(dev + 16, 0);
  Store64(dev + 24, 0);
  Store64(dev + 32, 0);
}

// Immediate vertices: the engine fills what Begin returns, and the renderer copies it at the next
// D3D entry, so one bump buffer in guest memory is enough. Replaces the ring segment.
constexpr uint32_t kScratchBytes = 32u << 20;
uint32_t g_scratch = 0, g_scratch_used = 0;
uint32_t ScratchAlloc(uint32_t bytes) {
  if (!g_scratch) g_scratch = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kScratchBytes, 4096);
  bytes = (bytes + 15) & ~15u;
  if (bytes > kScratchBytes) return 0;
  if (g_scratch_used + bytes > kScratchBytes) g_scratch_used = 0;
  const uint32_t guest = g_scratch + g_scratch_used;
  g_scratch_used += bytes;
  return guest;
}

// Begin's stride goes into the declaration key for the vertex shader patch, around the flush.
void FlushImmediate(PPCContext& ctx, uint8_t* base, uint32_t dev, uint32_t stride) {
  const uint8_t saved = Load8(dev + 12520), wanted = uint8_t(stride >> 2);
  Store8(dev + 12520, wanted);
  if (wanted != Load8(dev + 11824) && Load(dev + 12688)) Store64(dev + 16, Load64(dev + 16) | 0x80000);
  NativeFlush(ctx, base, dev);
  Store8(dev + 12520, saved);
}

// The End is inlined: it copies +13428 to the cursor. Nothing moved, so it changes nothing.
void FinishBegin(uint32_t dev) {
  Store(dev + 13428, Load(dev + kDevCursor));
  Store64(dev + 24, Load64(dev + 24) | 1);
}

}  // namespace

// DrawIndexedVertices(dev, _, _, start index, index count). Triangle strips (folded by the linker).
extern "C" REX_FUNC(sub_83762240) {
  if (!NativeOn()) return __imp__sub_83762240(ctx, base);
  g_stats.draws.fetch_add(1, std::memory_order_relaxed);
  const uint32_t dev = ctx.r3.u32, start = ctx.r6.u32, count = ctx.r7.u32;
  ng2::native::RendererDrainImmediate();
  { DrawTimer timer; NativeFlush(ctx, base, dev); }
  const uint32_t buffer = Load(dev + kDevIndexBuffer);
  const uint32_t common = Load(buffer);
  ng2::native::DrawCall draw;
  draw.dev = dev;
  draw.prim = 6;
  draw.count = count;
  draw.indexed = true;
  draw.index_32 = (common >> 31) != 0;
  draw.index_guest = Load(buffer + 24) + start * (draw.index_32 ? 4 : 2);
  draw.index_endian = common >> 30;
  ng2::native::RendererDraw(draw);
}

// DrawVertices(dev, prim, start, count)
extern "C" REX_FUNC(sub_83761E58) {
  if (!NativeOn()) return __imp__sub_83761E58(ctx, base);
  g_stats.draws.fetch_add(1, std::memory_order_relaxed);
  ng2::native::DrawCall draw;
  draw.dev = ctx.r3.u32;
  draw.prim = ctx.r4.u32;
  draw.first_vertex = ctx.r5.u32;
  draw.count = ctx.r6.u32;
  ng2::native::RendererDrainImmediate();
  { DrawTimer timer; NativeFlush(ctx, base, draw.dev); }
  ng2::native::RendererDraw(draw);
}

// BeginVertices(dev, prim, count, stride) -> pointer the engine fills. The End is inlined at 72
// sites, so the draw is recorded here and the bytes are picked up at the next D3D entry.
extern "C" REX_FUNC(sub_83761488) {
  if (!NativeOn()) return __imp__sub_83761488(ctx, base);
  g_stats.begins.fetch_add(1, std::memory_order_relaxed);
  ng2::native::DrawCall draw;
  draw.dev = ctx.r3.u32;
  draw.prim = ctx.r4.u32;
  draw.count = ctx.r5.u32;
  draw.immediate = true;
  draw.immediate_dwords = (ctx.r5.u32 * ctx.r6.u32 + 3) / 4;
  const uint32_t stride = ctx.r6.u32;
  ng2::native::RendererDrainImmediate();
  { DrawTimer timer; FlushImmediate(ctx, base, draw.dev, stride); }
  draw.immediate_guest = ScratchAlloc(draw.immediate_dwords * 4);
  FinishBegin(draw.dev);
  ctx.r3.u64 = draw.immediate_guest;
  if (draw.immediate_guest) ng2::native::RendererDraw(draw);
}

// BeginIndexedVertices(dev, prim, base vertex, vertices, indices, format, stride, void** index data,
// [sp+84] void** vertex data); r3 = 0 on success. Format bit 2 means 32-bit indices. As with
// BeginVertices the engine fills both after the call and there is no End to hook.
extern "C" REX_FUNC(sub_83761928) {
  if (!NativeOn()) return __imp__sub_83761928(ctx, base);
  g_stats.begins.fetch_add(1, std::memory_order_relaxed);
  ng2::native::DrawCall draw;
  draw.dev = ctx.r3.u32;
  draw.prim = ctx.r4.u32;
  draw.count = ctx.r7.u32;
  draw.indexed = true;
  draw.immediate = true;
  draw.index_32 = (ctx.r8.u32 & 4) != 0;
  draw.immediate_dwords = (ctx.r6.u32 * ctx.r9.u32 + 3) / 4;
  const uint32_t index_out = ctx.r10.u32, vertex_out = Load(ctx.r1.u32 + 84);
  const uint32_t stride = ctx.r9.u32;
  ng2::native::RendererDrainImmediate();
  { DrawTimer timer; FlushImmediate(ctx, base, draw.dev, stride); }
  draw.index_guest = ScratchAlloc(draw.count * (draw.index_32 ? 4 : 2));
  draw.immediate_guest = ScratchAlloc(draw.immediate_dwords * 4);
  FinishBegin(draw.dev);
  Store(index_out, draw.index_guest);
  Store(vertex_out, draw.immediate_guest);
  ctx.r3.u64 = 0;
  if (draw.index_guest && draw.immediate_guest) ng2::native::RendererDraw(draw);
}

// Predicated tiling. Replayed command segments could not work here, so log loudly if it ever runs.
NG2_IMP(838048E8);
NG2_IMP(83771E60);
NG2_IMP(837721D0);
extern "C" REX_FUNC(sub_838048E8) {
  if (NativeOn() && (g_stats.swaps.load() % 600) == 0) {
    REXLOG_INFO("NG2 NATIVE tiling gate obj {:08X} +16 = {}", ctx.r3.u32, Load(ctx.r3.u32 + 16));
  }
  __imp__sub_838048E8(ctx, base);
}
extern "C" REX_FUNC(sub_83771E60) {
  if (NativeOn()) REXLOG_WARN("NG2 NATIVE: BeginTiling called");
  __imp__sub_83771E60(ctx, base);
}
extern "C" REX_FUNC(sub_837721D0) {
  if (NativeOn()) REXLOG_WARN("NG2 NATIVE: EndTiling called");
  __imp__sub_837721D0(ctx, base);
}

// ---- Targets --------------------------------------------------------------------------------

// ClearF(dev, flags, rect, float colour[4], f1 z, r8 stencil): the Clear body. Clear itself is
// replaced below, so this only sees the two callers that go to it directly, sub_83771E60
// BeginTiling and sub_83772688, which is how the 2x session clears the scene depth (#129).
NG2_IMP(8375E350);
extern "C" REX_FUNC(sub_8375E350) {
  if (!NativeOn()) return __imp__sub_8375E350(ctx, base);
  g_stats.clears.fetch_add(1, std::memory_order_relaxed);
  ng2::native::RendererClear(ctx.r3.u32, ctx.r5.u32 ? 1 : 0, ctx.r5.u32, ctx.r4.u32, 0,
                             float(ctx.f1.f64), ctx.r8.u32, ctx.r6.u32);
}

// Clear(dev, count, rects, flags, color, z, stencil)
extern "C" REX_FUNC(sub_8375E290) {
  if (!NativeOn()) return __imp__sub_8375E290(ctx, base);
  g_stats.clears.fetch_add(1, std::memory_order_relaxed);
  ng2::native::RendererClear(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                             float(ctx.f1.f64), ctx.r9.u32);
}

// Resolve(dev, flags, rect, texture, point, level, slice, clear color*, ..), f1 clear z, stencil
// first on the stack.
extern "C" REX_FUNC(sub_83762CA8) {
  if (!NativeOn()) return __imp__sub_83762CA8(ctx, base);
  g_stats.resolves.fetch_add(1, std::memory_order_relaxed);
  ng2::native::RendererResolve(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
                               ctx.r10.u32, float(ctx.f1.f64), Load(ctx.r1.u32 + 84));
}

// ---- Present --------------------------------------------------------------------------------

// Swap: the library keeps its frame bookkeeping. VdSwap's packet lands in segment memory that is
// never submitted.
extern "C" REX_FUNC(sub_8376A9F8) {
  const uint32_t dev = ctx.r3.u32, front = ctx.r4.u32;
  __imp__sub_8376A9F8(ctx, base);
  if (!NativeOn()) return;
  g_stats.swaps.fetch_add(1, std::memory_order_relaxed);
  ng2::native::RendererSwap(dev, front);
  WaitForFlip(dev);
  LogStats();
}

// ---- Ring and fences ------------------------------------------------------------------------

// Shader constant upload into the ring, called from sub_83768758. Ring only.
NG2_IMP(83768568);
extern "C" REX_FUNC(sub_83768568) {
  if (!NativeOn()) return __imp__sub_83768568(ctx, base);
}

// Segment kick: writes an INDIRECT_BUFFER to the primary ring and bumps CP_RB_WPTR. Dropped, which
// is what keeps the command processor empty.
extern "C" REX_FUNC(sub_8375F428) {
  if (!NativeOn()) return __imp__sub_8375F428(ctx, base);
  g_stats.kicks.fetch_add(1, std::memory_order_relaxed);
}

// Emit fence: the packet would have the GPU store the fence and read position in the write-back
// block. Nothing runs later, so they are stored now.
extern "C" REX_FUNC(sub_8375F7C0) {
  if (!NativeOn()) return __imp__sub_8375F7C0(ctx, base);
  const uint32_t dev = ctx.r3.u32;
  const uint32_t fence = Load(dev + kDevFence);
  const uint32_t position = Load(dev + kDevCursor) | (Load(dev + kDevFenceTag) & 3);
  const uint32_t block = Load(dev + kDevGpuBlock);
  __imp__sub_8375F7C0(ctx, base);
  if (block) {
    Store(block, fence);
    Store(block + 4, position);
  }
  g_stats.fences.fetch_add(1, std::memory_order_relaxed);
}

// InsertCallback(dev, cursor, type, callback, context) -> cursor. The GPU would raise an interrupt
// when it reached this point; it runs now, on this thread, so timestamps are real host times.
extern "C" REX_FUNC(sub_8375FCE0) {
  if (!NativeOn()) return __imp__sub_8375FCE0(ctx, base);
  const uint32_t cursor = ctx.r4.u32;
  const uint32_t type = ctx.r5.u32;
  const uint32_t callback = ctx.r6.u32;
  if (((type & 6) == 0 || (type & 2)) && callback) {
    g_stats.callbacks.fetch_add(1, std::memory_order_relaxed);
    PPCContext saved = ctx;
    ctx.r3.u64 = saved.r7.u32;
    rex::runtime::ResolveIndirectFunction(callback)(ctx, base);
    ctx = saved;
  }
  ctx.r3.u64 = cursor;
}

// Shader GPR split: the three INDIRECT_BUFFERs are pipeline drains (#97).
extern "C" REX_FUNC(sub_837613A0) {
  if (!NativeOn()) return __imp__sub_837613A0(ctx, base);
  g_stats.drops.fetch_add(1, std::memory_order_relaxed);
}

// KickOff and BlockOnFence stay as they are: fences complete in sub_8375F7C0, and the kick is what
// recycles segment memory.
extern "C" REX_FUNC(sub_8375FB08) { __imp__sub_8375FB08(ctx, base); }
extern "C" REX_FUNC(sub_8375FC08) { __imp__sub_8375FC08(ctx, base); }

// BlockUntilIdle: nothing is ever outstanding. The kick recycles the segment.
extern "C" REX_FUNC(sub_8375FF08) {
  if (!NativeOn()) return __imp__sub_8375FF08(ctx, base);
  __imp__sub_8375FB08(ctx, base);
}
