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
#include <rex/input/mnk/mnk_input_driver.h>
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

// Replaces sub_83774818, the audio engine's per-hardware-thread barrier (called from
// sub_837746E0 by the mixer helper thread and the audio worker threads).
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
// Options > Recomp Settings pane. The game's copyright pane draw (0x836D6070) runs every
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

// D3D present (0x8376A9F8) entry: one call per game frame, for the FPS counter.
namespace ng2 {
void RecompCountGameFrame();
}
// ---- Ultrawide (experimental) -------------------------------------------------------
// The game's only 16:9 float (0x8228EBDC, findings #31) is the aspect argument of the 3D
// projection builder sub_822F9E40 (PerspectiveFov(fovY, aspect, near, far)) and is
// returned by sub_837EC688. Overwriting it widens the horizontal FOV (Hor+).
REXCVAR_DEFINE_DOUBLE(ng2_aspect_ratio, 0.0, "NG2",
                      "Experimental: override the game's 16:9 aspect constant (0 = off)");

namespace {
constexpr uint32_t kAspectConstant = 0x8228EBDC;
// Wide aspect for the camera (0 = native 16:9). The constant itself stays 16:9 except while a
// camera builds its projection (WideAspectScope in sub_822FA8B8): every other reader (screen
// effects that size themselves from it showed black bands, findings #37/#53) sees 16:9.
std::atomic<float> g_wide_aspect{0.0f};
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
  const float want = wide ? float(aspect) : 0.0f;
  if (g_wide_aspect.exchange(want) != want) {
    REXLOG_INFO("NG2 ultrawide: camera aspect {:.4f} (constant {:08X} stays 16:9 elsewhere)",
                wide ? want : 16.0f / 9.0f, kAspectConstant);
  }
}
}  // namespace

// Some readers of the 16:9 constant size full-screen effects, which must match the real render
// target (still 16:9 in pixels; the stretch happens at presentation). They run with the
// original value (findings #37). sub_82347700 runs only during effects (e.g. hit by a flaming
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
// Effects used to switch the constant back to 16:9 while they ran; it is 16:9 by default now,
// so this only documents which readers must not see the wide value.
struct NativeAspectScope {};
// The camera's projection is the one reader that gets the wide value (Hor+).
struct WideAspectScope {
  bool active;
  WideAspectScope() {
    const float wide = g_wide_aspect.load();
    active = wide > 0.0f;
    if (active) WriteAspectConstant(wide);
  }
  ~WideAspectScope() {
    if (active) WriteAspectConstant(16.0f / 9.0f);
  }
};
}  // namespace
extern "C" REX_FUNC(__imp__sub_82347700);
extern "C" REX_FUNC(sub_82347700) {
  static int calls = 0;
  if (calls++ % 300 == 0) REXLOG_INFO("NG2 ultrawide: effect sub_82347700 ran ({} calls)", calls);
  NativeAspectScope native;
  __imp__sub_82347700(ctx, base);
}

void Ng2SkipCutsceneFrame();
void Ng2OnPresent() {
  ng2::RecompCountGameFrame();
  ApplyAspectOverride();
  Ng2SkipCutsceneFrame();
}

// ---- Game system sounds from the RECOMP SETTINGS pane ------------------------------------
// sub_8242AF00(r3 = id) plays one of the game's system sounds; the generic menu list code
// calls it with id 17 when its cursor moves (call at 0x8364D1D8, found with the
// NG2_TRACE_ALL function counter). The pane runs on the host UI thread, so it only queues
// ids; they are played on the game thread right after the Options menu's per-frame update
// (vtable 0x82286FC0 slot 14, sub_836D6F38), which runs whenever Options is open.
// Recompiled functions are weak aliases of __imp__sub_X, so this strong definition
// replaces the slot without codegen changes.
#include <rex/ppc/func.h>

extern "C" REX_FUNC(__imp__sub_836D6F38);
extern "C" REX_FUNC(__imp__sub_8242AF00);

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
    __imp__sub_8242AF00(ctx, base);
    REXLOG_DEBUG("NG2 recomp pane: played system sound {}", ids[i]);
  }
  ctx = saved;
}

extern "C" REX_FUNC(sub_836D6F38) {
  __imp__sub_836D6F38(ctx, base);
  Ng2DrainSystemSounds(ctx, base);
}

// ---- Ultrawide: keep 2D (HUD, menus) at 16:9 ---------------------------------------------
// ---- 2D render buckets (findings #36) -------------------------------------------------------
// sub_8368E2D8, the 2D render pass, draws 91 priority buckets in order: bucket i is a linked
// list (item +4 = next) whose head is at kBuckets + parity * 728 + 4 * i, parity =
// [kBuckets + 1456] ^ 1; each item's vtable +4 draws it. Items are drawn by sub_8368FDA0
// (sprite), sub_8368F568 (gauge) and sub_8368EA48, which call sub_8368EFE8 with the quad.
constexpr uint32_t kBuckets = 0x85325ED8;
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
// Keyboard & mouse: true while the player is in control (HUD up, no pause / inventory /
// in-game menu over it). Written by the render pass, read by the UI thread.
std::atomic<bool> g_in_gameplay{false};
}  // namespace

bool Ng2InGameplay() { return g_in_gameplay.load(std::memory_order_relaxed); }

void Ng2IngameMenuFrame(bool drawn);
void Ng2DrainSystemSounds(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(__imp__sub_8368E2D8);
extern "C" REX_FUNC(sub_8368E2D8) {
  g_item_bucket.clear();
  g_hud_active = false;
  bool ingame_menu_drawn = false;
  bool menu_over_hud = false;  // pause / inventory (49-50) or in-game menu (70-72)
  const uint32_t parity = GuestLoadU32(kBuckets + 1456) ^ 1;
  const uint32_t heads = kBuckets + parity * 728;
  for (int i = 0; i < kBucketCount; ++i) {
    int guard = 0;
    for (uint32_t item = GuestLoadU32(heads + 4 * i); item && guard < 4096;
         item = GuestLoadU32(item + 4), ++guard) {
      g_item_bucket[item] = i;
      // Every bucket holds a placeholder item (draw method 0x83705C98) on every screen; only
      // real draw items in 57-59 mean the HUD is up.
      if (i >= 57 && i <= 59 && GuestLoadU32(GuestLoadU32(item) + 4) != 0x83705C98) {
        g_hud_active = true;
      }
      if (i >= 70 && i <= 72 && GuestLoadU32(GuestLoadU32(item) + 4) != 0x83705C98) {
        ingame_menu_drawn = true;
      }
      if (((i >= 49 && i <= 50) || (i >= 70 && i <= 72)) &&
          GuestLoadU32(GuestLoadU32(item) + 4) != 0x83705C98) {
        menu_over_hud = true;
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
  g_in_gameplay.store(g_hud_active && !menu_over_hud, std::memory_order_relaxed);
  Ng2IngameMenuFrame(ingame_menu_drawn);
  // Menu sounds requested by the Recomp Settings panel opened from the in-game menu (the
  // main Options update, which plays them otherwise, doesn't run in gameplay).
  if (ingame_menu_drawn) Ng2DrainSystemSounds(ctx, base);
  __imp__sub_8368E2D8(ctx, base);
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
extern "C" REX_FUNC(__imp__sub_8368FDA0);
extern "C" REX_FUNC(sub_8368FDA0) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8368FDA0(ctx, base);
  g_draw_bucket = -1;
}
extern "C" REX_FUNC(__imp__sub_8368F568);
extern "C" REX_FUNC(sub_8368F568) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8368F568(ctx, base);
  g_draw_bucket = -1;
}
extern "C" REX_FUNC(__imp__sub_8370D570);
extern "C" REX_FUNC(sub_8370D570) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8370D570(ctx, base);
  g_draw_bucket = -1;
}
// Untextured coloured rectangles (e.g. the red damage chunk on the health bar, bucket 59):
// sub_8368E878 builds 4 vertices (16-byte stride: x, y, z, colour) and draws them with
// sub_8368E4F0(r3 = context, r4 = vertices), not the textured quad path. Full-screen fades
// likely use it too, so only pinned (left/right) buckets are remapped; centre stays as is.
extern "C" REX_FUNC(__imp__sub_8368E878);
extern "C" REX_FUNC(sub_8368E878) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8368E878(ctx, base);
  g_draw_bucket = -1;
}
extern "C" REX_FUNC(__imp__sub_8368E4F0);
extern "C" REX_FUNC(sub_8368E4F0) {
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  const Anchor anchor = BucketAnchor(g_draw_bucket);
  if (aspect <= 16.0 / 9.0 + 1e-4 || anchor == Anchor::kCenter) {
    __imp__sub_8368E4F0(ctx, base);
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
  __imp__sub_8368E4F0(ctx, base);
  for (uint32_t i = 0; i < 4; ++i) {
    rex::memory::store_and_swap<float>(mem->TranslateVirtual<uint8_t*>(vertices + i * 16),
                                       original[i]);
  }
}
extern "C" REX_FUNC(__imp__sub_8368EA48);
extern "C" REX_FUNC(sub_8368EA48) {
  SetDrawBucket(ctx.r3.u32);
  __imp__sub_8368EA48(ctx, base);
  g_draw_bucket = -1;
}

// sub_8368EFE8(r3 = sprite, r4 = vertices) draws one 2D quad for the sprite system: 4
// vertices, 24-byte stride, xyz first, in 1280x720 pixel space (findings #32, #33). Both
// sprite paths use it: sub_8368FDA0 (vertices from sub_83690280) and sub_8368F568 (gauges,
// e.g. the health bar fill). The presenter stretches the whole 16:9 frame to the display
// aspect, so pre-squeeze x around the centre by the same factor: 2D ends up 16:9.
extern "C" REX_FUNC(__imp__sub_8368EFE8);
extern "C" REX_FUNC(sub_8368EFE8) {
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
    __imp__sub_8368EFE8(ctx, base);
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
  __imp__sub_8368EFE8(ctx, base);
  for (uint32_t i = 0; i < 4; ++i) {
    rex::memory::store_and_swap<float>(mem->TranslateVirtual<uint8_t*>(vertices + i * 24),
                                       original[i]);
  }
}

// ---- UI scene graph: ultrawide HUD anchoring + NG2 DIAG dump ----------------------------------
// sub_83704DB8(r3 = parent node, r4 = walk state) walks a UI node tree: node +0 local matrix
// (row-vector, translation at +48/+52), +96 world matrix, +116 first child, +248 next sibling.
REXCVAR_DEFINE_BOOL(ng2_ui_tree_dump, false, "NG2", "Diagnostic: log UI tree roots per call site");
extern "C" REX_FUNC(__imp__sub_83704DB8);
extern "C" REX_FUNC(sub_83704DB8) {
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
  // from a call site in the HUD screen code (0x836BFAF0-0x836CD8A8, gameplay only; menus
  // use other call sites and stay centred). The 2D draw squeezes everything around the
  // centre by 1/k; shifting a left-side widget root by -640*(k-1) first (right side +)
  // keeps its distance to the screen edge, so the HUD sits at the ultrawide edges.
  const uint32_t lr = uint32_t(ctx.lr);
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  // Superseded by render-bucket anchoring (findings #36); kept for reference, disabled.
  if (false && aspect > 16.0 / 9.0 + 1e-4 && lr >= 0x836BFAF0 && lr < 0x836CD8A8) {
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
      __imp__sub_83704DB8(ctx, base);
      rex::memory::store_and_swap<float>(mem->TranslateVirtual<uint8_t*>(root + 48), x0);
      return;
    }
  }
  __imp__sub_83704DB8(ctx, base);
}

// ---- Ultrawide: movies (FMV) at 16:9 --------------------------------------------------------
// sub_83660FC8(r3 = out, r4 = pixel rect) builds the movie player's screen quad in clip space
// (x = px / 1280 * 2 - 1), 4 vertices with a 16-byte stride, x at +0/+16/+32/+48, rewritten on
// every call (findings #34). Scale x by 1/k so the presenter's stretch leaves movies at 16:9.
extern "C" REX_FUNC(__imp__sub_83660FC8);
extern "C" REX_FUNC(sub_83660FC8) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_83660FC8(ctx, base);
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
// Text has its own render pass: sub_8370DD20 draws one string with the font engine, taking the
// string's 4x4 transform by value in r4..r10 (two floats per 64-bit register, row-major,
// row-vector, pixel space; findings #35). Squeezing x around 640 (like sprites) = scale column 0
// by 1/k and add 640 * (1 - 1/k) to the x translation. Column 0 is the high word of r4, r6, r8
// and r10 (m00, m10, m20, m30); the last row's m33 is 1 for these affine transforms.
extern "C" REX_FUNC(__imp__sub_8370DD20);
extern "C" REX_FUNC(sub_8370DD20) {
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
  __imp__sub_8370DD20(ctx, base);
}

// ---- Cheats (Recomp Settings > Cheats) -------------------------------------------------------
// sub_823E1060(r3 = profile + 720, r4 = d) answers "difficulty d is unlocked": true if any
// clear counter (u16 at block + 120, i.e. save file 0x398-0x39E: Acolyte, Warrior, Mentor,
// Master Ninja) from d-1 upward is non-zero (findings #40). Callers: the New Game path list
// (sub_8366F4D0: Mentor, Master Ninja), the costume list (sub_823E1410: one costume per
// difficulty) and an extras list (sub_823EEEE0). Answering "unlocked" for the enabled
// category unlocks the content without writing anything to the save.
REXCVAR_DEFINE_BOOL(ng2_cheat_difficulties, false, "NG2", "Cheat: unlock all difficulties");
REXCVAR_DEFINE_BOOL(ng2_cheat_costumes, false, "NG2", "Cheat: unlock all costumes");
extern "C" REX_FUNC(__imp__sub_823E1060);
extern "C" REX_FUNC(sub_823E1060) {
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t d = ctx.r4.u32;
  __imp__sub_823E1060(ctx, base);
  {
    static std::unordered_map<uint64_t, int> seen;  // NG2 DIAG: who asks, once per site/d
    if (seen[(uint64_t(lr) << 8) | d]++ == 0) {
      REXLOG_INFO("NG2 cheats: unlock check from {:08X} d={} -> {}", lr, d, ctx.r3.u32);
    }
  }
  if (ctx.r3.u32) return;
  auto in = [lr](uint32_t start, uint32_t insns) { return lr > start && lr < start + insns * 4; };
  // (sub_823EEEE0's extras list was tried too and changed nothing visible; dropped.)
  // The lists are built once when their menu is created, so changes apply after a restart.
  if ((in(0x8366F4D0, 316) && REXCVAR_GET(ng2_cheat_difficulties)) ||
      (in(0x823E1410, 893) && REXCVAR_GET(ng2_cheat_costumes))) {
    ctx.r3.u64 = 1;
  }
}


// ---- In-game Options > Recomp Settings ----------------------------------------------------------
// The in-game menu ("In-Game Manual and Options", opened with Back in gameplay) is built by
// sub_83695478 (vtable 0x822866C4 slot 0) each time it opens: 12 row objects, pointers at
// menu +8..+52, cursor (row index) at menu +20160. Row 8 (object at menu +15424) is
// NINJA CINEMA (label id 0x40017), renamed by the data patch above (findings #42).
// Row objects (sub_83693080): selected choice at +16, label text pointer at +228, choice text
// pointers at +1076 + i * 256, choice count at +1904. The menu draws in 2D buckets 70-72.
namespace {
constexpr uint32_t kIngameCursor = 20160;
constexpr uint32_t kIngameRecompRow = 8;
std::atomic<uint32_t> g_ingame_menu{0};
std::atomic<int64_t> g_ingame_visible_ms{0};
}  // namespace

extern "C" REX_FUNC(__imp__sub_83695478);
extern "C" REX_FUNC(sub_83695478) {
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_83695478(ctx, base);
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
  const uint32_t menu = g_ingame_menu.load();
  if (!drawn || !menu) return;
  g_ingame_visible_ms = NowMs();
  // Hide the row's highlight smear (it marked the old On / Off value): the row's highlight
  // group at +288 draws nothing while its selected index (+144) is -1 (sub_83691930).
  if (const uint32_t row = GuestLoadU32(menu + 8 + 4 * kIngameRecompRow)) {
    auto* mem = REX_KERNEL_STATE()->memory();
    rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual(row + 288 + 144), 0xFFFFFFFFu);
  }
}

// True while the in-game menu is on screen with its cursor on Recomp Settings.
bool Ng2IngameRecompRowSelected() {
  const uint32_t menu = g_ingame_menu.load();
  if (!menu || NowMs() - g_ingame_visible_ms.load() > 150) return false;
  return GuestLoadU32(menu + kIngameCursor) == kIngameRecompRow;
}

// ---- Skip Intro (Recomp Settings > System) -------------------------------------------------------
// Boot plays movie 0 (game:\NinjaVI.wmv, path table 0x83AB96C8) from the movie scene sub_83660E68
// via sub_83660C30(index), which uses the multi-stream player sub_836602D0 when the index has
// side streams (table 0x83AB9760; movie 0 has NinjaVI_Left/Right) and sub_83660158 otherwise.
// When a movie fails to open, both players clear its handle (0x84C557FC), set the "movie
// finished" flag (0x84A69CC4) and return 0, and the scene moves on. With ng2_skip_intro, the
// first play of movie 0 takes that same path without opening the files (findings #47).
REXCVAR_DEFINE_BOOL(ng2_skip_intro, false, "NG2", "Skip the intro movie at boot");
namespace {
std::atomic<bool> g_intro_seen{false};
// True (once) for the boot movie when Skip Intro is on; writes the players' failure state.
bool SkipBootMovie(uint32_t index, uint32_t busy_flag) {
  if (index != 0 || g_intro_seen.exchange(true) || !REXCVAR_GET(ng2_skip_intro)) return false;
  auto* mem = REX_KERNEL_STATE()->memory();
  auto put = [mem](uint32_t a, uint32_t v) {
    rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual(a), v);
  };
  put(0x84C55808, busy_flag);  // each player sets this before opening (0 / 1)
  put(0x84C557FC, 0);          // movie handle
  put(0x84A69CC4, 1);          // movie finished
  REXLOG_INFO("NG2 skip intro: boot movie skipped");
  return true;
}
}  // namespace
extern "C" REX_FUNC(__imp__sub_83660158);
extern "C" REX_FUNC(sub_83660158) {
  if (SkipBootMovie(ctx.r3.u32, 0)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_83660158(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_836602D0);
extern "C" REX_FUNC(sub_836602D0) {
  if (SkipBootMovie(ctx.r3.u32, 1)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_836602D0(ctx, base);
}

// ---- Skip Any Cutscene (Recomp Settings > System) ------------------------------------------------
// Story cutscenes are movies started by the event-script command sub_8365E760, which waits (fiber
// yields) until the movie scene reports done (0x84C55810) and, every frame, lets the player skip:
// if the script's skip argument is non-zero and that many frames have passed, the skip buttons
// are held in the game's pad state ([0x83C07FF8 + pad * 68] & 0x110, pad = byte 0x84C3DA3E) and
// the cutscene was seen before (byte 0x84C3EC4F + id, id = u16 0x84C3F0B4), it starts a 30-frame
// fade-out ([0x84C55818] = 30). ng2_skip_any_cutscene does the same skip without the seen and
// skip-argument checks.
// Real-time cutscenes run in the event-script loop sub_83655B98: while the event's skip state
// (u16 0x84A69AEA) is 1, holding the same buttons skips it if id < 384 and the seen byte
// [0x84C559E0] + 104 + id is set. That skip is a whole sequence inside the coroutine, so for these
// the hook sets the seen byte when the button is pressed (the event then ends and would be
// marked seen anyway) and the game skips it itself (findings #48).
REXCVAR_DEFINE_BOOL(ng2_skip_any_cutscene, true, "NG2", "Let any cutscene be skipped");
void Ng2SkipCutsceneFrame() {
  if (!REXCVAR_GET(ng2_skip_any_cutscene)) return;
  constexpr uint32_t kMovieState = 0x84C5580C;   // > 0 while the movie scene runs
  constexpr uint32_t kMovieDone = 0x84C55810;    // set when the movie has ended
  constexpr uint32_t kFadeOut = 0x84C55818;      // fade-out frames; reaching 1 stops the movie
  constexpr uint32_t kMovieFrames = 0x84C55828;  // frames since the movie started
  auto* mem = REX_KERNEL_STATE()->memory();
  const uint32_t pad = *mem->TranslateVirtual<uint8_t*>(0x84C3DA3E);
  if (pad >= 4 || !(GuestLoadU32(0x83C07FF8 + pad * 68) & 0x110)) return;  // skip buttons
  // Real-time cutscene (event script) in its skippable state: mark it seen.
  const uint16_t event_skip = rex::memory::load_and_swap<uint16_t>(
      mem->TranslateVirtual(0x84A69AEA));
  const uint16_t id = rex::memory::load_and_swap<uint16_t>(mem->TranslateVirtual(0x84C3F0B4));
  if (event_skip == 1 && id < 384) {
    if (const uint32_t table = GuestLoadU32(0x84C559E0)) {
      uint8_t* seen = mem->TranslateVirtual<uint8_t*>(table + 104 + id);
      if (!*seen) {
        *seen = 1;
        REXLOG_INFO("NG2 skip cutscene: event {} made skippable", id);
      }
    }
  }
  // Movie cutscene: start the game's own 30-frame fade-out.
  if (int32_t(GuestLoadU32(kMovieState)) <= 0 || GuestLoadU32(kMovieDone) ||
      GuestLoadU32(kFadeOut) || GuestLoadU32(kMovieFrames) < 15) {
    return;
  }
  rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual(kFadeOut), 30);
  REXLOG_INFO("NG2 skip cutscene: movie {} skipped", GuestLoadU32(0x84A69C74));
}

// ---- Keyboard & mouse: mouse camera ----------------------------------------------------
// The input update sub_822F2E38 (once per game frame) polls XInput and writes each pad's
// current stick floats at 0x83C07FF0 + pad*68 + 20..40 (lx, ly, rx, ry, lt, rt; clamped to
// [-1, 1], game deadzone applied) and the previous frame's at +44..64 (findings #52).
// Adding mouse motion to the right stick there was tried first: the camera's response curve
// turns large values back, and the auto-follow pulled against it. Mouse motion (SDK
// mnk_mouse_raw) now orbits the camera directly (below).
namespace ng2 {
struct MouseCameraParams {
  bool enabled;
  float sens_x, sens_y;
  bool invert_y;
  bool auto_center;  // let the game swing the camera back behind Ryu between mouse moves
};
MouseCameraParams GetMouseCameraParams();
}  // namespace ng2

// Camera update sub_823053F0(r3 = camera): reads the active pad's right stick (record +28/+32,
// pad index byte 0x84C3DA3E), applies a deadzone and the curve y = c*x^3 - x^4 (made for
// |x| <= 1; larger values turn back), then turns the camera by rebuilding its two points
// at camera+64 / camera+80 from atan2 of their difference. There are no stored yaw/pitch
// angles, and the follow-behind-Ryu logic moves the same points (findings #52).
// Mouse look orbits camera+64 around camera+80 by the mouse motion after the update, and
// while the mouse is in use it raises the "stick in use" flags the update sets for the pad
// (input block camera+816: +180, +192, +196) so the auto-follow holds off.
REXCVAR_DEFINE_BOOL(ng2_mouse_cam_log, false, "NG2", "Log mouse camera points once a second");
REXCVAR_DEFINE_INT32(ng2_mouse_yaw_sign, -1, "NG2", "Mouse camera yaw direction (1 or -1)");
REXCVAR_DEFINE_INT32(ng2_mouse_pitch_sign, 1, "NG2", "Mouse camera pitch direction (1 or -1)");

namespace {
struct GuestVec {
  float x, y, z;
};
float LoadF(uint8_t* p) {
  uint32_t b = rex::memory::load_and_swap<uint32_t>(p);
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}
void StoreF(uint8_t* p, float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  rex::memory::store_and_swap<uint32_t>(p, b);
}
GuestVec LoadVec(uint8_t* p) { return {LoadF(p), LoadF(p + 4), LoadF(p + 8)}; }
void StoreVec(uint8_t* p, const GuestVec& v) {
  StoreF(p, v.x);
  StoreF(p + 4, v.y);
  StoreF(p + 8, v.z);
}
int64_t g_mouse_used_ms = 0;
bool g_mouse_has_camera = false;  // set by the camera update below (game thread)
}  // namespace

extern "C" REX_FUNC(__imp__sub_823053F0);
extern "C" REX_FUNC(sub_823053F0) {
  const uint32_t cam = ctx.r3.u32;
  float dx = 0.0f, dy = 0.0f;
  rex::input::mnk::ConsumeMouseDelta(dx, dy);
  const ng2::MouseCameraParams p = ng2::GetMouseCameraParams();
  const bool active = p.enabled && Ng2InGameplay() && cam;
  auto* mem = REX_KERNEL_STATE()->memory();
  const int64_t now = GetTickCount64();
  if (active && (dx != 0.0f || dy != 0.0f)) g_mouse_used_ms = now;
  if (active && g_mouse_used_ms) {
    // Right stick in use (record values are after the game's deadzone): the controller
    // has the camera again, so the game's normal follow returns.
    uint32_t pad = *mem->TranslateVirtual<uint8_t*>(0x84C3DA3E);
    if (pad > 3) pad = 0;
    uint8_t* rec = mem->TranslateVirtual<uint8_t*>(0x83C07FF0 + pad * 68);
    if (LoadF(rec + 28) != 0.0f || LoadF(rec + 32) != 0.0f) g_mouse_used_ms = 0;
  }
  // Auto-center Off: hold the game's auto-follow off for as long as the mouse has the
  // camera; On: only for a moment after each mouse movement.
  const bool recent = active && g_mouse_used_ms &&
                      (!p.auto_center || now - g_mouse_used_ms < 1500);
  g_mouse_has_camera = recent;
  if (recent) {
    uint8_t* in = mem->TranslateVirtual<uint8_t*>(cam + 816);
    rex::memory::store_and_swap<uint32_t>(in + 180, 1);
    rex::memory::store_and_swap<uint32_t>(in + 192, 1);
    rex::memory::store_and_swap<uint32_t>(in + 196, 1);
  }
  __imp__sub_823053F0(ctx, base);
  if (!active) {
    g_mouse_has_camera = false;
    return;
  }
  uint8_t* eye_p = mem->TranslateVirtual<uint8_t*>(cam + 64);
  uint8_t* tgt_p = mem->TranslateVirtual<uint8_t*>(cam + 80);
  uint8_t* eye2_p = mem->TranslateVirtual<uint8_t*>(cam + 2432);
  const GuestVec eye = LoadVec(eye_p), tgt = LoadVec(tgt_p), eye2 = LoadVec(eye2_p);
  const float ox = eye.x - tgt.x, oy = eye.y - tgt.y, oz = eye.z - tgt.z;
  const float r = std::sqrt(ox * ox + oy * oy + oz * oz);
  static int64_t last_log = 0;
  if (REXCVAR_GET(ng2_mouse_cam_log) && now - last_log > 1000) {
    last_log = now;
    REXLOG_INFO("NG2 mouse cam: +64 ({:.2f} {:.2f} {:.2f}) +80 ({:.2f} {:.2f} {:.2f}) "
                "+2432 ({:.2f} {:.2f} {:.2f}) dist {:.2f} mouse {}",
                eye.x, eye.y, eye.z, tgt.x, tgt.y, tgt.z, eye2.x, eye2.y, eye2.z, r, recent);
  }
  // While the mouse has the camera, its direction is ours: whatever the game's follow logic
  // turned this frame is undone (the game's distance is kept: it pulls in for walls).
  static bool have_dir = false;
  static float yaw_state = 0.0f, elev_state = 0.0f;
  if (!recent || !(r > 1e-3f) || !std::isfinite(r)) {
    have_dir = false;
    return;
  }
  if (!have_dir) {
    yaw_state = std::atan2(ox, oz);
    elev_state = std::atan2(oy, std::sqrt(ox * ox + oz * oz));
    have_dir = true;
  }
  constexpr float kRadPerPixel = 0.0025f;  // at sensitivity 1.0
  yaw_state += dx * p.sens_x * kRadPerPixel * float(REXCVAR_GET(ng2_mouse_yaw_sign));
  elev_state += dy * p.sens_y * kRadPerPixel * (p.invert_y ? -1.0f : 1.0f) *
                float(REXCVAR_GET(ng2_mouse_pitch_sign));
  elev_state = std::clamp(elev_state, -1.40f, 1.40f);
  const float h = r * std::cos(elev_state);
  const GuestVec out{tgt.x + h * std::sin(yaw_state), tgt.y + r * std::sin(elev_state),
                     tgt.z + h * std::cos(yaw_state)};
  StoreVec(eye_p, out);
  // +2432 trails +64 slightly (a smoothed / previous-frame eye, 0.01-0.08 units behind in
  // mouselook-4); set it too so nothing eases toward the old direction.
  StoreVec(eye2_p, out);
}

// NG2 DIAG (ng2_cam_diag): log when the render camera's source fields or the gameplay
// camera's early-exit state change, to find what marks scripted cameras (executions).
REXCVAR_DEFINE_BOOL(ng2_cam_diag, false, "NG2", "Log camera source changes");
// After all cameras updated: log small-integer fields (modes, states) that change in the
// gameplay camera (slot 0, 5024 bytes) and the player object ([[0x83B28400]+1632], 1 KB).
extern "C" REX_FUNC(__imp__sub_822F7318);
extern "C" REX_FUNC(sub_822F7318) {
  if (REXCVAR_GET(ng2_cam_diag)) {
    const uint32_t mgr = ctx.r3.u32;
    const uint32_t cam = GuestLoadU32(mgr + 1604);
    const uint32_t world = GuestLoadU32(0x83B28400);
    const uint32_t player = world ? GuestLoadU32(world + 1632) : 0;
    struct Region {
      const char* name;
      uint32_t base, size;
      std::vector<uint32_t>* prev;
    };
    static std::vector<uint32_t> prev_cam, prev_player;
    static uint32_t prev_player_addr = 0;
    if (player != prev_player_addr) {
      prev_player.clear();
      prev_player_addr = player;
    }
    Region regions[] = {{"cam", cam, 5024, &prev_cam}, {"player", player, 1024, &prev_player}};
    for (auto& r : regions) {
      if (!r.base) continue;
      const uint32_t words = r.size / 4;
      std::vector<uint32_t> cur(words);
      for (uint32_t w = 0; w < words; ++w) cur[w] = GuestLoadU32(r.base + w * 4);
      if (r.prev->size() == words) {
        for (uint32_t w = 0; w < words; ++w) {
          const uint32_t a = (*r.prev)[w], b = cur[w];
          if (a != b && a < 4096 && b < 4096) {
            REXLOG_INFO("NG2 cam diag: {} {:08X} +{} {} -> {}", r.name, r.base, w * 4, a, b);
          }
        }
      }
      *r.prev = std::move(cur);
    }
  }
  __imp__sub_822F7318(ctx, base);
}

// Per-camera update sub_822FA8B8(r3 = camera; the camera manager runs it for 8 slots, slot 0
// = gameplay camera 0x84C569F0). Executions (player action 114 -> 111) switch the gameplay
// camera's mode (camera+4716, copy +2496) from 1 to 8 with flag +4640 = 1, and back when they
// end (camdiag-3, findings #52). While the mouse has the camera (gameplay, HUD up), any
// special mode (executions, Ultimate Techniques, ...) is turned back into the normal mode 1
// before the update, so the mouse keeps the camera. Cutscenes hide the HUD, so they keep
// their cameras.
extern "C" REX_FUNC(__imp__sub_822FA8B8);
extern "C" REX_FUNC(sub_822FA8B8) {
  WideAspectScope wide_projection;  // Hor+ for this camera's projection only
  const uint32_t c = ctx.r3.u32;
  const uint32_t mode = c == 0x84C569F0 ? GuestLoadU32(c + 4716) : 1;
  if (g_mouse_has_camera && Ng2InGameplay() && mode != 1 && mode < 64) {
    static uint64_t logged = 0;
    if (!(logged & (1ull << mode))) {
      logged |= 1ull << mode;
      REXLOG_INFO("NG2 mouse cam: overriding camera mode {} (player action {})", mode,
                  GuestLoadU32(GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) + 336));
    }
    auto* mem = REX_KERNEL_STATE()->memory();
    rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual<uint8_t*>(c + 4716), 1);
    rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual<uint8_t*>(c + 2496), 1);
    rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual<uint8_t*>(c + 4640), 0);
  }
  __imp__sub_822FA8B8(ctx, base);
}

// NG2 DIAG (ng2_aspect_diag): per-second call counts of the 16:9 constant's readers, to find
// the one that sizes effects showing black bars on ultrawide.
REXCVAR_DEFINE_BOOL(ng2_aspect_diag, false, "NG2", "Log calls of 16:9 constant readers");
namespace {
std::atomic<uint32_t> g_aspect_calls[7];
const char* const kAspectReaders[] = {"82330E98","82346DA8","82830448","82845BD8","837EC688","83889098","822FB368"};
void AspectDiagTick() {
  static int64_t last = 0;
  const int64_t now = GetTickCount64();
  if (now - last < 1000) return;
  last = now;
  std::string line;
  for (size_t i = 0; i < std::size(kAspectReaders); ++i) {
    line += std::string(" ") + kAspectReaders[i] + "=" + std::to_string(g_aspect_calls[i].exchange(0));
  }
  REXLOG_INFO("NG2 aspect diag:{}", line);
}
}  // namespace
extern "C" REX_FUNC(__imp__sub_82330E98);
extern "C" REX_FUNC(sub_82330E98) {
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[0].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_82330E98(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_82346DA8);
extern "C" REX_FUNC(sub_82346DA8) {
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[1].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_82346DA8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_82830448);
extern "C" REX_FUNC(sub_82830448) {
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[2].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_82830448(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_82845BD8);
// sub_82845BD8 (1.0 sub_828205F8) builds a projection from the 16:9 constant for effects:
// silent in normal play, bursts during projectile hits and Ultimate Technique charges
// (aspectdiag-1), which showed black bands with the wide value. It runs with 16:9 (#37, #53).
extern "C" REX_FUNC(sub_82845BD8) {
  NativeAspectScope native;
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[3].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_82845BD8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_837EC688);
extern "C" REX_FUNC(sub_837EC688) {
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[4].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_837EC688(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_83889098);
extern "C" REX_FUNC(sub_83889098) {
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[5].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_83889098(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_822FB368);
extern "C" REX_FUNC(sub_822FB368) {
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[6].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_822FB368(ctx, base);
}
