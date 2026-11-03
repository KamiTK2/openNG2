// PlayStation button prompts (findings #86). NG2 draws every pad prompt - the icons inside text
// (@[pad:NAME] -> a glyph) and the pause / title menu icons - from one 512x256 8-bit palettized
// texture, TID_CAPTION, in a resident XPR2 pack: 8x4 cells of 64x64 (row 0: X Y B A LT RT LB RB,
// rows 1-3: sticks, D-pad, BACK, START), palette PALETTE_TID_CAPTION (256x1 ARGB). When a
// PlayStation pad is in use, the cells that differ (face buttons, LT/RT/LB/RB, BACK/START) are
// replaced with Xelu's CC0 PlayStation prompts (playstation/, embedded via ng2.rc), the new
// colours take palette slots the replaced cells no longer use, and the indices + palette are
// written into guest memory (the GPU texture cache sees the write). Switching back writes the
// original. Sticks and D-pad keep the game's own icons.
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/thread/mutex.h>

REXCVAR_DEFINE_INT32(ng2_playstation_prompts, 0, "NG2",
                     "PlayStation button prompts: 0 = when a PS4/PS5 pad is in use, 1 = always "
                     "(PS5), 2 = never, 3 = always (PS4)");
REXCVAR_DEFINE_BOOL(ng2_menu_exit_label, true, "NG2",
                    "Main menu: draw EXIT TO DESKTOP over the Xbox LIVE Marketplace label");

namespace ng2 {
namespace {

constexpr uint32_t kW = 512, kH = 256, kCell = 64;
constexpr uintptr_t kGuestBase = 0x100000000ull;  // host address of guest 0 (crash_handler.cpp)

// Guest [addr, addr + n) allocated and readable, from the emulator's own page table (no
// syscall). VirtualQuery per 4 KB page here slowed boot badly: ~130k calls per scan while the
// game's loading changes page protections all the time (user: "the boot was super slow").
// All addresses here are in the 0xA0000000 view; the game allocates these packs through other
// views of the same physical memory, which that view's own page table doesn't show, so ask the
// parent physical heap with physical addresses (as SharedMemory does).
// Two checks, both needed: the physical heap (allocated at all; cheap, no syscall) and the host
// protection of this view (a page allocated through another view can still be NoAccess here:
// reading 0xABEA0000 crashed with the heap check alone, exit-label-4 / boot-shot).
bool HostReadable(uint32_t addr, size_t n) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(kGuestBase + addr);
  size_t done = 0;
  while (done < n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p + done, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mbi.Protect)
      return false;
    done += size_t(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - (p + done));
  }
  return true;
}
bool Readable(uint32_t addr, size_t n) {
  if (!n) return true;
  if (addr >= 0xA0000000u && addr < 0xC0000000u) {
    auto* phys = REX_KERNEL_STATE()->memory()->GetPhysicalHeap();
    const uint32_t p = addr - 0xA0000000u;
    if (!phys || phys->QueryRangeAccess(p, uint32_t(p + n - 1)) == rex::memory::PageAccess::kNoAccess)
      return false;
  }
  return HostReadable(addr, n);
}

// Calls fn(page address) for every 4 KB page start in 0xA0000000-0xC0000000 (64 KB-page
// physical view) whose 64 KB page is readable.
// hint: where the pack has always loaded (title.bin, mainmenu.bin, every log so far); tried
// first, so normally no scan runs at all. The full scan (28-170 ms with the heap lock taken per
// 64 KB page, a visible hitch at boot) runs at most every 10 s per caller (*next_full).
template <typename Fn>
void ForEachPhysicalPage(Fn&& fn, uint32_t hint, std::chrono::steady_clock::time_point* next_full) {
  if (hint && Readable(hint, 0x1000) && fn(hint)) return;
  const auto now = std::chrono::steady_clock::now();
  if (*next_full == std::chrono::steady_clock::time_point{}) {
    *next_full = now + std::chrono::seconds(20);  // boot: the packs load at the hint first
    return;
  }
  if (now < *next_full) return;
  *next_full = now + std::chrono::seconds(10);
  const auto t0 = std::chrono::steady_clock::now();
  uint32_t readable = 0;
  for (uint32_t big = 0xA0000000u; big < 0xC0000000u; big += 0x10000) {
    if (!Readable(big, 0x10000)) continue;
    ++readable;
    for (uint32_t a = big; a < big + 0x10000; a += 0x1000)
      if (fn(a)) {
        big = 0xC0000000u;
        break;
      }
  }
  const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  static int logged = 0;
  if (ms > 5.0 && logged < 20) {
    ++logged;
    REXLOG_INFO("NG2 texture scan: {:.1f} ms ({} readable 64 KB pages)", ms, readable);
  }
}
uint8_t* Host(uint32_t addr) { return reinterpret_cast<uint8_t*>(kGuestBase + addr); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Fetch-constant base address (0xE0000000 view: physical + 0x1000) -> 0xA0000000 view.
uint32_t PhysView(uint32_t fetch_word1) {
  const uint32_t a = fetch_word1 & 0xFFFFF000u;
  const uint32_t phys = (a & 0x1FFFFFFFu) + (a >= 0xE0000000u ? 0x1000u : 0u);
  return 0xA0000000u + phys;
}

// Xenos 2D tiled byte offset of texel (x, y) (bpb_log2: 0 = 8 bpp, 2 = 32 bpp; pitch in texels,
// multiple of 32). Same formula as the SDK texture loader.
uint32_t TiledOffset(uint32_t x, uint32_t y, uint32_t pitch, uint32_t bpb_log2) {
  uint32_t macro = ((y >> 5) * (pitch >> 5)) << (bpb_log2 + 7);
  uint32_t micro = ((y & 6) << 2) << bpb_log2;
  const uint32_t row = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + bpb_log2)) +
                       ((y & 1) << 4);
  macro = (x >> 5) << (bpb_log2 + 7);
  micro = (x & 7) << bpb_log2;
  const uint32_t off = row + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

struct Rgba {
  float r, g, b, a;
};
Rgba Mix(const Rgba& a, const Rgba& b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

struct Image {
  std::vector<Rgba> px = std::vector<Rgba>(kW * kH);
  Rgba& at(int x, int y) { return px[size_t(y) * kW + x]; }
};

// One PlayStation icon (straight RGBA, from the embedded PNG).
struct Icon {
  uint32_t w = 0, h = 0;
  std::vector<Rgba> px;
};

// Decodes RCDATA resource `id` (a PNG) with WIC. Worker thread (COM initialised there).
bool LoadIcon(int id, Icon& out) {
  HMODULE mod = GetModuleHandleW(nullptr);
  HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
  if (!res) return false;
  HGLOBAL h = LoadResource(mod, res);
  const void* data = h ? LockResource(h) : nullptr;
  const DWORD size = SizeofResource(mod, res);
  if (!data || !size) return false;
  IWICImagingFactory* f = nullptr;
  IWICStream* s = nullptr;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  bool ok = false;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&f))) &&
      SUCCEEDED(f->CreateStream(&s)) &&
      SUCCEEDED(s->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), size)) &&
      SUCCEEDED(f->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(f->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                 nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    UINT w = 0, hh = 0;
    conv->GetSize(&w, &hh);
    std::vector<uint8_t> raw(size_t(w) * hh * 4);
    if (w && hh && SUCCEEDED(conv->CopyPixels(nullptr, w * 4, UINT(raw.size()), raw.data()))) {
      out.w = w, out.h = hh;
      out.px.resize(size_t(w) * hh);
      for (size_t i = 0; i < out.px.size(); ++i)
        out.px[i] = {raw[i * 4] / 255.0f, raw[i * 4 + 1] / 255.0f, raw[i * 4 + 2] / 255.0f,
                     raw[i * 4 + 3] / 255.0f};
      ok = true;
    }
  }
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  if (s) s->Release();
  if (f) f->Release();
  return ok;
}

// Bilinear fetch, premultiplied, transparent outside.
Rgba Sample(const Icon& ic, float x, float y) {
  auto at = [&](int xx, int yy) -> Rgba {
    if (xx < 0 || yy < 0 || xx >= int(ic.w) || yy >= int(ic.h)) return {0, 0, 0, 0};
    const Rgba& c = ic.px[size_t(yy) * ic.w + xx];
    return {c.r * c.a, c.g * c.a, c.b * c.a, c.a};
  };
  const int x0 = int(std::floor(x - 0.5f)), y0 = int(std::floor(y - 0.5f));
  const float fx = x - 0.5f - x0, fy = y - 0.5f - y0;
  const Rgba a = Mix(at(x0, y0), at(x0 + 1, y0), fx), b = Mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx);
  return Mix(a, b, fy);
}

// Replaces cell (col, row) with `ic`, scaled so its visible part is about as big as the
// original icon's (40..46 px) and centred where the original was.
void PlaceIcon(Image& img, int col, int row, const Icon& ic) {
  const int x0 = col * kCell, y0 = row * kCell;
  int bx0 = kCell, bx1 = -1, by0 = kCell, by1 = -1;
  for (int y = 0; y < int(kCell); ++y)
    for (int x = 0; x < int(kCell); ++x)
      if (img.at(x0 + x, y0 + y).a > 0.5f) {
        bx0 = std::min(bx0, x), bx1 = std::max(bx1, x), by0 = std::min(by0, y), by1 = std::max(by1, y);
      }
  const float ocx = bx1 >= 0 ? (bx0 + bx1 + 1) * 0.5f : kCell * 0.5f;
  const float ocy = bx1 >= 0 ? (by0 + by1 + 1) * 0.5f : kCell * 0.5f;
  const float target = bx1 >= 0 ? std::clamp(float(std::max(bx1 - bx0, by1 - by0) + 1), 40.0f, 46.0f) : 44.0f;
  int ix0 = int(ic.w), ix1 = -1, iy0 = int(ic.h), iy1 = -1;
  for (uint32_t y = 0; y < ic.h; ++y)
    for (uint32_t x = 0; x < ic.w; ++x)
      if (ic.px[y * ic.w + x].a > 0.05f) {
        ix0 = std::min(ix0, int(x)), ix1 = std::max(ix1, int(x)), iy0 = std::min(iy0, int(y)),
        iy1 = std::max(iy1, int(y));
      }
  if (ix1 < 0) return;
  const float icx = (ix0 + ix1 + 1) * 0.5f, icy = (iy0 + iy1 + 1) * 0.5f;
  const float scale = float(std::max(ix1 - ix0, iy1 - iy0) + 1) / target;  // icon px per cell px
  constexpr int kSs = 4;  // 4x4 supersampling for a clean ~2.3x reduction
  for (int y = 0; y < int(kCell); ++y) {
    for (int x = 0; x < int(kCell); ++x) {
      Rgba acc{0, 0, 0, 0};
      for (int sy = 0; sy < kSs; ++sy)
        for (int sx = 0; sx < kSs; ++sx) {
          const float cx = x + (sx + 0.5f) / kSs, cy = y + (sy + 0.5f) / kSs;
          const Rgba c = Sample(ic, icx + (cx - ocx) * scale, icy + (cy - ocy) * scale);
          acc = {acc.r + c.r, acc.g + c.g, acc.b + c.b, acc.a + c.a};
        }
      const float n = float(kSs * kSs), a = acc.a / n;
      img.at(x0 + x, y0 + y) = a > 0.0f ? Rgba{acc.r / n / a, acc.g / n / a, acc.b / n / a, a}
                                        : Rgba{0, 0, 0, 0};
    }
  }
}

// Cells replaced for PlayStation pads, in resource order (ng2.rc: 200 + set * 10 + i).
struct PsButton {
  int col, row;
};
constexpr PsButton kPsButtons[10] = {
    {0, 0}, {1, 0}, {2, 0}, {3, 0},  // X Y B A -> Square Triangle Circle Cross
    {4, 0}, {5, 0}, {6, 0}, {7, 0},  // LT RT LB RB -> L2 R2 L1 R1
    {6, 3}, {7, 3},                  // BACK START -> Share (PS4) / Create (PS5), Options
};

struct Sheet {
  uint32_t pack = 0;           // XPR2 header (0xA0000000 view)
  uint32_t idx_addr = 0, pal_addr = 0;
  std::vector<uint8_t> orig_idx, orig_pal;  // raw tiled bytes as loaded
  std::vector<uint8_t> ps_idx[2], ps_pal[2];  // [0] PS4 labels, [1] PS5 labels
  bool ps_built[2] = {false, false};
  int shown = 0;  // 0 original, 1 PS4, 2 PS5
};
Sheet g_sheet;

constexpr size_t kIdxBytes = size_t(kW) * kH;           // 8 bpp, pitch 512, height 256
constexpr size_t kPalBytes = size_t(256) * 32 * 4;      // 256x1 at 32 bpp, tiled (32 rows)

// Finds TID_CAPTION (512x256 k_8) + PALETTE_TID_CAPTION in a resident XPR2 pack.
bool FindSheet(Sheet& s) {
  static std::chrono::steady_clock::time_point next_full{};
  bool ok = false;
  ForEachPhysicalPage([&](uint32_t a) {
    const uint8_t* p = Host(a);
    if (std::memcmp(p, "XPR2", 4) != 0) return false;
    const uint32_t count = Be32(p + 12);
    if (count < 2 || count > 512 || !Readable(a, 16 + 16 * count)) return false;
    for (uint32_t k = 0; k + 1 < count; ++k) {
      const uint8_t* e = p + 16 + 16 * k;
      if (std::memcmp(e, "TX2D", 4) != 0) continue;
      const uint32_t name = PhysView(Be32(e + 12) & ~0xFFFu) + (Be32(e + 12) & 0xFFF);
      if (!Readable(name, 12) || std::memcmp(Host(name), "TID_CAPTION", 12) != 0) continue;
      const uint8_t* t = p + 12 + Be32(e + 4) + 28;  // texture fetch constant
      const uint8_t* tp = p + 12 + Be32(e + 16 + 4) + 28;
      if (!Readable(uint32_t(t - Host(0)), 24) || !Readable(uint32_t(tp - Host(0)), 24)) continue;
      const uint32_t w1 = Be32(t + 4), w2 = Be32(t + 8), pw1 = Be32(tp + 4), pw2 = Be32(tp + 8);
      if ((w1 & 0x3F) != 2 || (w2 & 0x1FFF) + 1 != kW || ((w2 >> 13) & 0x1FFF) + 1 != kH) continue;
      if ((pw1 & 0x3F) != 6 || (pw2 & 0x1FFF) + 1 != 256) continue;
      s.pack = a;
      s.idx_addr = PhysView(w1);
      s.pal_addr = PhysView(pw1);
      ok = Readable(s.idx_addr, kIdxBytes) && Readable(s.pal_addr, kPalBytes);
      return true;  // stop scanning either way
    }
    return false;
  }, 0xB097B000u, &next_full);
  return ok;
}

bool SheetStillThere(const Sheet& s) {
  return s.pack && Readable(s.pack, 16) && std::memcmp(Host(s.pack), "XPR2", 4) == 0;
}

// Builds the PlayStation version from the original indices + palette.
void BuildPs(Sheet& s, int ps5) {
  std::array<Rgba, 256> pal{};
  for (int i = 0; i < 256; ++i) {
    const uint8_t* c = s.orig_pal.data() + TiledOffset(i, 0, 256, 2);
    pal[i] = {c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f, c[0] / 255.0f};
  }
  std::vector<uint8_t> idx(kW * kH);
  Image img;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) {
      idx[y * kW + x] = s.orig_idx[TiledOffset(x, y, kW, 0)];
      img.at(x, y) = pal[idx[y * kW + x]];
    }
  const Image before = img;
  int placed = 0;
  for (int i = 0; i < 10; ++i) {
    Icon ic;
    if (!LoadIcon(200 + ps5 * 10 + i, ic)) continue;
    PlaceIcon(img, kPsButtons[i].col, kPsButtons[i].row, ic);
    ++placed;
  }
  if (placed == 0) REXLOG_WARN("NG2 PS prompts: no PlayStation icons could be loaded");
  // Palette: keep every entry still used by unchanged pixels, give the changed pixels the rest
  // (median cut), then map each changed pixel to its nearest entry.
  auto same = [](const Rgba& a, const Rgba& b) {
    return std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b) + std::abs(a.a - b.a) < 1e-4f;
  };
  std::vector<uint32_t> changed;
  std::array<bool, 256> used{};
  for (uint32_t i = 0; i < kW * kH; ++i) {
    if (same(img.px[i], before.px[i])) used[idx[i]] = true;
    else changed.push_back(i);
  }
  std::vector<int> free_slots;
  for (int i = 0; i < 256; ++i)
    if (!used[i]) free_slots.push_back(i);
  std::array<Rgba, 256> npal = pal;
  std::array<bool, 256> valid = used;
  if (!free_slots.empty() && !changed.empty()) {
    struct Box {
      std::vector<uint32_t> px;
    };
    std::vector<Box> boxes{{changed}};
    auto chan = [&](uint32_t p, int c) {
      const Rgba& v = img.px[p];
      return c == 0 ? v.r : c == 1 ? v.g : c == 2 ? v.b : v.a;
    };
    while (boxes.size() < free_slots.size()) {
      int best = -1, best_c = 0;
      float best_range = 0.0f;
      for (int b = 0; b < int(boxes.size()); ++b) {
        if (boxes[b].px.size() < 2) continue;
        for (int c = 0; c < 4; ++c) {
          float lo = 1e9f, hi = -1e9f;
          for (uint32_t p : boxes[b].px) lo = std::min(lo, chan(p, c)), hi = std::max(hi, chan(p, c));
          const float range = (hi - lo) * std::sqrt(float(boxes[b].px.size()));
          if (range > best_range) best_range = range, best = b, best_c = c;
        }
      }
      if (best < 0) break;
      auto& v = boxes[best].px;
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end(),
                       [&](uint32_t a, uint32_t b) { return chan(a, best_c) < chan(b, best_c); });
      Box upper{std::vector<uint32_t>(v.begin() + v.size() / 2, v.end())};
      v.resize(v.size() / 2);
      boxes.push_back(std::move(upper));
    }
    for (size_t b = 0; b < boxes.size(); ++b) {
      Rgba m{0, 0, 0, 0};
      for (uint32_t p : boxes[b].px) m = {m.r + img.px[p].r, m.g + img.px[p].g, m.b + img.px[p].b, m.a + img.px[p].a};
      const float n = float(std::max<size_t>(1, boxes[b].px.size()));
      npal[free_slots[b]] = {m.r / n, m.g / n, m.b / n, m.a / n};
      valid[free_slots[b]] = true;
    }
  }
  for (uint32_t p : changed) {
    const Rgba& c = img.px[p];
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < 256; ++i) {
      if (!valid[i]) continue;
      const Rgba& q = npal[i];
      // Alpha matters most: a translucent pixel mapped opaque leaves a hard fringe.
      const float d = (c.r - q.r) * (c.r - q.r) + (c.g - q.g) * (c.g - q.g) + (c.b - q.b) * (c.b - q.b) +
                      2.0f * (c.a - q.a) * (c.a - q.a);
      if (d < bd) bd = d, best = i;
    }
    idx[p] = uint8_t(best);
  }
  s.ps_idx[ps5] = s.orig_idx;
  for (uint32_t y = 0; y < kH; ++y)
    for (uint32_t x = 0; x < kW; ++x) s.ps_idx[ps5][TiledOffset(x, y, kW, 0)] = idx[y * kW + x];
  s.ps_pal[ps5] = s.orig_pal;
  for (int i = 0; i < 256; ++i) {
    uint8_t* c = s.ps_pal[ps5].data() + TiledOffset(i, 0, 256, 2);
    auto q = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    c[0] = q(npal[i].a), c[1] = q(npal[i].r), c[2] = q(npal[i].g), c[3] = q(npal[i].b);
  }
  s.ps_built[ps5] = true;
  REXLOG_INFO("NG2 PS prompts: built {} sheet ({} changed pixels, {} free palette slots)",
              ps5 ? "PS5" : "PS4", changed.size(), free_slots.size());
}

void WriteTexture(uint32_t idx_addr, const std::vector<uint8_t>& idx, uint32_t pal_addr,
                  const std::vector<uint8_t>& pal);
void Write(const Sheet& s, const std::vector<uint8_t>& idx, const std::vector<uint8_t>& pal) {
  WriteTexture(s.idx_addr, idx, s.pal_addr, pal);
}
void WriteTexture(uint32_t idx_addr, const std::vector<uint8_t>& idx, uint32_t pal_addr,
                  const std::vector<uint8_t>& pal) {
  struct {
    uint32_t idx_addr, pal_addr;
  } s{idx_addr, pal_addr};
  if (!Readable(s.pal_addr, pal.size()) || !Readable(s.idx_addr, idx.size())) return;  // exiting
  // Once the GPU has loaded the texture it watches these pages, and a plain write from this
  // (non-guest) thread is neither noticed nor serviced: the prompts didn't change in game.
  // Fire the watch callbacks first (texture cache drops its copy, pages unprotected), as the
  // SDK's own GPU-side writes do.
  auto* mem = REX_KERNEL_STATE()->memory();
  auto invalidate = [&] {
    mem->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        s.pal_addr, uint32_t(pal.size()), true, false, true);
    mem->TriggerPhysicalMemoryCallbacks(rex::thread::global_critical_region::AcquireDirect(),
                                        s.idx_addr, uint32_t(idx.size()), true, false, true);
  };
  invalidate();  // unprotects watched pages so the copy below can't fault
  std::memcpy(Host(s.pal_addr), pal.data(), pal.size());
  std::memcpy(Host(s.idx_addr), idx.data(), idx.size());
  // Then drop the GPU's copies outright: after the first swap the pages are no longer watched,
  // and the watch path alone ignored every later swap (user: "only worked once").
  mem->InvalidatePhysicalMemoryRange(s.pal_addr - 0xA0000000u, uint32_t(pal.size()));
  mem->InvalidatePhysicalMemoryRange(s.idx_addr - 0xA0000000u, uint32_t(idx.size()));
}

// ---- Main menu EXIT TO DESKTOP label (findings #92) --------------------------------------------
// The main menu's last item "Xbox LIVE Marketplace" is a picture, mode_market_tga (520x90 k_8 +
// PALETTE_mode_market_tga) in the title pack, next to t_0x2000d "PRESS START BUTTON" and
// t_0x2000f "START-TASTE DRUCKEN" (585x91, same font, capitals 33 px vs the label's 25 px). The
// label is rebuilt from those letters - EXIT TO DESKTOP - so it matches the menu exactly; A on
// the item is caught in recomp_settings.cpp. Built from the player's own textures at runtime.

struct Tex {
  uint32_t idx_addr = 0, pal_addr = 0, w = 0, h = 0, pitch = 0;
  size_t idx_bytes = 0;
};

// Finds the named k_8 textures (each followed by its PALETTE_ entry) in one resident XPR2 pack.
bool FindPackTextures(const std::vector<std::string>& names, std::vector<Tex>& out, uint32_t* pack) {
  static std::chrono::steady_clock::time_point next_full{};
  bool ok = false;
  ForEachPhysicalPage([&](uint32_t a) {
    const uint8_t* p = Host(a);
    if (std::memcmp(p, "XPR2", 4) != 0) return false;
    const uint32_t count = Be32(p + 12);
    if (count < 2 || count > 512 || !Readable(a, 16 + 16 * count)) return false;
    out.assign(names.size(), Tex{});
    size_t found = 0;
    for (uint32_t k = 0; k + 1 < count; ++k) {
      const uint8_t* e = p + 16 + 16 * k;
      if (std::memcmp(e, "TX2D", 4) != 0) continue;
      const uint32_t name = PhysView(Be32(e + 12) & ~0xFFFu) + (Be32(e + 12) & 0xFFF);
      if (!Readable(name, 32)) continue;
      for (size_t n = 0; n < names.size(); ++n) {
        if (out[n].w || std::strncmp(reinterpret_cast<const char*>(Host(name)), names[n].c_str(),
                                     names[n].size() + 1) != 0)
          continue;
        const uint8_t* t = p + 12 + Be32(e + 4) + 28;
        const uint8_t* tp = p + 12 + Be32(e + 16 + 4) + 28;
        if (!Readable(uint32_t(t - Host(0)), 24) || !Readable(uint32_t(tp - Host(0)), 24)) continue;
        const uint32_t w1 = Be32(t + 4), w2 = Be32(t + 8), pw1 = Be32(tp + 4);
        if ((w1 & 0x3F) != 2 || (pw1 & 0x3F) != 6) continue;
        Tex x;
        x.w = (w2 & 0x1FFF) + 1, x.h = ((w2 >> 13) & 0x1FFF) + 1, x.pitch = (x.w + 31) & ~31u;
        uint32_t max_off = 0;
        for (uint32_t y = 0; y < x.h; ++y)
          for (uint32_t xx = 0; xx < x.w; ++xx) max_off = std::max(max_off, TiledOffset(xx, y, x.pitch, 0));
        x.idx_bytes = (size_t(max_off) + 0x1000) & ~size_t(0xFFF);
        x.idx_addr = PhysView(w1), x.pal_addr = PhysView(pw1);
        if (!Readable(x.idx_addr, x.idx_bytes) || !Readable(x.pal_addr, kPalBytes)) continue;
        out[n] = x;
        ++found;
      }
    }
    if (found == names.size()) {
      if (pack) *pack = a;
      ok = true;
      return true;
    }
    return false;
  }, 0xAFDEB000u, &next_full);
  return ok;
}

std::vector<Rgba> DecodeTex(const Tex& t, std::array<Rgba, 256>* pal_out = nullptr) {
  std::array<Rgba, 256> pal{};
  for (int i = 0; i < 256; ++i) {
    const uint8_t* c = Host(t.pal_addr) + TiledOffset(i, 0, 256, 2);
    pal[i] = {c[1] / 255.0f, c[2] / 255.0f, c[3] / 255.0f, c[0] / 255.0f};
  }
  if (pal_out) *pal_out = pal;
  std::vector<Rgba> px(size_t(t.w) * t.h);
  const uint8_t* idx = Host(t.idx_addr);
  for (uint32_t y = 0; y < t.h; ++y)
    for (uint32_t x = 0; x < t.w; ++x) px[size_t(y) * t.w + x] = pal[idx[TiledOffset(x, y, t.pitch, 0)]];
  return px;
}

// Median-cut palette for a whole image: entry 0 transparent, 255 for the rest.
void Quantize(const std::vector<Rgba>& img, std::vector<uint8_t>& idx, std::array<Rgba, 256>& pal) {
  idx.assign(img.size(), 0);
  pal.fill({0, 0, 0, 0});
  std::vector<uint32_t> vis;
  for (uint32_t i = 0; i < img.size(); ++i)
    if (img[i].a > 1.0f / 255.0f) vis.push_back(i);
  if (vis.empty()) return;
  auto chan = [&](uint32_t p, int c) {
    const Rgba& v = img[p];
    return c == 0 ? v.r : c == 1 ? v.g : c == 2 ? v.b : v.a;
  };
  std::vector<std::vector<uint32_t>> boxes{vis};
  while (boxes.size() < 255) {
    int best = -1, best_c = 0;
    float best_range = 0.0f;
    for (int b = 0; b < int(boxes.size()); ++b) {
      if (boxes[b].size() < 2) continue;
      for (int c = 0; c < 4; ++c) {
        float lo = 1e9f, hi = -1e9f;
        for (uint32_t p : boxes[b]) lo = std::min(lo, chan(p, c)), hi = std::max(hi, chan(p, c));
        const float range = (hi - lo) * std::sqrt(float(boxes[b].size()));
        if (range > best_range) best_range = range, best = b, best_c = c;
      }
    }
    if (best < 0) break;
    auto& v = boxes[best];
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end(),
                     [&](uint32_t a, uint32_t b) { return chan(a, best_c) < chan(b, best_c); });
    std::vector<uint32_t> upper(v.begin() + v.size() / 2, v.end());
    v.resize(v.size() / 2);
    boxes.push_back(std::move(upper));
  }
  for (size_t b = 0; b < boxes.size(); ++b) {
    Rgba m{0, 0, 0, 0};
    for (uint32_t p : boxes[b]) m = {m.r + img[p].r, m.g + img[p].g, m.b + img[p].b, m.a + img[p].a};
    const float n = float(std::max<size_t>(1, boxes[b].size()));
    pal[b + 1] = {m.r / n, m.g / n, m.b / n, m.a / n};
  }
  for (uint32_t p : vis) {
    const Rgba& c = img[p];
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i <= int(boxes.size()); ++i) {
      const Rgba& q = pal[i];
      const float d = (c.r - q.r) * (c.r - q.r) + (c.g - q.g) * (c.g - q.g) + (c.b - q.b) * (c.b - q.b) +
                      2.0f * (c.a - q.a) * (c.a - q.a);
      if (d < bd) bd = d, best = i;
    }
    idx[p] = uint8_t(best);
  }
}

// Letter body ("ink": opaque bright fill, not the glow) per column.
bool InkAt(const std::vector<Rgba>& px, uint32_t w, uint32_t x, uint32_t y) {
  const Rgba& c = px[size_t(y) * w + x];
  return c.a > 200.0f / 255.0f && 0.30f * c.r + 0.59f * c.g + 0.11f * c.b > 150.0f / 255.0f;
}
// Column in [lo, hi] with the least ink (the gap between two touching letters).
uint32_t SplitColumn(const std::vector<Rgba>& px, uint32_t w, uint32_t h, uint32_t lo, uint32_t hi) {
  uint32_t best = lo, best_n = UINT32_MAX;
  for (uint32_t x = lo; x <= hi; ++x) {
    uint32_t n = 0;
    for (uint32_t y = 0; y < h; ++y) n += InkAt(px, w, x, y);
    if (n < best_n) best_n = n, best = x;
  }
  return best;
}

struct MenuLabel {
  uint32_t pack = 0;
  Tex label;
  std::vector<uint8_t> new_idx, new_pal;
  bool built = false, failed = false;
};
MenuLabel g_menu_label;

void BuildMenuLabel(MenuLabel& m, const std::vector<Tex>& t) {
  // t[0] mode_market_tga, t[1] PRESS START BUTTON, t[2] START-TASTE DRUCKEN
  const std::vector<Rgba> mk = DecodeTex(t[0]), press = DecodeTex(t[1]), druck = DecodeTex(t[2]);
  const std::vector<Rgba>* src[3] = {&press, &druck, &mk};
  const uint32_t sw[3] = {t[1].w, t[2].w, t[0].w}, sh[3] = {t[1].h, t[2].h, t[0].h};
  // Ink column ranges measured on the English textures (findings #92); P and D touch their
  // neighbours, so their right edge is the emptiest column near the expected gap.
  const uint32_t p_end = SplitColumn(press, sw[0], sh[0], 46, 60) - 1;
  const uint32_t d_end = SplitColumn(druck, sw[1], sh[1], 384, 398) - 1;
  struct Glyph {
    int src;
    uint32_t x0, x1;
  };
  const Glyph E{1, 506, 533}, X{2, 65, 84}, I{2, 180, 193}, T{0, 237, 261}, O{0, 504, 532},
      D{1, 364, d_end}, S{0, 117, 141}, K{1, 479, 504}, P{0, 26, p_end};
  const Glyph kSpace{-1, 0, 0};
  const Glyph text[] = {E, X, I, T, kSpace, T, O, kSpace, D, E, S, K, T, O, P};
  // Capitals: 33 px (rows 27-60) in the 585x91 sources, 25 px (rows 28-52) in the label.
  const float scale_src[3] = {25.0f / 33.0f, 25.0f / 33.0f, 1.0f};
  const float top_src[3] = {27.0f, 27.0f, 28.0f};
  constexpr float kTop = 28.0f, kGap = 3.0f, kSpaceW = 13.0f, kMargin = 3.0f;
  float total = 0.0f;
  for (size_t i = 0; i < std::size(text); ++i) {
    const Glyph& g = text[i];
    total += g.src < 0 ? kSpaceW : (g.x1 - g.x0 + 1) * scale_src[g.src];
    if (i + 1 < std::size(text) && g.src >= 0 && text[i + 1].src >= 0) total += kGap;
  }
  const uint32_t W = t[0].w, H = t[0].h;
  std::vector<Rgba> out(size_t(W) * H, Rgba{0, 0, 0, 0});
  float pen = (65.0f + 457.0f) * 0.5f - total * 0.5f;  // centred like the original label
  for (size_t i = 0; i < std::size(text); ++i) {
    const Glyph& g = text[i];
    if (g.src < 0) {
      pen += kSpaceW;
      continue;
    }
    const std::vector<Rgba>& s = *src[g.src];
    const float sc = scale_src[g.src];
    const float w_dst = (g.x1 - g.x0 + 1) * sc;
    // Destination box incl. the glow margin; sample the source with 3x3 supersampling.
    const int dx0 = int(std::floor(pen - kMargin * sc)), dx1 = int(std::ceil(pen + w_dst + kMargin * sc));
    for (int y = 0; y < int(H); ++y) {
      for (int x = std::max(0, dx0); x <= std::min(int(W) - 1, dx1); ++x) {
        Rgba acc{0, 0, 0, 0};
        for (int sy = 0; sy < 3; ++sy)
          for (int sx = 0; sx < 3; ++sx) {
            const float fx = g.x0 + (x + (sx + 0.5f) / 3.0f - pen) / sc;
            const float fy = top_src[g.src] + (y + (sy + 0.5f) / 3.0f - kTop) / sc;
            if (fx < g.x0 - kMargin || fx > g.x1 + 1 + kMargin || fx < 0 || fy < 0 ||
                fx >= float(sw[g.src]) || fy >= float(sh[g.src]))
              continue;
            const Rgba& c = s[size_t(int(fy)) * sw[g.src] + size_t(int(fx))];
            acc = {acc.r + c.r * c.a, acc.g + c.g * c.a, acc.b + c.b * c.a, acc.a + c.a};
          }
        const float a = acc.a / 9.0f;
        if (a <= 0.0f) continue;
        const Rgba c{acc.r / acc.a, acc.g / acc.a, acc.b / acc.a, a};
        Rgba& d = out[size_t(y) * W + x];  // "over": glows of neighbours blend
        const float na = c.a + d.a * (1.0f - c.a);
        d = {(c.r * c.a + d.r * d.a * (1.0f - c.a)) / na, (c.g * c.a + d.g * d.a * (1.0f - c.a)) / na,
             (c.b * c.a + d.b * d.a * (1.0f - c.a)) / na, na};
      }
    }
    pen += w_dst + ((i + 1 < std::size(text) && text[i + 1].src >= 0) ? kGap : 0.0f);
  }
  std::vector<uint8_t> idx;
  std::array<Rgba, 256> pal;
  Quantize(out, idx, pal);
  m.new_idx.assign(Host(t[0].idx_addr), Host(t[0].idx_addr) + t[0].idx_bytes);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) m.new_idx[TiledOffset(x, y, t[0].pitch, 0)] = idx[size_t(y) * W + x];
  m.new_pal.assign(Host(t[0].pal_addr), Host(t[0].pal_addr) + kPalBytes);
  for (int i = 0; i < 256; ++i) {
    uint8_t* c = m.new_pal.data() + TiledOffset(i, 0, 256, 2);
    auto q = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    c[0] = q(pal[i].a), c[1] = q(pal[i].r), c[2] = q(pal[i].g), c[3] = q(pal[i].b);
  }
  m.built = true;
  REXLOG_INFO("NG2 menu label: EXIT TO DESKTOP built ({:.0f} px wide, P ends {}, D ends {})", total,
              p_end, d_end);
}

// Every 2 s: find the title pack; build the label once per load; rewrite it whenever the
// texture doesn't hold ours (first load, or the pack reloaded from disk).
void ApplyMenuLabel() {
  if (!REXCVAR_GET(ng2_menu_exit_label)) return;
  static auto next = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(2);
  MenuLabel& m = g_menu_label;
  if (m.pack && !(Readable(m.pack, 16) && std::memcmp(Host(m.pack), "XPR2", 4) == 0)) m = MenuLabel{};
  if (!m.pack) {
    std::vector<Tex> t;
    uint32_t pack = 0;
    if (!FindPackTextures({"mode_market_tga", "t_0x2000d", "t_0x2000f"}, t, &pack)) return;
    if (t[0].w != 520 || t[0].h != 90 || t[1].w != 585 || t[2].w != 585) {
      if (!m.failed) REXLOG_WARN("NG2 menu label: unexpected texture sizes, left alone");
      m.failed = true;
      return;
    }
    m.pack = pack;
    m.label = t[0];
    BuildMenuLabel(m, t);
  }
  if (!m.built) return;
  if (std::memcmp(Host(m.label.idx_addr), m.new_idx.data(), m.new_idx.size()) != 0 ||
      std::memcmp(Host(m.label.pal_addr), m.new_pal.data(), m.new_pal.size()) != 0) {
    WriteTexture(m.label.idx_addr, m.new_idx, m.label.pal_addr, m.new_pal);
    REXLOG_INFO("NG2 menu label: EXIT TO DESKTOP written at {:08X}", m.label.idx_addr);
  }
}

}  // namespace

// Worker thread only: the guest-memory writes fault into the GPU write watch, which takes the
// GPU's global lock; from the UI thread that deadlocked at boot (ps-prompts-1: AppHang).
static void Apply(int want) {
  static auto next_check = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  Sheet& s = g_sheet;
  // Every 2 s: find the sheet (only once PlayStation prompts are wanted), notice an unloaded
  // pack, and notice the game reloading the original pixels over ours.
  if (now >= next_check) {
    next_check = now + std::chrono::seconds(2);
    if (s.pack && !SheetStillThere(s)) {
      REXLOG_INFO("NG2 PS prompts: icon sheet gone (pack unloaded)");
      s = Sheet{};
    }
    if (!s.pack) {
      if (want == 0) return;  // nothing to restore, nothing to show
      Sheet found;
      if (!FindSheet(found)) return;
      found.orig_idx.assign(Host(found.idx_addr), Host(found.idx_addr) + kIdxBytes);
      found.orig_pal.assign(Host(found.pal_addr), Host(found.pal_addr) + kPalBytes);
      s = std::move(found);
      REXLOG_INFO("NG2 PS prompts: icon sheet at {:08X} (palette {:08X})", s.idx_addr, s.pal_addr);
    } else if (s.shown != 0) {
      // The game may have reloaded the pack's pixels (original art again): reapply.
      const auto& cur = s.ps_idx[s.shown - 1];
      if (std::memcmp(Host(s.idx_addr), cur.data(), 4096) != 0) s.shown = -1;
    }
  }
  if (!s.pack || want == s.shown) return;
  if (want == 0) {
    Write(s, s.orig_idx, s.orig_pal);
  } else {
    const int v = want - 1;
    if (!s.ps_built[v]) BuildPs(s, v);
    Write(s, s.ps_idx[v], s.ps_pal[v]);
  }
  s.shown = want;
  REXLOG_INFO("NG2 PS prompts: showing {}", want == 0 ? "Xbox" : want == 1 ? "PS4" : "PS5");
}

static std::atomic<int> g_want{0};

// Prompt style in use (0 Xbox, 1 PS4, 2 PS5), for prompts the app draws itself.
int CurrentPromptStyle() { return g_want.load(); }

// UI thread, once per frame. want: 0 Xbox, 1 PS4 (and older), 2 PS5.
void UpdatePlayStationPrompts(int want) {
  const int mode = REXCVAR_GET(ng2_playstation_prompts);
  if (mode == 1) want = 2;
  if (mode == 2) want = 0;
  if (mode == 3) want = 1;
  g_want = want;
  static std::once_flag started;
  std::call_once(started, [] {
    std::thread([] {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC (PNG decoding)
      for (;;) {
        Apply(g_want.load());
        ApplyMenuLabel();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }).detach();
  });
}

}  // namespace ng2
