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
#include <rex/graphics/frame_stats.h>
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

// Fast Loading: time of the last read from a game archive (*.ng2; music streams excluded).
std::atomic<int64_t> g_last_archive_read_ms{0};
std::atomic<bool> g_loading_screen{false};  // loading screen sprites (bucket 86), no HUD
std::atomic<bool> g_chapter_screen{false};  // chapter title screen text (bucket 85)

void OnFileRead(const std::string& path, uint64_t file_offset, uint32_t guest_buffer,
                uint32_t length) {
  if (EndsWithNoCase(path, ".ng2")) g_last_archive_read_ms = int64_t(GetTickCount64());
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
// 80 ms: two frames at 30 fps. 150 ms left the pane up visibly after leaving Options (GitHub #7);
// the pane's focus survives short gaps on its own (RecompOptionsPane::OnDraw, 400 ms).
bool Ng2RecompPaneVisible() { return NowMs() - g_recomp_pane_last_ms.load() < 80; }

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
// Wide aspect for the camera (0 = native 16:9). Applied where g_aspect_scope_mode says (default:
// everywhere; effects that must stay 16:9 use NativeAspectScope).
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
// Readers that size full-screen effects run with the native 16:9 value (findings #37/#53).
struct NativeAspectScope {
  float saved;
  NativeAspectScope() {
    saved = rex::memory::load_and_swap<float>(
        REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(kAspectConstant));
    WriteAspectConstant(16.0f / 9.0f);
  }
  ~NativeAspectScope() { WriteAspectConstant(saved); }
};
// The camera's projection is the one reader that gets the wide value (Hor+).
// Where the wide value applies. 2 = everywhere (the constant stays wide; default): narrower
// scopes made the game's visibility culling 16:9, so objects popped in at the sides
// (popin-1, findings #56). NG2 DIAG (ng2_aspect_cycle, F9) cycles: 0 = main projection only
// (sub_822FB368), 1 = whole camera update (sub_822FA8B8), 2 = everywhere, 3 = plain 16:9.
std::atomic<int> g_aspect_scope_mode{2};
}  // namespace
REXCVAR_DEFINE_BOOL(ng2_aspect_cycle, false, "NG2", "Diagnostic: F9 cycles the ultrawide aspect scope");
const char* Ng2AspectTestModeName() {
  static const char* const kNames[] = {"Aspect test A: main projection wide",
                                       "Aspect test B: camera update wide",
                                       "Aspect test C: wide everywhere (old)",
                                       "Aspect test D: plain 16:9 view"};
  return REXCVAR_GET(ng2_aspect_cycle) ? kNames[g_aspect_scope_mode.load() & 3] : nullptr;
}
namespace {
struct WideAspectScope {
  bool active;
  explicit WideAspectScope(int mode) {
    const float wide = g_wide_aspect.load();
    active = wide > 0.0f && g_aspect_scope_mode.load() == mode;
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
bool Ng2InGameplay();  // defined with the 2D render pass below
REXCVAR_DEFINE_BOOL(ng2_thread_cpu, false, "NG2", "NG2 DIAG: log CPU use per thread every 5 s");
REXCVAR_DEFINE_BOOL(ng2_spin_yield, true, "NG2",
                    "Let the game's busy-wait loops sleep briefly instead of spinning a core");
// Busy waits (findings #64): NG2's main loop spins until the render thread finishes a frame,
// and the render thread spins in the D3D runtime until the GPU frees command ring space, each
// burning a whole core. A 100 us high-resolution sleep per iteration frees the core for the
// command processor and the rest (weak CPUs) at a negligible latency cost.
void Ng2SpinYield() {
  if (!REXCVAR_GET(ng2_spin_yield)) return;
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) {
    SwitchToThread();
    return;
  }
  LARGE_INTEGER due;
  due.QuadPart = -1000;  // 100 us, relative (100 ns units)
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer, INFINITE);
  }
}
// The D3D runtime's wait-poll helper (r3 = wait state): returns 1 while still waiting.
extern "C" REX_FUNC(__imp__sub_8375E538);
extern "C" REX_FUNC(sub_8375E538) {
  __imp__sub_8375E538(ctx, base);
  if (ctx.r3.u32 == 1) Ng2SpinYield();
}
REXCVAR_DEFINE_BOOL(ng2_fast_loading, true, "NG2",
                    "Run loading screens unthrottled (guest vblank off while loading)");
void Ng2OnPresent() {
  ng2::RecompCountGameFrame();
  ApplyAspectOverride();
  {
    // Fast Loading (findings #57): NG2 loads a fixed amount of work per frame while its
    // loading screen animates, and the death / retry load also waits ~10 s with no reads at
    // all, so loads take a set number of 60 Hz frames. While the loading icon (2D bucket 86:
    // petals -> shuriken, NOW LOADING) is up without the HUD, the guest vblank runs
    // unthrottled (cvar vsync off: 1 ms vblanks) and the load finishes in a fraction of the time.
    // Latched: on when the icon appears, off when the chapter screen ([Proceed]) or the HUD
    // appears, or 15 s after the icon went away (the fade after the icon is a wait too).
    const int64_t now = int64_t(GetTickCount64());
    static bool latched = false;
    static int64_t icon_seen_ms = 0;
    if (REXCVAR_GET(ng2_fast_loading) && g_loading_screen.load()) {
      latched = true;
      icon_seen_ms = now;
    }
    // A 3D scene ends it too: a real-time cutscene right after a load has no HUD and no chapter
    // screen, and ran unthrottled for up to 15 s (GitHub #6: Genshin vs Joe at double speed,
    // odd audio). Loading screens draw 155-270 calls per frame, scenes 550+ (prof-route-4).
    const bool scene_3d = rex::graphics::LastFrameDrawCount() >= 400;
    if (latched && !g_loading_screen.load() &&
        (g_chapter_screen.load() || Ng2InGameplay() || scene_3d || now - icon_seen_ms > 15000 ||
         !REXCVAR_GET(ng2_fast_loading))) {
      if (scene_3d) REXLOG_INFO("NG2 fast loading: 3D scene ({} draws), off", rex::graphics::LastFrameDrawCount());
      latched = false;
    }
    const bool turbo = latched;
    static bool applied = false;
    if (turbo != applied) {
      applied = turbo;
      rex::cvar::SetFlagByName("vsync", turbo ? "false" : "true");
      REXLOG_INFO("NG2 fast loading: {}", turbo ? "on" : "off");
    }
  }
  if (rex::cvar::GetFlagByName("ng2_aspect_cycle") == "true") {
    static bool was_down = false;
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (down && !was_down) {
      const int mode = (g_aspect_scope_mode.load() + 1) % 4;
      g_aspect_scope_mode = mode;
      static const char* const kNames[] = {"A: main projection", "B: camera update",
                                           "C: everywhere", "D: 16:9 view"};
      REXLOG_WARN("NG2 aspect test mode {}", kNames[mode]);
    }
    was_down = down;
  }
  {
    // Mode C keeps the constant wide; the others keep it 16:9 outside their scope. Written
    // only on a change: the camera thread may be inside a scope right now.
    static int applied = -1;
    static float applied_wide = -1.0f;
    const int mode = g_aspect_scope_mode.load();
    const float wide = g_wide_aspect.load();
    if (mode != applied || (mode == 2 && wide != applied_wide)) {
      applied = mode;
      applied_wide = wide;
      WriteAspectConstant(mode == 2 && wide > 0.0f ? wide : 16.0f / 9.0f);
    }
  }
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
REXCVAR_DEFINE_BOOL(ng2_bar_diag, false, "NG2", "Diagnostic: log boss bar quads and rects");
namespace {
std::unordered_map<uint32_t, int> g_item_bucket;  // game thread only (render pass)
int g_draw_bucket = -1;                           // bucket of the item being drawn
uint32_t g_draw_item = 0;                         // the item being drawn
// Per-item x extent of everything an item drew (1280 space): this frame and the previous one.
// Shared buckets (39-41) are anchored per item from the previous frame's extent (issue #1).
struct ItemExtent {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
  bool rect = false;  // untextured rectangle (sub_8368E4F0)
};
std::unordered_map<uint32_t, ItemExtent> g_item_x_cur, g_item_x_prev;
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
  g_item_x_prev.swap(g_item_x_cur);
  g_item_x_cur.clear();
  g_hud_active = false;
  bool ingame_menu_drawn = false;
  bool menu_over_hud = false;  // pause / inventory (49-50) or in-game menu (70-72)
  bool loading_screen = false;
  bool chapter_screen = false;
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
      if (i == 86 && GuestLoadU32(GuestLoadU32(item) + 4) != 0x83705C98) {
        loading_screen = true;  // loading-screen petals / NOW LOADING (skulldiag-1)
      }
      if (i == 85 && GuestLoadU32(GuestLoadU32(item) + 4) != 0x83705C98) {
        chapter_screen = true;  // chapter title + [Proceed] (also save-slot text; see turbo)
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
  g_loading_screen.store(loading_screen && !g_hud_active, std::memory_order_relaxed);
  g_chapter_screen.store(chapter_screen, std::memory_order_relaxed);
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
// Buckets 39-41 are shared by many HUD widgets (karma / combo panel, hit counter and its
// shadow layer, boss bar, projectile stock, ...; diag-hud-bow-1: bucket 40 spans x -23..1347),
// so each item is anchored by where it draws: wholly in the left half = left edge, wholly in
// the right half = right edge, across the centre = centred. [x0, x1] is the quad being drawn.
Anchor AnchorOfExtent(float x0, float x1) {
  if (x1 <= 640.0f) return Anchor::kLeft;
  if (x0 >= 640.0f) return Anchor::kRight;
  // Across the centre: by where its middle is. The hit counter's blood splash (x -18..698,
  // middle 340) crosses the centre a little; centring it moved the counter back to its 16:9
  // place on the frames the splash existed (flicker, user 2026-11-02). Boss bar (64..1190,
  // middle 627) stays centred.
  const float mid = (x0 + x1) * 0.5f;
  if (mid < 1280.0f / 3.0f) return Anchor::kLeft;
  if (mid > 1280.0f * 2.0f / 3.0f) return Anchor::kRight;
  return Anchor::kCenter;
}
// Pieces of a widget (the boss bar's blue damage rectangle and purple lost-health sprite lie in
// the right half on their own) follow the widest textured item of the last frame that
// contains them, so the whole widget moves together.
Anchor BucketAnchor(int bucket, float x0 = 640.0f, float x1 = 640.0f, float y0 = 360.0f,
                    float y1 = 360.0f, bool rect = false) {
  if (!REXCVAR_GET(ng2_hud_anchor) || !g_hud_active) return Anchor::kCenter;
  if (bucket >= 57 && bucket <= 61) return Anchor::kLeft;
  if (bucket < 39 || bucket > 41) return Anchor::kCenter;
  if (g_draw_item) {
    auto& cur = g_item_x_cur[g_draw_item];
    cur.x0 = std::min(cur.x0, x0), cur.x1 = std::max(cur.x1, x1);
    cur.y0 = std::min(cur.y0, y0), cur.y1 = std::max(cur.y1, y1);
    cur.rect = rect;
    {
      // This piece's extent (last frame's when known, it is the whole item).
      float ex0 = x0, ex1 = x1, ey0 = y0, ey1 = y1;
      if (auto own = g_item_x_prev.find(g_draw_item); own != g_item_x_prev.end() && !rect) {
        ex0 = own->second.x0, ex1 = own->second.x1, ey0 = own->second.y0, ey1 = own->second.y1;
      }
      const float cx = (ex0 + ex1) * 0.5f, cy = (ey0 + ey1) * 0.5f;
      const ItemExtent* best = nullptr;
      for (const auto& [item, e] : g_item_x_prev) {
        if (item == g_draw_item || e.rect || e.x1 - e.x0 <= ex1 - ex0) continue;
        if (cx < e.x0 || cx > e.x1 || cy < e.y0 - 12.0f || cy > e.y1 + 12.0f) continue;
        if (e.x1 - e.x0 >= 1200.0f) continue;  // screen-wide overlays are no widget
        // Widest container = the whole widget (the boss bar frame), not one of its pieces
        // (its purple lost-health part alone lies in the right half).
        if (!best || e.x1 - e.x0 > best->x1 - best->x0) best = &e;
      }
      if (REXCVAR_GET(ng2_bucket_dump) && best) {
        static std::unordered_map<uint64_t, int> seen;
        const uint64_t key = (uint64_t(int(ex0) / 16) << 32) | uint32_t(int(ey0) / 16 * 1000 + bucket);
        if (seen[key]++ == 0) {
          REXLOG_INFO("NG2 HUD piece{}: bucket {} x {:.0f}-{:.0f} y {:.0f}-{:.0f} -> container {}",
                      rect ? " (rect)" : "", bucket, ex0, ex1, ey0, ey1,
                      best ? fmt::format("x {:.0f}-{:.0f} y {:.0f}-{:.0f}", best->x0, best->x1,
                                         best->y0, best->y1)
                           : std::string("none"));
        }
      }
      if (best) return AnchorOfExtent(best->x0, best->x1);
    }
    auto it = g_item_x_prev.find(g_draw_item);
    if (it != g_item_x_prev.end()) {
      x0 = std::min(x0, it->second.x0);
      x1 = std::max(x1, it->second.x1);
    }
  }
  return AnchorOfExtent(x0, x1);
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
  g_draw_item = item;
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
  const uint32_t vertices = ctx.r4.u32;
  auto* mem = REX_KERNEL_STATE()->memory();
  float rx0 = 1e9f, rx1 = -1e9f, ry0 = 1e9f, ry1 = -1e9f;
  for (uint32_t i = 0; i < 4; ++i) {
    const float x = rex::memory::load_and_swap<float>(mem->TranslateVirtual(vertices + i * 16));
    const float y = rex::memory::load_and_swap<float>(mem->TranslateVirtual(vertices + i * 16 + 4));
    rx0 = std::min(rx0, x), rx1 = std::max(rx1, x);
    ry0 = std::min(ry0, y), ry1 = std::max(ry1, y);
  }
  const Anchor anchor = BucketAnchor(g_draw_bucket, rx0, rx1, ry0, ry1, true);
  if (REXCVAR_GET(ng2_bar_diag) && g_hud_active && ry0 >= 600.0f && ry1 <= 700.0f) {
    static int64_t last = 0;
    const int64_t t = NowMs();
    if (t - last > 250) {
      last = t;
      REXLOG_INFO("NG2 bar rect: bucket {} item {:08X} x {:.1f}-{:.1f} y {:.0f}-{:.0f} anchor {}",
                  g_draw_bucket, g_draw_item, rx0, rx1, ry0, ry1, int(anchor));
    }
  }
  // Centred rects stay full-width (fades) except in the shared HUD buckets, where they are
  // pieces of a centred widget (the boss bar's damage strips) and get its 16:9 squeeze.
  const bool hud_piece = g_hud_active && REXCVAR_GET(ng2_hud_anchor) && g_draw_bucket >= 39 &&
                         g_draw_bucket <= 41 && rx1 - rx0 < 1200.0f;
  if (aspect <= 16.0 / 9.0 + 1e-4 || (anchor == Anchor::kCenter && !hud_piece)) {
    __imp__sub_8368E4F0(ctx, base);
    return;
  }
  const float inv_k = float((16.0 / 9.0) / aspect);
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
// Scissor items (GitHub issue #1, boss bar): sub_8369FBA8(r3 = item) enables the scissor test
// with the rectangle at item +24/+28 (left, top) and +16/+20 (width, height), 1280x720 space;
// sub_8369FC88 disables it. The boss bar's red fill is a full-length sprite clipped this way,
// so the rectangle gets the same squeeze as the sprites it clips (it ran too wide before).
extern "C" REX_FUNC(__imp__sub_8369FBA8);
extern "C" REX_FUNC(sub_8369FBA8) {
  const double aspect = REXCVAR_GET(ng2_aspect_ratio);
  const uint32_t item = ctx.r3.u32;
  if (aspect <= 16.0 / 9.0 + 1e-4 || !item) {
    __imp__sub_8369FBA8(ctx, base);
    return;
  }
  auto* mem = REX_KERNEL_STATE()->memory();
  uint8_t* px = mem->TranslateVirtual<uint8_t*>(item + 24);
  uint8_t* pw = mem->TranslateVirtual<uint8_t*>(item + 16);
  const float x0 = rex::memory::load_and_swap<float>(px), w = rex::memory::load_and_swap<float>(pw);
  const float y0 = rex::memory::load_and_swap<float>(mem->TranslateVirtual<uint8_t*>(item + 28));
  const float h = rex::memory::load_and_swap<float>(mem->TranslateVirtual<uint8_t*>(item + 20));
  SetDrawBucket(item);
  const Anchor anchor = BucketAnchor(g_draw_bucket, x0, x0 + w, y0, y0 + h);
  const float inv_k = float((16.0 / 9.0) / aspect);
  const float nx0 = MapX(x0, inv_k, anchor), nx1 = MapX(x0 + w, inv_k, anchor);
  if (REXCVAR_GET(ng2_bar_diag) && g_hud_active) {
    static int64_t last = 0;
    if (NowMs() - last > 500) {
      last = NowMs();
      REXLOG_INFO("NG2 scissor: bucket {} x {:.1f}+{:.1f} -> {:.1f}-{:.1f} y {:.0f}+{:.0f} anchor {}",
                  g_draw_bucket, x0, w, nx0, nx1, y0, h, int(anchor));
    }
  }
  rex::memory::store_and_swap<float>(px, nx0);
  rex::memory::store_and_swap<float>(pw, nx1 - nx0);
  __imp__sub_8369FBA8(ctx, base);
  rex::memory::store_and_swap<float>(px, x0);
  rex::memory::store_and_swap<float>(pw, w);
  g_draw_bucket = -1;
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
namespace ng2 {
bool PromptsUseKeyboard();
std::string PromptLabel(const std::string& tag, bool gameplay);
void PushPromptGlyph(float x0, float y0, float x1, float y1, const std::string& label);
}  // namespace ng2
REXCVAR_DEFINE_BOOL(ng2_prompt_diag, false, "NG2",
                    "Log distinct sprites drawn in menu buckets (button icon search)");
extern "C" REX_FUNC(__imp__sub_8368EFE8);
extern "C" REX_FUNC(sub_8368EFE8) {
  const uint32_t vertices = ctx.r4.u32;
  // Pause / inventory menu button icons (bucket 49): 59x59 quads on one texture strip,
  // v = 0 X, .125 Y, .25 B, .375 A (pausediag-1, findings #54). With keyboard prompts the
  // icon is not drawn; the settings overlay draws the key in its place.
  if (g_draw_bucket == 49 && ng2::PromptsUseKeyboard()) {
    auto* m = REX_KERNEL_STATE()->memory();
    auto f = [&](uint32_t i, uint32_t off) {
      return rex::memory::load_and_swap<float>(m->TranslateVirtual(vertices + i * 24 + off));
    };
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, v0 = 1e9f;
    for (uint32_t i = 0; i < 4; ++i) {
      x0 = std::min(x0, f(i, 0)), x1 = std::max(x1, f(i, 0));
      y0 = std::min(y0, f(i, 4)), y1 = std::max(y1, f(i, 4));
      v0 = std::min(v0, f(i, 16));
    }
    const float slot = v0 / 0.125f;
    const int idx = int(std::lround(slot));
    if (std::abs(x1 - x0 - 59.0f) < 3.0f && std::abs(y1 - y0 - 59.0f) < 3.0f && idx >= 0 &&
        idx <= 3 && std::abs(slot - float(idx)) < 0.02f) {
      static const char* const kTags[] = {"Rl", "Ru", "Rr", "Rd"};
      const std::string label = ng2::PromptLabel(kTags[idx], false);
      if (!label.empty()) {
        ng2::PushPromptGlyph(x0, y0, x1, y1, label);
        return;
      }
    }
  }
  if (REXCVAR_GET(ng2_prompt_diag) && g_draw_bucket >= 0) {
    auto* m = REX_KERNEL_STATE()->memory();
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f, u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
    for (uint32_t i = 0; i < 4; ++i) {
      auto f = [&](uint32_t off) {
        return rex::memory::load_and_swap<float>(m->TranslateVirtual(vertices + i * 24 + off));
      };
      x0 = std::min(x0, f(0)), x1 = std::max(x1, f(0)), y0 = std::min(y0, f(4)), y1 = std::max(y1, f(4));
      u0 = std::min(u0, f(12)), u1 = std::max(u1, f(12)), v0 = std::min(v0, f(16)), v1 = std::max(v1, f(16));
    }
    static std::unordered_map<std::string, int> seen;
    char key[160];
    std::snprintf(key, sizeof(key), "%d %.0f %.0f %.0f %.0f v %.4f %.4f", g_draw_bucket, x0, y0,
                  x1 - x0, y1 - y0, v0, v1);
    if (seen[key]++ == 0) {
      REXLOG_INFO("NG2 prompt diag: bucket {} sprite {:08X} tex {:08X} rect ({:.0f},{:.0f})-({:.0f},{:.0f}) "
                  "uv ({:.4f},{:.4f})-({:.4f},{:.4f}) lr {:08X}",
                  g_draw_bucket, ctx.r3.u32, GuestLoadU32(ctx.r3.u32 + 8), x0, y0, x1, y1, u0,
                  v0, u1, v1, uint32_t(ctx.lr));
    }
  }
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
  float qx0 = 1e9f, qx1 = -1e9f, qy0 = 1e9f, qy1 = -1e9f;
  for (uint32_t i = 0; i < 4; ++i) {
    const float x = rex::memory::load_and_swap<float>(mem->TranslateVirtual(vertices + i * 24));
    const float y = rex::memory::load_and_swap<float>(mem->TranslateVirtual(vertices + i * 24 + 4));
    qx0 = std::min(qx0, x), qx1 = std::max(qx1, x);
    qy0 = std::min(qy0, y), qy1 = std::max(qy1, y);
  }
  const Anchor anchor = BucketAnchor(g_draw_bucket, qx0, qx1, qy0, qy1);
  if (REXCVAR_GET(ng2_bar_diag) && g_hud_active && qy0 >= 600.0f && qy1 <= 700.0f) {
    // NG2 DIAG (boss bar): every bottom-bar quad of one whole frame per second, with the
    // squeezed result, so the piece that maps differently shows.
    static uint32_t log_frame = ~0u;
    static int64_t last = 0;
    const uint32_t frame = GuestLoadU32(kBuckets + 1456);
    const int64_t t = NowMs();
    if (frame != log_frame && t - last > 1000) { last = t; log_frame = frame; }
    if (frame == log_frame) {
      const double asp = REXCVAR_GET(ng2_aspect_ratio);
      const float ik = asp > 16.0 / 9.0 ? float((16.0 / 9.0) / asp) : 1.0f;
      REXLOG_INFO("NG2 bar quad: bucket {} item {:08X} method {:08X} x {:.1f}-{:.1f} -> {:.1f}-{:.1f} y {:.0f}-{:.0f} anchor {} lr {:08X}",
                  g_draw_bucket, g_draw_item, GuestLoadU32(GuestLoadU32(g_draw_item) + 4), qx0, qx1,
                  MapX(qx0, ik, anchor), MapX(qx1, ik, anchor), qy0, qy1, int(anchor), uint32_t(ctx.lr));
    }
  }
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
    float tx;
    {
      const uint32_t bits = uint32_t(ctx.r10.u64 >> 32);
      std::memcpy(&tx, &bits, 4);  // m30: the string's x position
    }
    float ty;
    {
      const uint32_t bits = uint32_t(ctx.r10.u64 & 0xFFFFFFFFull);
      std::memcpy(&ty, &bits, 4);  // m31: the string's y position
    }
    const Anchor anchor = BucketAnchor(g_draw_bucket, tx, tx, ty, ty);
    scale_hi(ctx.r4, 0.0f);
    scale_hi(ctx.r6, 0.0f);
    scale_hi(ctx.r8, 0.0f);
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


// Mission Mode (findings #66): when a mission is selected, sub_823E4720 sets the number of
// selectable difficulties at [r31+48] from the mission's clear flags (high nibble of byte 7 of
// its 8-byte record at [0x84C559E0] + 1016 + 8 * i: 0x10 -> 2, 0x20 -> 3, 0x40 / 0x80 -> 4,
// none -> 1 = straight to costume selection); missions of table type 3 ([0x83B2E334 + 12 * i])
// keep their fixed count. The hook runs right after the count is loaded into r11.
void Ng2MissionDifficulties(PPCRegister& r11, PPCRegister& r31) {
  if (!REXCVAR_GET(ng2_cheat_difficulties) || r11.u32 >= 4) return;
  const uint32_t mission = GuestLoadU32(r31.u32 + 4);
  if (mission >= 100 || GuestLoadU32(0x83B2E334 + mission * 12) == 3) return;
  r11.u64 = 4;
  auto* mem = REX_KERNEL_STATE()->memory();
  rex::memory::store_and_swap<uint32_t>(mem->TranslateVirtual(r31.u32 + 48), 4u);
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

// Milliseconds since the in-game (pause) menu was last drawn.
int64_t Ng2IngameMenuHiddenMs() { return NowMs() - g_ingame_visible_ms.load(); }

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
bool g_mouse_has_camera = false;
float g_zoom = 1.0f;         // camera distance factor in use (applied in sub_822FADB8)
float g_zoom_target = 1.0f;  // set by the mouse wheel; g_zoom eases toward it  // set by the camera update below (game thread)
}  // namespace

// Off by default: boss grabs use the same mode 8 / action 114 -> 111 as executions, and taking
// that camera made the game swing it 60-180 deg every frame (cam-follow-1, user 2026-11-02).
REXCVAR_DEFINE_BOOL(ng2_mouse_cam_take_scripted, false, "NG2",
                    "Mouse look takes over scripted cameras (executions, grabs: mode 8)");
int Ng2F9Presses();  // crash_handler.cpp
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
    if (LoadF(rec + 28) != 0.0f || LoadF(rec + 32) != 0.0f) {
      g_mouse_used_ms = 0;
      g_zoom_target = 1.0f;  // the wheel zoom eases back out (no snap) when the pad takes over
    }
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
  // First-person view (camera mode 4) is updated elsewhere; the mouse goes to the stick there
  // (sub_822F2E38 below) and the third-person orbit is skipped.
  const bool first_person = cam && GuestLoadU32(cam + 4716) == 4;
  {
    const int steps = rex::input::mnk::ConsumeMouseWheel();  // + = wheel up = zoom in
    // One wheel scale: below 1.0 the eye moves toward the target (stays on the path the game
    // already checked against walls), above 1.0 the field of view widens instead (moving the
    // eye back clipped through walls).
    if (steps) g_zoom_target = std::clamp(g_zoom_target * std::pow(0.9f, float(steps)), 0.4f, 1.5f);
  }
  const GuestVec pre_eye = cam ? LoadVec(mem->TranslateVirtual<uint8_t*>(cam + 64)) : GuestVec{0, 0, 0};
  const GuestVec pre_tgt = cam ? LoadVec(mem->TranslateVirtual<uint8_t*>(cam + 80)) : GuestVec{0, 0, 0};
  __imp__sub_823053F0(ctx, base);
  if (REXCVAR_GET(ng2_mouse_cam_log) && cam) {
    // NG2 DIAG: per-frame camera history; F9 dumps the last 4 s (boss camera vs mouse look).
    struct CamFrame {
      int64_t ms;
      uint32_t mode, action, flag4640;
      bool mouse;
      float dx, dy;
      GuestVec pre_eye, pre_tgt, game_eye, game_tgt, player;
    };
    static CamFrame ring[240];
    static size_t head = 0;
    static int seen_f9 = 0;
    const uint32_t pl = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
    CamFrame& f = ring[head++ % 240];
    f = {now, GuestLoadU32(cam + 4716), pl ? GuestLoadU32(pl + 336) : 0, GuestLoadU32(cam + 4640),
         g_mouse_has_camera, dx, dy, pre_eye, pre_tgt,
         LoadVec(mem->TranslateVirtual<uint8_t*>(cam + 64)), LoadVec(mem->TranslateVirtual<uint8_t*>(cam + 80)),
         pl ? LoadVec(mem->TranslateVirtual<uint8_t*>(pl + 48)) : GuestVec{0, 0, 0}};
    const int f9 = Ng2F9Presses();
    if (f9 != seen_f9) {
      seen_f9 = f9;
      REXLOG_INFO("NG2 cam history (F9 #{}): last {} frames", f9, std::min<size_t>(head, 240));
      for (size_t i = head > 240 ? head - 240 : 0; i < head; ++i) {
        const CamFrame& c = ring[i % 240];
        REXLOG_INFO("NG2 cam h {} mode {} act {} f {} mouse {} d {:.0f} {:.0f} pre e({:.2f} {:.2f} {:.2f}) "
                    "t({:.2f} {:.2f} {:.2f}) game e({:.2f} {:.2f} {:.2f}) t({:.2f} {:.2f} {:.2f}) pl({:.2f} {:.2f} {:.2f})",
                    c.ms, c.mode, c.action, c.flag4640, c.mouse, c.dx, c.dy, c.pre_eye.x, c.pre_eye.y,
                    c.pre_eye.z, c.pre_tgt.x, c.pre_tgt.y, c.pre_tgt.z, c.game_eye.x, c.game_eye.y,
                    c.game_eye.z, c.game_tgt.x, c.game_tgt.y, c.game_tgt.z, c.player.x, c.player.y,
                    c.player.z);
      }
    }
  }
  if (first_person) return;  // the game's first-person look (mouse fed as the stick above)
  // Scripted cameras (grabs, executions: any mode but 1) play as on a console; mouse look
  // resumes from wherever they leave the camera.
  const bool scripted = cam && GuestLoadU32(cam + 4716) != 1 && !REXCVAR_GET(ng2_mouse_cam_take_scripted);
  if (!active || scripted) {
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
  {
    // NG2 DIAG (boss / grab camera fights the mouse): frames where the camera target jumps.
    static GuestVec last_tgt{0, 0, 0};
    static float last_r = 0.0f;
    const float jx = tgt.x - last_tgt.x, jy = tgt.y - last_tgt.y, jz = tgt.z - last_tgt.z;
    const float jump = std::sqrt(jx * jx + jy * jy + jz * jz);
    if (REXCVAR_GET(ng2_mouse_cam_log) && last_r > 0.0f && (jump > 0.15f * last_r || std::abs(r - last_r) > 0.3f * last_r)) {
      static int n = 0;
      if (n++ < 400) {
        const uint32_t pla = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
        uint8_t* pl = mem->TranslateVirtual<uint8_t*>((pla ? pla : 0x84C56000) + 48);
        REXLOG_INFO("NG2 mouse cam jump: target ({:.2f} {:.2f} {:.2f}) moved {:.2f}, dist {:.2f} -> {:.2f}, "
                    "player ({:.2f} {:.2f} {:.2f}) mode {} action {} mouse {}",
                    tgt.x, tgt.y, tgt.z, jump, last_r, r, LoadF(pl), LoadF(pl + 4), LoadF(pl + 8),
                    GuestLoadU32(cam + 4716),
                    GuestLoadU32(GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) + 336), recent);
      }
    }
    last_tgt = tgt;
    last_r = r;
  }
  if (REXCVAR_GET(ng2_mouse_cam_log)) {
    // NG2 DIAG: where is Ryu's position? Vec3s in the player object region (below the camera
    // block) near the camera target, once every 2 s.
    static int64_t last_scan = 0;
    if (now - last_scan > 2000) {
      last_scan = now;
      std::string hits;
      for (uint32_t off = 0; off + 12 <= 0x9F0; off += 4) {
        uint8_t* v = mem->TranslateVirtual<uint8_t*>(0x84C56000 + off);
        const float x = LoadF(v), y = LoadF(v + 4), z = LoadF(v + 8);
        if (!std::isfinite(x) || std::abs(x - tgt.x) > 2.5f || std::abs(z - tgt.z) > 2.5f ||
            std::abs(y - tgt.y) > 3.0f) continue;
        char b[64];
        std::snprintf(b, sizeof(b), " +%X(%.2f %.2f %.2f)", off, x, y, z);
        hits += b;
      }
      REXLOG_INFO("NG2 pos scan: target ({:.2f} {:.2f} {:.2f}) mode {}:{}", tgt.x, tgt.y, tgt.z,
                  GuestLoadU32(cam + 4716), hits);
    }
  }
  // While the mouse has the camera, its direction is ours: whatever the game's follow logic
  // turned this frame is undone (the game's distance is kept: it pulls in for walls).
  static bool have_dir = false;
  static float yaw_state = 0.0f, elev_state = 0.0f;
  static GuestVec last_out{0, 0, 0};
  if (!recent || !(r > 1e-3f) || !std::isfinite(r)) {
    have_dir = false;
    return;
  }
  static int64_t game_owns_until = 0;
  // Forced lock-on (boss fights): the game aims the camera at a point away from Ryu and turns
  // it between frames; fighting that made the camera flicker. Lock-on = the camera target more
  // than 3 units (horizontally) from Ryu (player object +0x130, feet; normally within ~2 units,
  // pos scan in cam-scripted-2). The game then keeps the camera, until 0.4 s after the target
  // is back on Ryu. A 3-degree "the game turned it" rule caught ordinary nudges and left the
  // mouse dead for 0.4 s at a time (user, 2026-11-02).
  {
    const uint32_t pla = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
    if (pla) {
      const GuestVec ryu = LoadVec(mem->TranslateVirtual<uint8_t*>(pla + 0x130));
      const float dxz = std::hypot(pre_tgt.x - ryu.x, pre_tgt.z - ryu.z);
      if (std::isfinite(dxz) && dxz > 3.0f) {
        if (REXCVAR_GET(ng2_mouse_cam_log) && now >= game_owns_until) {
          REXLOG_INFO("NG2 mouse cam: lock-on (target {:.1f} from Ryu), game has the camera", dxz);
        }
        game_owns_until = now + 400;
      }
    }
  }
  if (have_dir && now >= game_owns_until) {
    // A big turn by the game since our last write (a lock-on just ended, a respawn, ...):
    // continue from the game's direction instead of snapping back.
    const float ax = last_out.x - pre_tgt.x, ay = last_out.y - pre_tgt.y, az = last_out.z - pre_tgt.z;
    const float bx = pre_eye.x - pre_tgt.x, by = pre_eye.y - pre_tgt.y, bz = pre_eye.z - pre_tgt.z;
    const float la = std::sqrt(ax * ax + ay * ay + az * az), lb = std::sqrt(bx * bx + by * by + bz * bz);
    if (la > 1e-3f && lb > 1e-3f && (ax * bx + ay * by + az * bz) / (la * lb) < 0.9063f) {  // > 25 deg
      have_dir = false;
    }
  }
  if (now < game_owns_until) {
    // Lock-on: the game's camera as is; mouse look resumes from its direction afterwards.
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
  last_out = out;
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
void Ng2CaptureBurst(const char* tag);  // crash_handler.cpp (NG2 DIAG)
extern "C" REX_FUNC(sub_822F7318) {
  if (rex::cvar::GetFlagByName("ng2_effect_capture") == "true" &&
      GuestLoadU32(ctx.r3.u32 + 1644) == 1) {  // camera shake pulse (camdiag-2: hits)
    Ng2CaptureBurst("shake");
  }
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
  WideAspectScope wide_update(1);  // NG2 DIAG mode B
  const uint32_t c = ctx.r3.u32;
  const uint32_t mode = c == 0x84C569F0 ? GuestLoadU32(c + 4716) : 1;
  // Mode 4 = first-person view (held 1st-Person View): left alone, or moving the mouse would
  // pull the player out of it.
  // Only executions (mode 8) are taken over: grabs use mode 7 (diag-hud-bow-3), and forcing it
  // back to 1 left the camera flickering wildly after the grab (user report 2026-11-02).
  if (REXCVAR_GET(ng2_mouse_cam_take_scripted) && g_mouse_has_camera && Ng2InGameplay() && mode == 8) {
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
void Ng2CaptureBurst(const char* tag);  // crash_handler.cpp (NG2 DIAG)
REXCVAR_DEFINE_BOOL(ng2_effect_capture, false, "NG2",
                    "Capture frame bursts when effect projection sub_82845BD8 runs");
extern "C" REX_FUNC(sub_82845BD8) {
  NativeAspectScope native;
  if (REXCVAR_GET(ng2_effect_capture)) Ng2CaptureBurst("82845BD8");
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
// sub_822FB368(r3 = camera) builds the camera's main projection (camera+512) from the 16:9
// constant (or 4:3 0x8228716C under some flag). It is the only code that gets the wide value:
// the camera update sub_822FA8B8 also builds +688 (4:3) and +752 (16:9) projections, which
// effects use; widening those gave explosion effects black bands (findings #53).
REXCVAR_DEFINE_INT32(ng2_fov_percent, 100, "NG2", "Gameplay camera field of view, percent of the game's")
    .range(50, 200);
extern "C" REX_FUNC(sub_822FB368) {
  WideAspectScope wide_projection(0);
  // Field of View setting: the gameplay camera's fov (camera+176, read here for the main
  // projection) is scaled while the projection is built, then restored, so the game's own fov
  // changes keep working. Cutscenes (HUD hidden) keep their framing.
  const uint32_t fov_cam = ctx.r3.u32;
  const int fov_pct = REXCVAR_GET(ng2_fov_percent);
  // Mouse wheel zoom past 1.0 widens the view (see sub_822FADB8).
  const float wheel_fov = GuestLoadU32(0x84C569F0 + 4716) == 4 ? 1.0f : std::max(1.0f, g_zoom);
  const float fov_scale = float(fov_pct) / 100.0f * wheel_fov;
  uint8_t* fov_p = nullptr;
  float fov_saved = 0.0f;
  if (fov_scale != 1.0f && fov_cam == 0x84C569F0 && Ng2InGameplay()) {
    fov_p = REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(fov_cam + 176);
    fov_saved = LoadF(fov_p);
    StoreF(fov_p, fov_saved * fov_scale);
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("NG2 fov: gameplay camera fov {:.4f} scaled to {}%", fov_saved, fov_pct);
    }
  }
  if (REXCVAR_GET(ng2_aspect_diag)) {
    g_aspect_calls[6].fetch_add(1);
    AspectDiagTick();
  }
  __imp__sub_822FB368(ctx, base);
  if (fov_p) StoreF(fov_p, fov_saved);
}

// ---- Keyboard & mouse prompts ------------------------------------------------------------
// Text layout sub_83706C20(r3 = text renderer state): UTF-16BE string at +52, position +96;
// "@[" tags go to the parser sub_837073A0, where "pad:NAME" becomes one button-icon glyph
// (name -> glyph map via sub_837082D0, glyph at +168). The renderer state is per draw and
// does not own the string, so with keyboard prompts on, a string holding pad tags is swapped
// for a cached copy (guest heap) where each tag reads "[Key]" (findings #54).
namespace ng2 {
bool PromptsUseKeyboard();
uint32_t PromptLabelsVersion();
std::string PromptLabel(const std::string& tag, bool gameplay);
}  // namespace ng2
namespace {
// renderer state -> {last +52, prompt mode it was checked under}
std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> g_text_last_string;
std::unordered_map<uint32_t, bool> g_prompt_buffers;        // our rewritten strings
std::unordered_map<uint64_t, uint32_t> g_prompt_cache;      // source key -> rewritten

uint32_t RewritePromptString(uint32_t src) {
  auto* mem = REX_KERNEL_STATE()->memory();
  const uint8_t* p = mem->TranslateVirtual<uint8_t*>(src);
  std::vector<uint16_t> in;
  for (int i = 0; i < 4096; ++i) {
    const uint16_t c = rex::memory::load_and_swap<uint16_t>(p + 2 * i);
    if (!c) break;
    in.push_back(c);
  }
  static const char kTag[] = "@[pad:";
  auto tag_at = [&](size_t i) {
    for (size_t k = 0; k < 6; ++k) {
      if (i + k >= in.size() || in[i + k] != uint16_t(kTag[k])) return false;
    }
    return true;
  };
  bool any = false;
  for (size_t i = 0; i < in.size() && !any; ++i) any = tag_at(i);
  if (!any) return 0;
  const bool gameplay = Ng2InGameplay();
  uint64_t h = 1469598103934665603ull;
  for (uint16_t c : in) h = (h ^ c) * 1099511628211ull;
  const uint64_t key = h ^ (uint64_t(src) << 1) ^ (uint64_t(ng2::PromptLabelsVersion()) << 40) ^
                       (gameplay ? 0x8000000000000000ull : 0);
  if (auto it = g_prompt_cache.find(key); it != g_prompt_cache.end()) return it->second;
  std::vector<uint16_t> out;
  for (size_t i = 0; i < in.size();) {
    if (tag_at(i)) {
      size_t end = i + 6;
      while (end < in.size() && in[end] != uint16_t(']')) ++end;
      std::string name;
      for (size_t k = i + 6; k < end; ++k) name.push_back(char(in[k] & 0x7F));
      const std::string label = ng2::PromptLabel(name, gameplay);
      if (!label.empty() && end < in.size()) {
        out.push_back(uint16_t('['));
        for (char ch : label) out.push_back(uint16_t(uint8_t(ch)));
        out.push_back(uint16_t(']'));
        i = end + 1;
        continue;
      }
    }
    out.push_back(in[i]);
    ++i;
  }
  out.push_back(0);
  const uint32_t buf = mem->SystemHeapAlloc(uint32_t(out.size() * 2));
  if (!buf) return 0;
  uint8_t* d = mem->TranslateVirtual<uint8_t*>(buf);
  for (size_t i = 0; i < out.size(); ++i) rex::memory::store_and_swap<uint16_t>(d + 2 * i, out[i]);
  g_prompt_cache[key] = buf;
  g_prompt_buffers[buf] = true;
  return buf;
}
}  // namespace

extern "C" REX_FUNC(__imp__sub_83706C20);
extern "C" REX_FUNC(sub_83706C20) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t str = GuestLoadU32(obj + 52);
  if (str) {
    const bool keyboard = ng2::PromptsUseKeyboard();
    const uint32_t mode = keyboard ? (ng2::PromptLabelsVersion() << 2) | (Ng2InGameplay() ? 2u : 1u)
                                   : 0u;
    auto& last = g_text_last_string[obj];
    if (last.first != str || last.second != mode) {
      last = {str, mode};
      if (!g_prompt_buffers.count(str) && keyboard) {
        if (const uint32_t buf = RewritePromptString(str)) {
          rex::memory::store_and_swap<uint32_t>(
              REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(obj + 52), buf);
          last.first = buf;
        }
      }
    }
  }
  __imp__sub_83706C20(ctx, base);
}

// Mouse wheel zoom (findings #56). The render step of the camera update takes the gameplay
// camera's direction but computes its own distance (smoothing, wall collision), then copies eye
// and target to R+128 / R+144, runs sub_822FDDF8 and sub_822FADB8, and builds the view from them.
// Zooming the gameplay camera (+64) therefore did nothing; here the final eye's distance from the
// target is scaled after the game's own collision, right before the view is built.
extern "C" REX_FUNC(__imp__sub_822FADB8);
extern "C" REX_FUNC(sub_822FADB8) {
  const uint32_t render_cam = ctx.r3.u32;
  __imp__sub_822FADB8(ctx, base);
  if (render_cam != 0x84C569F0) return;
  {
    // Ease toward the wheel's target (time constant ~70 ms) instead of snapping per notch.
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(0.1f, std::chrono::duration<float>(now - last).count());
    last = now;
    g_zoom += (g_zoom_target - g_zoom) * (1.0f - std::exp(-dt / 0.07f));
    if (std::abs(g_zoom_target - g_zoom) < 1e-4f) g_zoom = g_zoom_target;
  }
  if (g_zoom >= 1.0f || !Ng2InGameplay() || GuestLoadU32(render_cam + 4716) == 4) return;
  auto* mem = REX_KERNEL_STATE()->memory();
  uint8_t* eye_p = mem->TranslateVirtual<uint8_t*>(render_cam + 128);
  const GuestVec eye = LoadVec(eye_p);
  const GuestVec tgt = LoadVec(mem->TranslateVirtual<uint8_t*>(render_cam + 144));
  StoreVec(eye_p, {tgt.x + (eye.x - tgt.x) * g_zoom, tgt.y + (eye.y - tgt.y) * g_zoom,
                   tgt.z + (eye.z - tgt.z) * g_zoom});
}

// First-person view (1st-Person View held; gameplay camera mode 4, findings #56) runs its own
// look code instead of sub_823053F0 and reads the right stick. The input update sub_822F2E38
// writes the stick floats every frame (record 0x83C07FF0 + pad*68, +28/+32); in first person
// the mouse motion is added there, clamped to the stick range.
REXCVAR_DEFINE_BOOL(ng2_bow_aim_direct, true, "NG2",
                    "Mouse turns the bow aim directly (off = mouse acts as the left stick)");
REXCVAR_DEFINE_DOUBLE(ng2_bow_aim_gain, 1.0, "NG2", "Bow aim: mouse speed factor (Aim Sensitivity)");
REXCVAR_DEFINE_BOOL(ng2_bow_manual_aim, true, "NG2",
                    "Keyboard & mouse: aiming a projectile starts in manual aim (no stick tap needed)");
namespace {
int g_bow_mouse_frames = 0;  // frames the mouse still owns the aim (smoothed value reset after)
float g_bow_pending_x = 0.0f, g_bow_pending_y = 0.0f;  // turn above the peak rate, for later frames
}
REXCVAR_DEFINE_BOOL(ng2_bow_aim_invert_curve, true, "NG2",
                    "Bow aim: undo the stick curve so aim speed follows mouse speed");
REXCVAR_DEFINE_BOOL(ng2_aim_diag, false, "NG2", "Diagnostic: search the bow aim angles while aiming");
REXCVAR_DEFINE_BOOL(ng2_action_log, false, "NG2",
                     "Diagnostic: log player action changes with camera mode and left stick");
extern "C" REX_FUNC(__imp__sub_822F2E38);
extern "C" REX_FUNC(sub_822F2E38) {
  __imp__sub_822F2E38(ctx, base);
  if (REXCVAR_GET(ng2_aim_diag)) {
    // NG2 DIAG (issue #3): find the bow aim angles. While aiming (action 115 / 184), compare
    // the player + gameplay camera block (0x84C56000..+0x3000) frame to frame: values that
    // change while the left stick is held and stay put while it is released.
    constexpr uint32_t kBase = 0x84C56000, kWords = 0x3000 / 4;
    static std::vector<uint32_t> prev(kWords);
    static std::vector<int> moved(kWords), still(kWords);
    static std::vector<double> corr_x(kWords), corr_y(kWords);
    static bool was_aiming = false, have_prev = false;
    static int frames_moving = 0, frames_still = 0;
    const uint32_t pl = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
    const uint32_t act = pl ? GuestLoadU32(pl + 336) : 0;
    const bool aiming = act == 115 || act == 184;
    auto* mem = REX_KERNEL_STATE()->memory();
    if (aiming) {
      uint32_t pad = *mem->TranslateVirtual<uint8_t*>(0x84C3DA3E);
      if (pad > 3) pad = 0;
      uint8_t* rec = mem->TranslateVirtual<uint8_t*>(0x83C07FF0 + pad * 68);
      const float lx = LoadF(rec + 20), ly = LoadF(rec + 24);
      const bool moving = std::abs(lx) > 0.1f || std::abs(ly) > 0.1f;
      (moving ? frames_moving : frames_still)++;
      for (uint32_t i = 0; i < kWords; ++i) {
        const uint32_t v = GuestLoadU32(kBase + i * 4);
        if (have_prev && v != prev[i]) {
          (moving ? moved : still)[i]++;
          float a, b;
          std::memcpy(&a, &v, 4);
          std::memcpy(&b, &prev[i], 4);
          const double d = std::isfinite(a) && std::isfinite(b) ? double(a) - double(b) : 0.0;
          corr_x[i] += d * lx;
          corr_y[i] += d * ly;
        }
        prev[i] = v;
      }
      have_prev = true;
    } else if (was_aiming) {
      std::vector<uint32_t> idx;
      for (uint32_t i = 0; i < kWords; ++i) {
        if (moved[i] >= std::max(5, frames_moving / 3) && still[i] * 4 <= frames_still) idx.push_back(i);
      }
      std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return moved[a] > moved[b]; });
      REXLOG_INFO("NG2 aim diag: aim ended, {} frames moving / {} still, {} candidates", frames_moving,
                  frames_still, idx.size());
      for (size_t k = 0; k < idx.size() && k < 40; ++k) {
        const uint32_t i = idx[k];
        float v;
        std::memcpy(&v, &prev[i], 4);
        REXLOG_INFO("NG2 aim diag:   {:08X} (+{:X}) moved {} still {} corr x {:.3f} y {:.3f} value {}",
                    kBase + i * 4, i * 4, moved[i], still[i], corr_x[i], corr_y[i], v);
      }
      std::fill(moved.begin(), moved.end(), 0);
      std::fill(still.begin(), still.end(), 0);
      std::fill(corr_x.begin(), corr_x.end(), 0.0);
      std::fill(corr_y.begin(), corr_y.end(), 0.0);
      frames_moving = frames_still = 0;
      have_prev = false;
    }
    was_aiming = aiming;
  }
  if (REXCVAR_GET(ng2_action_log)) {
    static uint32_t last_action = ~0u, last_mode = ~0u;
    const uint32_t player = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
    const uint32_t action = player ? GuestLoadU32(player + 336) : ~1u;
    const uint32_t mode = GuestLoadU32(0x84C569F0 + 4716);
    if (action != last_action || mode != last_mode) {
      auto* mem = REX_KERNEL_STATE()->memory();
      uint32_t pad = *mem->TranslateVirtual<uint8_t*>(0x84C3DA3E);
      if (pad > 3) pad = 0;
      uint8_t* rec = mem->TranslateVirtual<uint8_t*>(0x83C07FF0 + pad * 68);
      REXLOG_INFO("NG2 action: {} -> {} cam mode {} gameplay {} lstick {:.2f} {:.2f} player {:08X}",
                  int32_t(last_action), int32_t(action), mode, Ng2InGameplay(), LoadF(rec + 20),
                  LoadF(rec + 24), player);
      last_action = action;
      last_mode = mode;
    }
  }
  const ng2::MouseCameraParams p = ng2::GetMouseCameraParams();
  {
    // Holding the projectile button auto-aims; only tilting the left stick switches to manual
    // aim, so keyboard players had to tap W first (user, 2026-11-02). On keyboard & mouse the
    // aim starts manual: one frame of stick tilt when aiming begins, then the game's smoothed
    // aim (+0x420/+0x424) is cleared so the tilt does not turn the aim.
    // Holding the button: action 195 (bow drawn) -> 255 (auto-aim); a stick tilt then gives
    // 115 (manual aim) (bow-manual-1).
    static uint32_t prev_action = 0;
    static int kick = 0;
    const uint32_t pl = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
    const uint32_t act = pl ? GuestLoadU32(pl + 336) : 0;
    // Auto-aim (255) is entered from many moves: drawing (195), landing (48, the jump-draw that
    // absorbs essence), in the air (11), running (34), ... (bow-manual-2). A stick already held
    // at entry does not switch to manual aim, so: one frame released, then tilted.
    const bool auto_aim_start = act == 255 && prev_action != 255 && Ng2InGameplay();
    prev_action = act;
    auto* m = REX_KERNEL_STATE()->memory();
    uint32_t pd = *m->TranslateVirtual<uint8_t*>(0x84C3DA3E);
    if (pd > 3) pd = 0;
    uint8_t* r = m->TranslateVirtual<uint8_t*>(0x83C07FF0 + pd * 68);
    if (auto_aim_start && p.enabled && ng2::PromptsUseKeyboard() && REXCVAR_GET(ng2_bow_manual_aim)) {
      kick = 5;
    }
    if (kick > 0) {
      --kick;
      if (act == 115 || act == 184) {
        // Manual aim reached: drop what the tilt put into the smoothed aim.
        uint8_t* smooth = m->TranslateVirtual<uint8_t*>(0x84C569F0 + 0x420);
        StoreF(smooth, LoadF(r + 20));
        StoreF(smooth + 4, LoadF(r + 24));
        kick = 0;
      } else if (act != 255) {
        kick = 0;  // left auto-aim (a throw, not a held draw)
      } else if (kick == 4) {
        StoreF(r + 20, 0.0f);  // released
        StoreF(r + 24, 0.0f);
      } else {
        StoreF(r + 20, 0.0f);  // tilted
        StoreF(r + 24, 0.5f);
      }
    }
  }
  if (!p.enabled || !Ng2InGameplay() || GuestLoadU32(0x84C569F0 + 4716) != 4) {
    g_bow_pending_x = g_bow_pending_y = 0.0f;
    return;
  }
  float dx = 0.0f, dy = 0.0f;
  rex::input::mnk::ConsumeMouseDelta(dx, dy);
  {
    const uint32_t pl = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
    const uint32_t act = pl ? GuestLoadU32(pl + 336) : 0;
    if (act != 115 && act != 184) g_bow_pending_x = g_bow_pending_y = 0.0f;  // not aiming
  }
  const bool carry = g_bow_pending_x != 0.0f || g_bow_pending_y != 0.0f;
  if (dx == 0.0f && dy == 0.0f && !carry) {
    if (g_bow_mouse_frames > 0 && --g_bow_mouse_frames == 0) {
      // The mouse stopped: no easing back from the mouse rate (the aim would drift on);
      // the smoothed aim takes the stick's (WASD's) value at once.
      auto* m = REX_KERNEL_STATE()->memory();
      uint32_t pd = *m->TranslateVirtual<uint8_t*>(0x84C3DA3E);
      if (pd > 3) pd = 0;
      uint8_t* r = m->TranslateVirtual<uint8_t*>(0x83C07FF0 + pd * 68);
      uint8_t* smooth = m->TranslateVirtual<uint8_t*>(0x84C569F0 + 0x420);
      StoreF(smooth, LoadF(r + 20));
      StoreF(smooth + 4, LoadF(r + 24));
    }
    return;
  }
  auto* mem = REX_KERNEL_STATE()->memory();
  uint32_t pad = *mem->TranslateVirtual<uint8_t*>(0x84C3DA3E);
  if (pad > 3) pad = 0;
  uint8_t* rec = mem->TranslateVirtual<uint8_t*>(0x83C07FF0 + pad * 68);
  constexpr float kStickPerPixel = 0.12f;  // at sensitivity 1.0 (user: 1.5x of 0.08)
  const float sx = dx * p.sens_x * kStickPerPixel * -float(REXCVAR_GET(ng2_mouse_yaw_sign));
  const float sy = -dy * p.sens_y * kStickPerPixel * (p.invert_y ? -1.0f : 1.0f);
  // Bow aiming also uses camera mode 4, but aims with the LEFT stick (WASD): player action
  // 115 (aiming on the ground) / 184 (in the air) (diag-hud-bow-1, GitHub issue #3).
  const uint32_t player = GuestLoadU32(0x83B28400) ? GuestLoadU32(GuestLoadU32(0x83B28400) + 1632) : 0;
  const uint32_t action = player ? GuestLoadU32(player + 336) : 0;
  const bool bow = action == 115 || action == 184;
  if (bow && REXCVAR_GET(ng2_bow_aim_direct)) {
    // Gameplay camera +0x420 / +0x424 are the game's smoothed aim stick (they ease toward the
    // stick and fall back to 0 on release: aim-diag-1, aim-direct-1), which is the ramp-up the
    // mouse felt through the stick. Setting the stick and the smoothed value to the same
    // mouse rate leaves nothing to ease, and the rate is not capped at full stick.
    // The aim goes through the stick curve y = 2x^3 - x^4 (as the camera, findings #52): it
    // peaks at x = 1.5 (y = 1.6875) and turns back beyond, so fast mouse moves pushed past
    // 1.5 jerked the aim backwards. The mouse asks for a turn rate; the stick value giving it
    // is found by inverting the curve on [0, 1.5], capped at the peak rate.
    // 0.35: the feel the user chose (2026-11-02) is Aim Sensitivity 1.0.
    const float gain = 0.35f * float(REXCVAR_GET(ng2_bow_aim_gain));
    g_bow_pending_x += sx * gain;
    g_bow_pending_y += sy * gain;
    auto emit = [](float& pending) {
      constexpr float kPeak = 1.6875f;
      // The excess is dropped, not carried: carried turn kept the aim going after the mouse
      // stopped (user, 2026-11-02).
      const float r = std::clamp(pending, -kPeak, kPeak);
      pending = 0.0f;
      if (!REXCVAR_GET(ng2_bow_aim_invert_curve)) return std::clamp(r, -1.5f, 1.5f);
      float lo = 0.0f, hi = 1.5f;
      const float want = std::abs(r);
      for (int i = 0; i < 24; ++i) {
        const float mid = (lo + hi) * 0.5f;
        (2.0f * mid * mid * mid - mid * mid * mid * mid < want ? lo : hi) = mid;
      }
      return std::copysign((lo + hi) * 0.5f, r);
    };
    const float vx = std::clamp(LoadF(rec + 20) + emit(g_bow_pending_x), -1.5f, 1.5f);
    const float vy = std::clamp(LoadF(rec + 24) + emit(g_bow_pending_y), -1.5f, 1.5f);
    StoreF(rec + 20, vx);
    StoreF(rec + 24, vy);
    uint8_t* smooth = mem->TranslateVirtual<uint8_t*>(0x84C569F0 + 0x420);
    StoreF(smooth, vx);
    StoreF(smooth + 4, vy);
    g_bow_mouse_frames = 1;
    return;
  }
  const uint32_t stick = bow ? 20 : 28;
  StoreF(rec + stick, std::clamp(LoadF(rec + stick) + sx, -1.0f, 1.0f));
  StoreF(rec + stick + 4, std::clamp(LoadF(rec + stick + 4) + sy, -1.0f, 1.0f));
}
