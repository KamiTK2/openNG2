// Host replacements for guest code that is unsafe under host scheduling.
// Wired up with [[entrypoint.midasm_hook]] entries in ng2_manifest.toml.
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/file_read_hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace {

uint32_t GuestLoadU32(uint32_t address) {
  auto* mem = REX_KERNEL_STATE()->memory();
  return rex::memory::load_and_swap<uint32_t>(mem->TranslateVirtual(address));
}

struct BarrierState {
  uint32_t arrived = 0;
  uint64_t generation = 0;
};

// ---------------------------------------------------------------------------------
// Data patches applied to file contents as the game reads them (game files on disk are
// never modified). A patch only applies if the original bytes match exactly.

struct DataPatch {
  const char* file_suffix;       // lower-case, matched against the end of the guest path
  uint64_t offset;               // file offset of the patched region
  std::u16string original;       // expected UTF-16 text at offset (NUL-terminated on disk)
  std::u16string replacement;    // new text; the rest of the original's slot is zeroed
  // Bytes available for the replacement incl. its NUL (0 = the original's length + NUL). Only
  // larger when the padding up to the next string is verified to be zero.
  size_t slot_bytes = 0;
};

const std::vector<DataPatch>& DataPatches() {
  static const std::vector<DataPatch> patches = {
      // Options menu, left column: "Copyright Information" -> "Recomp Settings".
      // (The copyright screen's own title at 0x484C0 is left alone.)
      {"other.ng2", 0x46E00, u"Copyright Information", u"Recomp Settings"},
      // In-game Options list (string pack 4, id 0x40017): "NINJA CINEMA" -> "Recomp Settings".
      // The slot runs to the next string at 0x52010 (32 bytes; zero padding is checked).
      {"other.ng2", 0x51FF0, u"NINJA CINEMA", u"Recomp Settings", 32},
  };
  return patches;
}

std::vector<uint8_t> ToUtf16Be(const std::u16string& s, size_t total_bytes) {
  std::vector<uint8_t> out(total_bytes, 0);
  for (size_t i = 0; i < s.size() && 2 * i + 1 < total_bytes; ++i) {
    out[2 * i] = uint8_t(s[i] >> 8);
    out[2 * i + 1] = uint8_t(s[i] & 0xFF);
  }
  return out;
}

bool EndsWithNoCase(const std::string& s, const char* suffix) {
  size_t n = std::strlen(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(uint8_t(s[s.size() - n + i])) != suffix[i]) return false;
  }
  return true;
}

void OnFileRead(const std::string& path, uint64_t file_offset, uint32_t guest_buffer,
                uint32_t length) {
  for (const auto& p : DataPatches()) {
    if (!EndsWithNoCase(path, p.file_suffix)) continue;
    // Slot = original text + NUL terminator.
    const size_t slot = p.slot_bytes ? p.slot_bytes : (p.original.size() + 1) * 2;
    const uint64_t lo = std::max<uint64_t>(file_offset, p.offset);
    const uint64_t hi = std::min<uint64_t>(file_offset + length, p.offset + slot);
    if (lo >= hi) continue;
    // Only patch when the whole slot is in this read, so we can verify it first.
    if (lo != p.offset || hi != p.offset + slot) {
      REXLOG_WARN("NG2 data patch {}@{:X}: partial read, skipped", p.file_suffix, p.offset);
      continue;
    }
    auto* mem = REX_KERNEL_STATE()->memory();
    auto* dst = mem->TranslateVirtual<uint8_t*>(guest_buffer + uint32_t(p.offset - file_offset));
    const auto expected = ToUtf16Be(p.original, slot);
    if (!std::equal(expected.begin(), expected.end(), dst)) {
      REXLOG_WARN("NG2 data patch {}@{:X}: original text mismatch, skipped", p.file_suffix,
                  p.offset);
      continue;
    }
    const auto repl = ToUtf16Be(p.replacement, slot);
    std::copy(repl.begin(), repl.end(), dst);
    REXLOG_INFO("NG2 data patch applied: {}@{:X} -> guest {:08X}", p.file_suffix, p.offset,
                guest_buffer + uint32_t(p.offset - file_offset));
  }
}

std::mutex g_barrier_mutex;
std::condition_variable g_barrier_cv;
std::unordered_map<uint32_t, BarrierState> g_barriers;  // keyed by guest slot address

}  // namespace

// Replaces sub_8374F078, the audio engine's per-hardware-thread barrier (called from
// sub_8374EF40 by the mixer helper thread and the audio worker threads).
//
// Guest version: each participant sets byte [slot + KPCR.current_cpu] = 1, then spins
// until the 8-byte slot equals the expected per-CPU pattern (engine+308..+328) or 0;
// whoever sees the full pattern clears the slot. Successive barriers can reuse the same
// slot back to back, so a participant that is descheduled while the slot is briefly
// full and then cleared (and refilled by the next barrier) never sees a release and
// spins forever, while the others wait for it in the next barrier. On a 360 each thread
// owns a hardware thread and the window is never missed; on a PC it deadlocks the audio
// callback within minutes (notes\findings.md #18/#20).
//
// Host version: same participant count, but a generation counter per slot, so a late
// waker always observes its release.
void Ng2AudioBarrier(PPCRegister& r3, PPCRegister& r4) {
  const uint32_t engine = r3.u32;
  const uint32_t slot = r4.u32;

  // Single-threaded mixing: the guest returns immediately.
  if (GuestLoadU32(engine + 304) == 0) {
    return;
  }
  uint32_t participants = 0;
  for (uint32_t offset = 308; offset <= 328; offset += 4) {
    if (GuestLoadU32(engine + offset) != 0) {
      ++participants;
    }
  }
  if (participants <= 1) {
    return;
  }

  std::unique_lock<std::mutex> lock(g_barrier_mutex);
  BarrierState& state = g_barriers[slot];
  const uint64_t my_generation = state.generation;
  if (++state.arrived >= participants) {
    state.arrived = 0;
    ++state.generation;
    lock.unlock();
    g_barrier_cv.notify_all();
    return;
  }
  g_barrier_cv.wait(lock, [&] { return state.generation != my_generation; });
}

// Register the data-patch file hook (called once at startup from Ng2App).
void Ng2InstallDataPatches() { rex::kernel::SetFileReadHook(&OnFileRead); }


// ---------------------------------------------------------------------------------
// Options > Recomp Settings pane. The game's copyright pane draw (0x836B0B58) runs every
// frame while the item is highlighted; we skip it and note the time so the app can draw
// the RECOMP SETTINGS pane in its place.
#include <atomic>
#include <chrono>

namespace {
std::atomic<int64_t> g_recomp_pane_last_ms{0};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

void Ng2RecompPaneDraw(PPCRegister& r3) {
  (void)r3;
  g_recomp_pane_last_ms.store(NowMs());
}

// True while the Recomp Settings item is highlighted in Options (pane drawn recently).
bool Ng2RecompPaneVisible() { return NowMs() - g_recomp_pane_last_ms.load() < 150; }

// D3D present (0x837452C0) entry: one call per game frame, for the FPS counter.
namespace ng2 {
void RecompCountGameFrame();
}
// ---- Ultrawide (experimental) -------------------------------------------------------
// The game's only 16:9 float (0x8228925C, findings #31) is the aspect argument of the 3D
// projection builder sub_822F9E40 (PerspectiveFov(fovY, aspect, near, far)) and is
// returned by sub_837C6F40. Overwriting it widens the horizontal FOV (Hor+).
REXCVAR_DEFINE_DOUBLE(ng2_aspect_ratio, 0.0, "NG2",
                      "Experimental: override the game's 16:9 aspect constant (0 = off)");

namespace {
constexpr uint32_t kAspectConstant = 0x8228925C;
void ApplyAspectOverride() {
  // 0 (or anything up to 16:9) = the game's own 16:9: restore the constant and the
  // presenter, so the setting can be switched back live.
  double aspect = REXCVAR_GET(ng2_aspect_ratio);
  const bool wide = aspect > 16.0 / 9.0 + 1e-4;
  // The game still renders a 16:9 frame; the presenter stretches it to `aspect`.
  static double presented = -1.0;
  if (presented != (wide ? aspect : 0.0)) {
    presented = wide ? aspect : 0.0;
    rex::cvar::SetFlagByName("present_aspect_ratio", std::to_string(presented));
  }
  auto* mem = REX_KERNEL_STATE()->memory();
  auto* p = mem->TranslateVirtual<uint8_t*>(kAspectConstant);
  const float want = wide ? float(aspect) : 16.0f / 9.0f;
  if (rex::memory::load_and_swap<float>(p) != want) {
    // The constant is in the image's read-only data: unprotect the host page briefly.
    DWORD old_protect = 0;
    if (!VirtualProtect(p, sizeof(float), PAGE_READWRITE, &old_protect)) {
      REXLOG_WARN("NG2 ultrawide: cannot unprotect the aspect constant");
      return;
    }
    rex::memory::store_and_swap<float>(p, want);
    VirtualProtect(p, sizeof(float), old_protect, &old_protect);
    REXLOG_INFO("NG2 ultrawide: aspect constant {:08X} set to {:.4f}", kAspectConstant, want);
  }
}
}  // namespace

// Some readers of the 16:9 constant size full-screen effects, which must match the real render
// target (still 16:9 in pixels; the stretch happens at presentation). They run with the
// original value (findings #37). sub_82347288 runs only during effects (e.g. hit by a flaming
// arrow: black band at the top with the wide value).
namespace {
void WriteAspectConstant(float value) {
  auto* p = REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(kAspectConstant);
  if (rex::memory::load_and_swap<float>(p) == value) return;
  DWORD old_protect = 0;
  if (!VirtualProtect(p, sizeof(float), PAGE_READWRITE, &old_protect)) return;
  rex::memory::store_and_swap<float>(p, value);
  VirtualProtect(p, sizeof(float), old_protect, &old_protect);
}
struct NativeAspectScope {
  float saved;
  NativeAspectScope() {
    saved = rex::memory::load_and_swap<float>(
        REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(kAspectConstant));
    WriteAspectConstant(16.0f / 9.0f);
  }
  ~NativeAspectScope() { WriteAspectConstant(saved); }
};
}  // namespace
extern "C" REX_FUNC(__imp__sub_82347288);
extern "C" REX_FUNC(sub_82347288) {
  static int calls = 0;
  if (calls++ % 300 == 0) REXLOG_INFO("NG2 ultrawide: effect sub_82347288 ran ({} calls)", calls);
  NativeAspectScope native;
  __imp__sub_82347288(ctx, base);
}

void Ng2OnPresent() {
  ng2::RecompCountGameFrame();
  ApplyAspectOverride();
}

// ---- Game system sounds from the RECOMP SETTINGS pane ------------------------------------
// sub_82429DA8(r3 = id) plays one of the game's system sounds; the generic menu list code
// calls it with id 17 when its cursor moves (call at 0x8364D1D8, found with the
// NG2_TRACE_ALL function counter). The pane runs on the host UI thread, so it only queues
// ids; they are played on the game thread right after the Options menu's per-frame update
// (vtable 0x82281630 slot 14, sub_836B1AB8), which runs whenever Options is open.
// Recompiled functions are weak aliases of __imp__sub_X, so this strong definition
// replaces the slot without codegen changes.
#include <rex/ppc/func.h>

extern "C" REX_FUNC(__imp__sub_836B1AB8);
extern "C" REX_FUNC(__imp__sub_82429DA8);

namespace {
constexpr int kSoundQueueSize = 8;
std::mutex g_sound_mutex;
uint32_t g_sound_queue[kSoundQueueSize];
int g_sound_count = 0;
std::atomic<int64_t> g_sound_queued_ms{0};
}  // namespace

void Ng2QueueSystemSound(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_sound_mutex);
  if (g_sound_count < kSoundQueueSize) g_sound_queue[g_sound_count++] = id;
  g_sound_queued_ms.store(NowMs());
}

void Ng2DrainSystemSounds(PPCContext& ctx, uint8_t* base) {
  uint32_t ids[kSoundQueueSize];
  int n = 0;
  {
    std::lock_guard<std::mutex> lock(g_sound_mutex);
    // Drop requests that waited too long (e.g. queued just as Options closed).
    if (NowMs() - g_sound_queued_ms.load() < 250) {
      n = g_sound_count;
      std::copy(g_sound_queue, g_sound_queue + n, ids);
    }
    g_sound_count = 0;
  }
  if (!n) return;
  // Call as if the menu code made the call here: everything the callee may clobber is
  // restored, so the caller sees the update's original return state.
  const PPCContext saved = ctx;
  for (int i = 0; i < n; ++i) {
    ctx.r3.u64 = ids[i];
    __imp__sub_82429DA8(ctx, base);
    REXLOG_DEBUG("NG2 recomp pane: played system sound {}", ids[i]);
  }
  ctx = saved;
}

extern "C" REX_FUNC(sub_836B1AB8) {
  __imp__sub_836B1AB8(ctx, base);
  Ng2DrainSystemSounds(ctx, base);
}

// ---- Ultrawide: keep 2D (HUD, menus) at 16:9 ---------------------------------------------
// ---- 2D render buckets (findings #36) -------------------------------------------------------
// sub_8366B0D8, the 2D render pass, draws 91 priority buckets in order: bucket i is a linked
// list (item +4 = next) whose head is at kBuckets + parity * 728 + 4 * i, parity =
// [kBuckets + 1456] ^ 1; each item's vtable +4 draws it. Items are drawn by sub_8366CBA0
// (sprite), sub_8366C368 (gauge) and sub_8366B848, which call sub_8366BDE8 with the quad.
constexpr uint32_t kBuckets = 0x8530B200;
constexpr int kBucketCount = 91;
REXCVAR_DEFINE_BOOL(ng2_bucket_dump, false, "NG2", "Diagnostic: log 2D draws per render bucket");
namespace {
std::unordered_map<uint32_t, int> g_item_bucket;  // game thread only (render pass)
int g_draw_bucket = -1;                           // bucket of the item being drawn
// The gameplay HUD is on screen this frame: buckets 57-59 (health bar fill) have items. Main
// menu screens reuse buckets 60/61 (main Options), so pinning is limited to HUD frames.
bool g_hud_active = false;
struct BucketStat {
  int draws = 0;
  float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
};
BucketStat g_bucket_stats[kBucketCount];
int64_t g_bucket_log_ms = 0;
}  // namespace

void Ng2IngameMenuFrame(bool drawn);
void Ng2DrainSystemSounds(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(__imp__sub_8366B0D8);
extern "C" REX_FUNC(sub_8366B0D8) {
  g_item_bucket.clear();
  g_hud_active = false;
  bool ingame_menu_drawn = false;
  const uint32_t parity = GuestLoadU32(kBuckets + 1456) ^ 1;
  const uint32_t heads = kBuckets + parity * 728;
  for (int i = 0; i < kBucketCount; ++i) {
    int guard = 0;
    for (uint32_t item = GuestLoadU32(heads + 4 * i); item && guard < 4096;
         item = GuestLoadU32(item + 4), ++guard) {
      g_item_bucket[item] = i;
      // Every bucket holds a placeholder item (draw method 0x836E1498) on every screen; only
      // real draw items in 57-59 mean the HUD is up.
      if (i >= 57 && i <= 59 && GuestLoadU32(GuestLoadU32(item) + 4) != 0x836E1498) {
        g_hud_active = true;
      }
      if (i >= 70 && i <= 72 && GuestLoadU32(GuestLoadU32(item) + 4) != 0x836E1498) {
        ingame_menu_drawn = true;
      }
      if (REXCVAR_GET(ng2_bucket_dump)) {
        static std::unordered_map<uint64_t, int> seen_methods;
        const uint32_t method = GuestLoadU32(GuestLoadU32(item) + 4);
        if (seen_methods[(uint64_t(i) << 32) | method]++ == 0) {
          REXLOG_INFO("NG2 2D bucket {} item draw method {:08X}", i, method);
        }
      }
    }
  }
  Ng2IngameMenuFrame(ingame_menu_drawn);
  // Menu sounds requested by the Recomp Settings panel opened from the in-game menu (the
  // main Options update, which plays them otherwise, doesn't run in gameplay).
  if (ingame_menu_drawn) Ng2DrainSystemSounds(ctx, base);
  __imp__sub_8366B0D8(ctx, base);
  g_draw_bucket = -1;
  if (REXCVAR_GET(ng2_bucket_dump) && NowMs() - g_bucket_log_ms >= 1000) {
    g_bucket_log_ms = NowMs();
    std::string line;
    for (int i = 0; i < kBucketCount; ++i) {
      BucketStat& b = g_bucket_stats[i];
      if (!b.draws) continue;
      char buf[96];
      std::snprintf(buf, sizeof(buf), " [b%d n=%d x%.0f-%.0f y%.0f-%.0f]", i, b.draws, b.min_x,
                    b.max_x, b.min_y, b.max_y);
      line += buf;
      b = BucketStat{};
    }
    REXLOG_INFO("NG2 2D buckets:{}", line);
  }
}

// Ultrawide HUD edge pinning by render bucket (findings #36). Buckets seen in play: 40 = karma /
// combo panel (left), 57-61 = health bar, ki (left); menus use 20, 49-50, 70-72, 80-86. Only
// known HUD buckets are pinned; everything else stays a centred 16:9 block.
REXCVAR_DEFINE_BOOL(ng2_hud_anchor, true, "NG2",
                    "Ultrawide: pin the HUD to the screen edges (off = centred 16:9 HUD)");
namespace {
enum class Anchor { kCenter, kLeft, kRight };
Anchor BucketAnchor(int bucket) {
  if (!REXCVAR_GET(ng2_hud_anchor) || !g_hud_active) return Anchor::kCenter;
  if (bucket == 40 || (bucket >= 57 && bucket <= 61)) return Anchor::kLeft;
  return Anchor::kCenter;
}
// x in 1280-wide space -> pre-stretch x. Centre: squeeze around 640; left/right: keep the
// 16:9 distance to that screen edge.
float MapX(float x, float inv_k, Anchor a) {
  switch (a) {
    case Anchor::kLeft: return x * inv_k;
    case Anchor::kRight: return 1280.0f - (1280.0f - x) * inv_k;
    default: return 640.0f + (x - 640.0f) * inv_k;
  }
}
}  // namespace

namespace {
void SetDrawBucket(uint32_t item) {
  auto it = g_item_bucket.find(item);
  g_draw_bucket = it == g_item_bucket.end() ? -1 : it->second;
}
}  // namespace
extern "C" REX_FUNC(__imp__sub_8366CBA0);
extern "C" REX_FUNC(sub_8366CBA0) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8366CBA0(ctx, base);
  g_draw_bucket = -1;
}
extern "C" REX_FUNC(__imp__sub_8366C368);
extern "C" REX_FUNC(sub_8366C368) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8366C368(ctx, base);
  g_draw_bucket = -1;
}
extern "C" REX_FUNC(__imp__sub_836E8D70);
extern "C" REX_FUNC(sub_836E8D70) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_836E8D70(ctx, base);
  g_draw_bucket = -1;
}
// Untextured coloured rectangles (e.g. the red damage chunk on the health bar, bucket 59):
// sub_8366B678 builds 4 vertices (16-byte stride: x, y, z, colour) and draws them with
// sub_8366B2F0(r3 = context, r4 = vertices), not the textured quad path. Full-screen fades
// likely use it too, so only pinned (left/right) buckets are remapped; centre stays as is.
extern "C" REX_FUNC(__imp__sub_8366B678);
extern "C" REX_FUNC(sub_8366B678) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8366B678(ctx, base);
  g_draw_bucket = -1;
}
extern "C" REX_FUNC(__imp__sub_8366B2F0);
extern "C" REX_FUNC(sub_8366B2F0) {
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  const Anchor anchor = BucketAnchor(g_draw_bucket);
  if (aspect <= 16.0 / 9.0 + 1e-4 || anchor == Anchor::kCenter) {
    __imp__sub_8366B2F0(ctx, base);
    return;
  }
  const float inv_k = float((16.0 / 9.0) / aspect);
  const uint32_t vertices = ctx.r4.u32;
  auto* mem = REX_KERNEL_STATE()->memory();
  float original[4];
  for (uint32_t i = 0; i < 4; ++i) {
    auto* px = mem->TranslateVirtual<uint8_t*>(vertices + i * 16);
    original[i] = rex::memory::load_and_swap<float>(px);
    rex::memory::store_and_swap<float>(px, std::floor(MapX(original[i], inv_k, anchor) + 0.5f));
  }
  __imp__sub_8366B2F0(ctx, base);
  for (uint32_t i = 0; i < 4; ++i) {
    rex::memory::store_and_swap<float>(mem->TranslateVirtual<uint8_t*>(vertices + i * 16),
                                       original[i]);
  }
}
extern "C" REX_FUNC(__imp__sub_8366B848);
extern "C" REX_FUNC(sub_8366B848) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8366B848(ctx, base);
  g_draw_bucket = -1;
}

// sub_8366BDE8(r3 = sprite, r4 = vertices) draws one 2D quad for the sprite system: 4
// vertices, 24-byte stride, xyz first, in 1280x720 pixel space (findings #32, #33). Both
// sprite paths use it: sub_8366CBA0 (vertices from sub_8366D080) and sub_8366C368 (gauges,
// e.g. the health bar fill). The presenter stretches the whole 16:9 frame to the display
// aspect, so pre-squeeze x around the centre by the same factor: 2D ends up 16:9.
extern "C" REX_FUNC(__imp__sub_8366BDE8);
extern "C" REX_FUNC(sub_8366BDE8) {
  const uint32_t vertices = ctx.r4.u32;
  if (REXCVAR_GET(ng2_bucket_dump) && g_draw_bucket < 0) {
    static std::unordered_map<uint32_t, int> seen_lr;
    const uint32_t lr = uint32_t(ctx.lr);
    if (seen_lr[lr]++ % 600 == 0) {
      auto* m = REX_KERNEL_STATE()->memory();
      REXLOG_INFO("NG2 2D untagged quad draw from {:08X} (x={:.0f} y={:.0f}), {} so far", lr,
                  rex::memory::load_and_swap<float>(m->TranslateVirtual(vertices)),
                  rex::memory::load_and_swap<float>(m->TranslateVirtual(vertices + 4)),
                  seen_lr[lr]);
    }
  }
  if (REXCVAR_GET(ng2_bucket_dump) && g_draw_bucket >= 0) {
    auto* m = REX_KERNEL_STATE()->memory();
    BucketStat& b = g_bucket_stats[g_draw_bucket];
    ++b.draws;
    for (uint32_t i = 0; i < 4; ++i) {
      const float x = rex::memory::load_and_swap<float>(m->TranslateVirtual(vertices + i * 24));
      const float y = rex::memory::load_and_swap<float>(m->TranslateVirtual(vertices + i * 24 + 4));
      b.min_x = std::min(b.min_x, x);
      b.max_x = std::max(b.max_x, x);
      b.min_y = std::min(b.min_y, y);
      b.max_y = std::max(b.max_y, y);
    }
  }
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  if (aspect <= 16.0 / 9.0 + 1e-4) {
    __imp__sub_8366BDE8(ctx, base);
    return;
  }
  const float inv_k = float((16.0 / 9.0) / aspect);
  auto* mem = REX_KERNEL_STATE()->memory();
  const Anchor anchor = BucketAnchor(g_draw_bucket);
  // Squeeze for the draw, then restore: some callers keep their vertices across frames.
  float original[4];
  for (uint32_t i = 0; i < 4; ++i) {
    auto* px = mem->TranslateVirtual<uint8_t*>(vertices + i * 24);
    original[i] = rex::memory::load_and_swap<float>(px);
    rex::memory::store_and_swap<float>(
        px, std::floor(MapX(original[i], inv_k, anchor) + 0.5f));
  }
  __imp__sub_8366BDE8(ctx, base);
  for (uint32_t i = 0; i < 4; ++i) {
    rex::memory::store_and_swap<float>(mem->TranslateVirtual<uint8_t*>(vertices + i * 24),
                                       original[i]);
  }
}

// ---- UI scene graph: ultrawide HUD anchoring + NG2 DIAG dump ----------------------------------
// sub_836E05B8(r3 = parent node, r4 = walk state) walks a UI node tree: node +0 local matrix
// (row-vector, translation at +48/+52), +96 world matrix, +116 first child, +248 next sibling.
REXCVAR_DEFINE_BOOL(ng2_ui_tree_dump, false, "NG2", "Diagnostic: log UI tree roots per call site");
extern "C" REX_FUNC(__imp__sub_836E05B8);
extern "C" REX_FUNC(sub_836E05B8) {
  if (REXCVAR_GET(ng2_ui_tree_dump)) {
    static std::mutex dump_mutex;
    static std::unordered_map<uint64_t, int> seen;
    const uint32_t lr = uint32_t(ctx.lr), root = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(dump_mutex);
    int& n = seen[(uint64_t(lr) << 32) | root];
    if (n++ == 0) {
      auto* mem = REX_KERNEL_STATE()->memory();
      auto f32 = [&](uint32_t a) { return rex::memory::load_and_swap<float>(mem->TranslateVirtual(a)); };
      std::string line;
      int count = 0;
      for (uint32_t c = GuestLoadU32(root + 116); c && count < 40; c = GuestLoadU32(c + 248), ++count) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), " [%08X x=%.0f y=%.0f wx=%.0f kids=%d]", c, f32(c + 48),
                      f32(c + 52), f32(c + 144), GuestLoadU32(c + 116) ? 1 : 0);
        line += buf;
      }
      REXLOG_INFO("NG2 UI tree: lr {:08X} root {:08X} root x={:.0f} children {}:{}", lr, root,
                  f32(root + 48), count, line);
    }
  }
  // Ultrawide HUD anchoring (findings #33). Every HUD widget is its own small tree walked
  // from a call site in the HUD screen code (0x8369C000-0x836A8000, gameplay only; menus
  // use other call sites and stay centred). The 2D draw squeezes everything around the
  // centre by 1/k; shifting a left-side widget root by -640*(k-1) first (right side +)
  // keeps its distance to the screen edge, so the HUD sits at the ultrawide edges.
  const uint32_t lr = uint32_t(ctx.lr);
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  // Superseded by render-bucket anchoring (findings #36); kept for reference, disabled.
  if (false && aspect > 16.0 / 9.0 + 1e-4 && lr >= 0x8369C000 && lr < 0x836A8000) {
    auto* mem = REX_KERNEL_STATE()->memory();
    const uint32_t root = ctx.r3.u32;
    auto* root_x = mem->TranslateVirtual<uint8_t*>(root + 48);
    const float x0 = rex::memory::load_and_swap<float>(root_x);
    float content_x = x0;
    if (uint32_t child = GuestLoadU32(root + 116)) {
      content_x += rex::memory::load_and_swap<float>(mem->TranslateVirtual(child + 48));
    }
    const float shift = float(640.0 * (aspect / (16.0 / 9.0) - 1.0));
    const float dx = content_x < 1280.0f / 3.0f ? -shift : content_x > 2560.0f / 3.0f ? shift : 0.0f;
    if (dx != 0.0f) {
      rex::memory::store_and_swap<float>(root_x, x0 + dx);
      __imp__sub_836E05B8(ctx, base);
      rex::memory::store_and_swap<float>(mem->TranslateVirtual<uint8_t*>(root + 48), x0);
      return;
    }
  }
  __imp__sub_836E05B8(ctx, base);
}

// ---- Ultrawide: movies (FMV) at 16:9 --------------------------------------------------------
// sub_8363E5E8(r3 = out, r4 = pixel rect) builds the movie player's screen quad in clip space
// (x = px / 1280 * 2 - 1), 4 vertices with a 16-byte stride, x at +0/+16/+32/+48, rewritten on
// every call (findings #34). Scale x by 1/k so the presenter's stretch leaves movies at 16:9.
extern "C" REX_FUNC(__imp__sub_8363E5E8);
extern "C" REX_FUNC(sub_8363E5E8) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_8363E5E8(ctx, base);
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  if (aspect <= 16.0 / 9.0 + 1e-4) return;
  const float inv_k = float((16.0 / 9.0) / aspect);
  auto* mem = REX_KERNEL_STATE()->memory();
  for (uint32_t i = 0; i < 4; ++i) {
    auto* px = mem->TranslateVirtual<uint8_t*>(out + i * 16);
    rex::memory::store_and_swap<float>(px, rex::memory::load_and_swap<float>(px) * inv_k);
  }
}

// ---- Ultrawide: text at 16:9 -------------------------------------------------------------------
// Text has its own render pass: sub_836E9520 draws one string with the font engine, taking the
// string's 4x4 transform by value in r4..r10 (two floats per 64-bit register, row-major,
// row-vector, pixel space; findings #35). Squeezing x around 640 (like sprites) = scale column 0
// by 1/k and add 640 * (1 - 1/k) to the x translation. Column 0 is the high word of r4, r6, r8
// and r10 (m00, m10, m20, m30); the last row's m33 is 1 for these affine transforms.
extern "C" REX_FUNC(__imp__sub_836E9520);
extern "C" REX_FUNC(sub_836E9520) {
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  if (aspect > 16.0 / 9.0 + 1e-4) {
    const float inv_k = float((16.0 / 9.0) / aspect);
    auto scale_hi = [&](PPCRegister& r, float add) {
      uint32_t bits = uint32_t(r.u64 >> 32);
      float v;
      std::memcpy(&v, &bits, 4);
      v = v * inv_k + add;
      std::memcpy(&bits, &v, 4);
      r.u64 = (uint64_t(bits) << 32) | (r.u64 & 0xFFFFFFFFull);
    };
    scale_hi(ctx.r4, 0.0f);
    scale_hi(ctx.r6, 0.0f);
    scale_hi(ctx.r8, 0.0f);
    const Anchor anchor = BucketAnchor(g_draw_bucket);
    // m30' = MapX(m30): MapX is affine (x * inv_k + c), c = MapX(0).
    scale_hi(ctx.r10, MapX(0.0f, inv_k, anchor));
  }
  __imp__sub_836E9520(ctx, base);
}

// ---- Cheats (Recomp Settings > Cheats) -------------------------------------------------------
// sub_823E0CC0(r3 = profile + 720, r4 = d) answers "difficulty d is unlocked": true if any
// clear counter (u16 at block + 120, i.e. save file 0x398-0x39E: Acolyte, Warrior, Mentor,
// Master Ninja) from d-1 upward is non-zero (findings #40). Callers: the New Game path list
// (sub_8364CBE8: Mentor, Master Ninja), the costume list (sub_823E1070: one costume per
// difficulty) and an extras list (sub_823EEB38). Answering "unlocked" for the enabled
// category unlocks the content without writing anything to the save.
REXCVAR_DEFINE_BOOL(ng2_cheat_difficulties, false, "NG2", "Cheat: unlock all difficulties");
REXCVAR_DEFINE_BOOL(ng2_cheat_costumes, false, "NG2", "Cheat: unlock all costumes");
extern "C" REX_FUNC(__imp__sub_823E0CC0);
extern "C" REX_FUNC(sub_823E0CC0) {
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t d = ctx.r4.u32;
  __imp__sub_823E0CC0(ctx, base);
  {
    static std::unordered_map<uint64_t, int> seen;  // NG2 DIAG: who asks, once per site/d
    if (seen[(uint64_t(lr) << 8) | d]++ == 0) {
      REXLOG_INFO("NG2 cheats: unlock check from {:08X} d={} -> {}", lr, d, ctx.r3.u32);
    }
  }
  if (ctx.r3.u32) return;
  auto in = [lr](uint32_t start, uint32_t insns) { return lr > start && lr < start + insns * 4; };
  // (sub_823EEB38's extras list was tried too and changed nothing visible; dropped.)
  // The lists are built once when their menu is created, so changes apply after a restart.
  if ((in(0x8364CBE8, 186) && REXCVAR_GET(ng2_cheat_difficulties)) ||
      (in(0x823E1070, 893) && REXCVAR_GET(ng2_cheat_costumes))) {
    ctx.r3.u64 = 1;
  }
}


// ---- In-game Options > Recomp Settings ----------------------------------------------------------
// The in-game menu ("In-Game Manual and Options", opened with Back in gameplay) is built by
// sub_836722A8 (vtable 0x82280D34 slot 0) each time it opens: 12 row objects, pointers at
// menu +8..+52, cursor (row index) at menu +20160. Row 8 (object at menu +15424) is
// NINJA CINEMA (label id 0x40017), renamed by the data patch above (findings #42).
// Row objects (sub_8366FCA0): selected choice at +16, label text pointer at +228, choice text
// pointers at +1076 + i * 256, choice count at +1904. The menu draws in 2D buckets 70-72.
namespace {
constexpr uint32_t kIngameCursor = 20160;
constexpr uint32_t kIngameRecompRow = 8;
std::atomic<uint32_t> g_ingame_menu{0};
std::atomic<int64_t> g_ingame_visible_ms{0};
}  // namespace

extern "C" REX_FUNC(__imp__sub_836722A8);
extern "C" REX_FUNC(sub_836722A8) {
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_836722A8(ctx, base);
  g_ingame_menu = menu;
  // Blank the row's On / Off (the old NINJA CINEMA value): point every choice at the label's
  // NUL terminator. Left/Right never reach the game on this row (input filter), so the value
  // itself is never changed.
  auto* mem = REX_KERNEL_STATE()->memory();
  const uint32_t row = GuestLoadU32(menu + 8 + 4 * kIngameRecompRow);
  const uint32_t label = row ? GuestLoadU32(row + 228) : 0;
  if (label) {
    uint32_t end = label;
    while (end < label + 256 &&
           rex::memory::load_and_swap<uint16_t>(mem->TranslateVirtual(end)) != 0) {
      end += 2;
    }
    const uint32_t count = GuestLoadU32(row + 1904);
    for (uint32_t i = 0; i < count && i < 4; ++i) {
      rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual(row + 1076 + i * 256), end);
    }
  }
  REXLOG_INFO("NG2 in-game menu built: {:08X}", menu);
}

// Called once per game frame from the 2D render pass with whether buckets 70-72 hold items.
void Ng2IngameMenuFrame(bool drawn) {
  if (drawn && g_ingame_menu.load()) g_ingame_visible_ms = NowMs();
}

// True while the in-game menu is on screen with its cursor on Recomp Settings.
bool Ng2IngameRecompRowSelected() {
  const uint32_t menu = g_ingame_menu.load();
  if (!menu || NowMs() - g_ingame_visible_ms.load() > 150) return false;
  return GuestLoadU32(menu + kIngameCursor) == kIngameRecompRow;
}
