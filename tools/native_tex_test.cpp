// Offline test of project/ng2/src/native/native_textures on the game's XPR2 bundles.
// Reads game data only; PNGs go wherever --png points and must never be committed.
//
//   native_tex_test <game dir> [--per-format N] [--png dir] [--png-count N]
//
// Checks per texture:
//  - decodes with no block past the bundle's data,
//  - with NG2_SDK_REFERENCE: same bytes as the SDK's guest layout + tiling + packed mip code,
//  - each mip level matches a box downsample of the level above (independent of the SDK).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../project/ng2/src/native/native_textures.h"

#ifdef NG2_SDK_REFERENCE
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#endif

using namespace ng2::native;
namespace fs = std::filesystem;

static uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// ---- BC decode, for PNGs and the mip check ------------------------------------------------
struct Rgba {
  uint8_t r, g, b, a;
};

static void Bc1Colors(const uint8_t* b, Rgba out[16]) {
  uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  auto expand = [](uint32_t c, int& r, int& g, int& b2) {
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b2 = (c & 31) * 255 / 31;
  };
  int r[4], g[4], bl[4];
  expand(c0, r[0], g[0], bl[0]);
  expand(c1, r[1], g[1], bl[1]);
  if (c0 > c1) {
    r[2] = (2 * r[0] + r[1]) / 3, g[2] = (2 * g[0] + g[1]) / 3, bl[2] = (2 * bl[0] + bl[1]) / 3;
    r[3] = (r[0] + 2 * r[1]) / 3, g[3] = (g[0] + 2 * g[1]) / 3, bl[3] = (bl[0] + 2 * bl[1]) / 3;
  } else {
    r[2] = (r[0] + r[1]) / 2, g[2] = (g[0] + g[1]) / 2, bl[2] = (bl[0] + bl[1]) / 2;
    r[3] = g[3] = bl[3] = 0;
  }
  uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    int s = (idx >> (i * 2)) & 3;
    out[i] = {uint8_t(r[s]), uint8_t(g[s]), uint8_t(bl[s]), 255};
  }
}

// Decodes one level into RGB(A). The channel order of the source is kept as stored.
static std::vector<Rgba> DecodeLevel(const TextureDesc& d, const LoadedTexture& t,
                                     const LoadedMip& m) {
  std::vector<Rgba> px(size_t(m.width) * m.height);
  const uint8_t* s = t.data.data() + m.offset;
  if (d.block_width == 4) {
    uint32_t bytes = d.bytes_per_block;
    for (uint32_t by = 0; by < m.rows; ++by) {
      for (uint32_t bx = 0; bx < m.row_bytes / bytes; ++bx) {
        Rgba c[16];
        const uint8_t* block = s + size_t(by) * m.row_bytes + size_t(bx) * bytes;
        Bc1Colors(bytes == 16 ? block + 8 : block, c);
        for (uint32_t y = 0; y < 4; ++y) {
          for (uint32_t x = 0; x < 4; ++x) {
            uint32_t px_x = bx * 4 + x, px_y = by * 4 + y;
            if (px_x < m.width && px_y < m.height) {
              px[size_t(px_y) * m.width + px_x] = c[y * 4 + x];
            }
          }
        }
      }
    }
  } else if (d.bytes_per_block == 1) {
    for (size_t i = 0; i < px.size(); ++i) {
      px[i] = {s[i], s[i], s[i], 255};
    }
  } else if (d.vk_format == kVkFormatR8G8B8A8Unorm) {
    for (size_t i = 0; i < px.size(); ++i) {
      px[i] = {s[i * 4], s[i * 4 + 1], s[i * 4 + 2], 255};
    }
  } else {
    for (size_t i = 0; i < px.size(); ++i) {
      uint32_t v;
      std::memcpy(&v, s + i * 4, 4);
      px[i] = {uint8_t((v & 1023) >> 2), uint8_t(((v >> 10) & 1023) >> 2),
               uint8_t(((v >> 20) & 1023) >> 2), 255};
    }
  }
  return px;
}

// Pearson correlation between a level's box downsample and the next level, on luma.
// Returns false when either side is too flat for the number to mean anything.
static bool MipCorrelation(const std::vector<Rgba>& a, uint32_t aw, uint32_t ah,
                           const std::vector<Rgba>& b, uint32_t bw, uint32_t bh, double& r) {
  auto luma = [](const Rgba& c) { return 0.3 * c.r + 0.59 * c.g + 0.11 * c.b; };
  std::vector<double> sum(size_t(bw) * bh, 0.0);
  std::vector<uint32_t> count(size_t(bw) * bh, 0);
  for (uint32_t y = 0; y < ah; ++y) {
    for (uint32_t x = 0; x < aw; ++x) {
      size_t i = size_t(y * bh / ah) * bw + x * bw / aw;
      sum[i] += luma(a[size_t(y) * aw + x]);
      ++count[i];
    }
  }
  double ma = 0, mb = 0, n = double(bw) * bh;
  for (size_t i = 0; i < sum.size(); ++i) {
    sum[i] /= count[i];
    ma += sum[i];
    mb += luma(b[i]);
  }
  ma /= n, mb /= n;
  double va = 0, vb = 0, cov = 0;
  for (size_t i = 0; i < sum.size(); ++i) {
    double da = sum[i] - ma, db = luma(b[i]) - mb;
    va += da * da, vb += db * db, cov += da * db;
  }
  if (va / n < 4.0 || vb / n < 4.0) {
    return false;
  }
  r = cov / std::sqrt(va * vb);
  return true;
}

// ---- PNG (stored deflate) -----------------------------------------------------------------
static uint32_t Crc(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFF) {
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
  }
  return crc;
}

static void PngChunk(FILE* f, const char* tag, const std::vector<uint8_t>& data) {
  uint8_t be[4] = {uint8_t(data.size() >> 24), uint8_t(data.size() >> 16), uint8_t(data.size() >> 8),
                   uint8_t(data.size())};
  fwrite(be, 1, 4, f);
  fwrite(tag, 1, 4, f);
  if (!data.empty()) {
    fwrite(data.data(), 1, data.size(), f);
  }
  uint32_t crc = Crc(reinterpret_cast<const uint8_t*>(tag), 4);
  crc = ~Crc(data.data(), data.size(), crc);
  uint8_t c[4] = {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)};
  fwrite(c, 1, 4, f);
}

static void WritePng(const fs::path& path, const std::vector<Rgba>& px, uint32_t w, uint32_t h) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) {
    return;
  }
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  fwrite(sig, 1, 8, f);
  std::vector<uint8_t> ihdr = {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w),
                               uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h),
                               8,                2,                0,               0,
                               0};
  PngChunk(f, "IHDR", ihdr);
  std::vector<uint8_t> raw;
  raw.reserve((size_t(w) * 3 + 1) * h);
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < w; ++x) {
      const Rgba& c = px[size_t(y) * w + x];
      raw.push_back(c.r), raw.push_back(c.g), raw.push_back(c.b);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t s1 = 1, s2 = 0;
  for (uint8_t v : raw) {
    s1 = (s1 + v) % 65521;
    s2 = (s2 + s1) % 65521;
  }
  for (size_t i = 0; i < raw.size();) {
    size_t n = std::min<size_t>(raw.size() - i, 65535);
    z.push_back(i + n == raw.size());
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n)), z.push_back(uint8_t((~n) >> 8));
    z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    i += n;
  }
  uint32_t adler = (s2 << 16) | s1;
  z.push_back(uint8_t(adler >> 24)), z.push_back(uint8_t(adler >> 16));
  z.push_back(uint8_t(adler >> 8)), z.push_back(uint8_t(adler));
  PngChunk(f, "IDAT", z);
  PngChunk(f, "IEND", {});
  fclose(f);
}

// ---- SDK reference ------------------------------------------------------------------------
#ifdef NG2_SDK_REFERENCE
// Linked from the SDK core library normally; this tool only pulls in the texture sources.
namespace rex {
uint8_t lzcnt(uint32_t v) { return uint8_t(std::countl_zero(v)); }
}  // namespace rex

namespace tu = rex::graphics::texture_util;
namespace xe = rex::graphics::xenos;

// Rebuilds a level with the SDK's layout and addressing. Bytes are copied through the same
// endian rule, since the SDK's swap lives in compute shaders.
static void SdkLevel(const TextureDesc& d, const LoadedMip& m, const tu::TextureGuestLayout& layout,
                     const uint8_t* base, size_t base_size, const uint8_t* mips, size_t mips_size,
                     std::vector<uint8_t>& out) {
  auto format = xe::TextureFormat(d.xenos_format);
  bool is_base = m.level == 0;
  uint32_t stored = is_base ? 0 : std::min(m.level, uint32_t(layout.packed_level));
  if (layout.packed_level == 0) {
    stored = 0;
  }
  const auto& level = is_base ? layout.base : layout.mips[stored];
  uint32_t offset = is_base ? 0 : layout.mip_offsets_bytes[stored];
  uint32_t ox = 0, oy = 0, oz = 0;
  if (m.level >= layout.packed_level) {
    tu::GetPackedMipOffset(d.width, d.height, 1, format, m.level, ox, oy, oz);
  }
  const uint8_t* src = is_base ? base : mips;
  size_t limit = is_base ? base_size : mips_size;
  uint32_t bytes = d.bytes_per_block;
  uint32_t log2_bytes = 31 - std::countl_zero(bytes);
  uint32_t pitch_blocks = level.row_pitch_bytes / bytes;
  out.assign(m.size, 0);
  for (uint32_t y = 0; y < m.rows; ++y) {
    for (uint32_t x = 0; x < m.row_bytes / bytes; ++x) {
      size_t at = offset;
      at += d.tiled ? size_t(tu::GetTiledOffset2D(int32_t(x + ox), int32_t(y + oy), pitch_blocks,
                                                  log2_bytes))
                    : size_t(level.row_pitch_bytes) * (y + oy) + size_t(x + ox) * bytes;
      if (at + bytes > limit) {
        continue;
      }
      uint8_t* dst = out.data() + size_t(y) * m.row_bytes + size_t(x) * bytes;
      for (uint32_t i = 0; i < bytes; ++i) {
        uint32_t j = i;
        if (d.endian == 1) j = i ^ 1;
        if (d.endian == 2) j = i ^ 3;
        dst[i] = src[at + j];
      }
    }
  }
}
#endif

// ---- Crunch tiling formula (a second, separately derived one) -----------------------------
static uint32_t CrunchOffset(uint32_t x, uint32_t y, uint32_t width, uint32_t log2_bpp) {
  uint32_t macro = ((y / 32) * (width / 32)) << (log2_bpp + 7);
  uint32_t micro = ((y & 6) << 2) << log2_bpp;
  uint32_t row = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) +
                 ((y & 1) << 4);
  macro = (x / 32) << (log2_bpp + 7);
  micro = (x & 7) << log2_bpp;
  uint32_t offset = row + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

static uint64_t CheckTilingFormulas() {
  uint64_t bad = 0;
  for (uint32_t log2 : {0u, 2u, 3u, 4u}) {
    for (uint32_t pitch : {32u, 64u, 128u, 256u, 512u}) {
      for (uint32_t y = 0; y < 256; ++y) {
        for (uint32_t x = 0; x < pitch; ++x) {
          bad += TiledBlockOffset(x, y, pitch, log2) != CrunchOffset(x, y, pitch, log2);
        }
      }
    }
  }
  return bad;
}

// ---- Run ----------------------------------------------------------------------------------
struct Stats {
  uint64_t seen = 0, decoded = 0, unsupported = 0, truncated = 0, levels = 0, bytes = 0;
  uint64_t sdk_textures = 0, sdk_levels = 0, sdk_bad_levels = 0;
  uint64_t mip_pairs = 0, mip_flat = 0, mip_ge90 = 0, mip_ge70 = 0, mip_ge50 = 0, mip_low = 0;
  uint64_t tail_pairs = 0, tail_low = 0;
};

static std::mutex g_mutex;
static std::map<uint32_t, Stats> g_stats;
static std::vector<std::string> g_issues;
static std::atomic<int> g_png_left{0};
static fs::path g_png_dir;
static std::string g_dump;  // Substring of a texture's location: write all of its levels.
static std::atomic<uint64_t> g_dump_seq{0};
static uint64_t g_per_format = 0;
static std::map<uint32_t, uint64_t> g_taken;

static void Issue(const std::string& s) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_issues.size() < 40) {
    g_issues.push_back(s);
  }
}

static void TestTexture(const std::string& where, const uint8_t* header, const uint8_t* data,
                        const uint8_t* data_end) {
  uint32_t words[6];
  for (int i = 0; i < 6; ++i) {
    words[i] = Be32(header + 28 + i * 4);
  }
  uint32_t format = words[1] & 0x3F;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats[format].seen;
    if (g_per_format && ++g_taken[format] > g_per_format) {
      return;
    }
  }
  // Offsets on disk start at 0, which would read as "no base". Rebase by one page, which is
  // what the game's loader does with the bundle's real address.
  bool has_mip_offset = (words[5] >> 12) != 0;
  words[1] += 1 << 12;
  if (has_mip_offset) {
    words[5] += 1 << 12;
  }
  TextureDesc d;
  Stats s;
  if (!ParseTextureFetch(words, d)) {
    Issue(where + ": not a texture constant");
    return;
  }
  if (!d.supported) {
    s.unsupported = 1;
    std::lock_guard<std::mutex> lock(g_mutex);
    ++g_stats[format].unsupported;
    return;
  }
  const uint8_t* base = d.base_address ? data + d.base_address - 0x1000 : nullptr;
  const uint8_t* mips = d.mip_address ? data + d.mip_address - 0x1000 : nullptr;
  size_t base_size = base && base < data_end ? size_t(data_end - base) : 0;
  size_t mip_size = mips && mips < data_end ? size_t(data_end - mips) : 0;
  LoadedTexture t;
  LoadTexture(d, base, base_size, mips, mip_size, t);
  s.decoded = 1;
  s.levels = t.mips.size();
  s.bytes = t.data.size();
  if (t.truncated) {
    s.truncated = 1;
    Issue(where + ": truncated " + std::to_string(d.width) + "x" + std::to_string(d.height));
  }

#ifdef NG2_SDK_REFERENCE
  auto layout = tu::GetGuestTextureLayout(
      xe::DataDimension::k2DOrStacked, (words[0] >> 22) & 0x1FF, d.width, d.height, 1, d.tiled,
      xe::TextureFormat(format), d.packed_mips, d.first_level == 0, d.last_level);
  s.sdk_textures = 1;
  std::vector<uint8_t> ref;
  for (const LoadedMip& m : t.mips) {
    SdkLevel(d, m, layout, base, base_size, mips, mip_size, ref);
    ++s.sdk_levels;
    if (std::memcmp(ref.data(), t.data.data() + m.offset, m.size) != 0) {
      ++s.sdk_bad_levels;
      Issue(where + ": SDK mismatch level " + std::to_string(m.level));
    }
  }
#endif

  // Mip continuity. Skip levels under 8 texels on a side, where BC rounding dominates.
  uint32_t packed_level = 0;
  if (d.packed_mips) {
    uint32_t smallest = std::min(d.width, d.height);
    uint32_t log2_size = smallest <= 1 ? 0 : 32 - std::countl_zero(smallest - 1);
    packed_level = log2_size > 4 ? log2_size - 4 : 0;
  } else {
    packed_level = UINT32_MAX;
  }
  if (!g_dump.empty() && where.find(g_dump) != std::string::npos && !g_png_dir.empty()) {
    for (const LoadedMip& m : t.mips) {
      wchar_t name[64];
      swprintf(name, 64, L"dump_%llu_L%u_%ux%u.png", (unsigned long long)g_dump_seq.fetch_add(1),
               m.level, m.width, m.height);
      WritePng(g_png_dir / name, DecodeLevel(d, t, m), m.width, m.height);
    }
  }

  std::vector<Rgba> prev;
  for (size_t i = 0; i < t.mips.size(); ++i) {
    const LoadedMip& m = t.mips[i];
    if (m.width < 8 || m.height < 8) {
      break;
    }
    auto cur = DecodeLevel(d, t, m);
    if (i) {
      double r = 0;
      const LoadedMip& up = t.mips[i - 1];
      if (MipCorrelation(prev, up.width, up.height, cur, m.width, m.height, r)) {
        ++s.mip_pairs;
        bool tail = m.level >= packed_level;
        s.tail_pairs += tail;
        if (r >= 0.9) {
          ++s.mip_ge90;
        } else if (r >= 0.7) {
          ++s.mip_ge70;
        } else if (r >= 0.5) {
          ++s.mip_ge50;
        } else {
          ++s.mip_low;
          s.tail_low += tail;
          Issue(where + ": low mip correlation " + std::to_string(r) + " at level " +
                std::to_string(m.level) + " size " + std::to_string(d.width) + "x" +
                std::to_string(d.height) + (tail ? " (tail)" : ""));
        }
      } else {
        ++s.mip_flat;
      }
    }
    prev = std::move(cur);
  }

  if (!g_png_dir.empty() && !t.mips.empty() && d.width >= 64 && d.height >= 64 &&
      g_png_left.fetch_sub(1) > 0) {
    const LoadedMip& m = t.mips[0];
    wchar_t name[64];
    swprintf(name, 64, L"f%02u_%ux%u_%d.png", format, d.width, d.height, g_png_left.load());
    WritePng(g_png_dir / name, DecodeLevel(d, t, m), m.width, m.height);
    if (t.mips.size() > 1) {
      const LoadedMip& m1 = t.mips.back();
      if (m1.width >= 4 && m1.height >= 4) {
        wchar_t tail[64];
        swprintf(tail, 64, L"f%02u_%ux%u_%d_last.png", format, d.width, d.height,
                 g_png_left.load());
        WritePng(g_png_dir / tail, DecodeLevel(d, t, m1), m1.width, m1.height);
      }
    }
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  Stats& g = g_stats[format];
  g.decoded += s.decoded, g.truncated += s.truncated, g.levels += s.levels, g.bytes += s.bytes;
  g.sdk_textures += s.sdk_textures, g.sdk_levels += s.sdk_levels;
  g.sdk_bad_levels += s.sdk_bad_levels;
  g.mip_pairs += s.mip_pairs, g.mip_flat += s.mip_flat, g.mip_ge90 += s.mip_ge90;
  g.mip_ge70 += s.mip_ge70, g.mip_ge50 += s.mip_ge50, g.mip_low += s.mip_low;
  g.tail_pairs += s.tail_pairs, g.tail_low += s.tail_low;
}

static void TestFile(const fs::path& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  LARGE_INTEGER size;
  GetFileSizeEx(file, &size);
  HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  const uint8_t* m = map ? static_cast<const uint8_t*>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0))
                         : nullptr;
  if (m) {
    size_t n = size_t(size.QuadPart);
    for (size_t i = 0; i + 16 < n; ++i) {
      const void* hit = memchr(m + i, 'X', n - 16 - i);
      if (!hit) {
        break;
      }
      i = static_cast<const uint8_t*>(hit) - m;
      if (std::memcmp(m + i, "XPR2", 4) != 0) {
        continue;
      }
      uint32_t header_size = Be32(m + i + 4), data_size = Be32(m + i + 8), count = Be32(m + i + 12);
      if (!count || count > 100000 || size_t(i) + 16 + size_t(count) * 16 > n ||
          size_t(i) + 12 + header_size > n) {
        continue;
      }
      const uint8_t* b = m + i;
      const uint8_t* data = b + 12 + header_size;
      const uint8_t* data_end = std::min(data + data_size, m + n);
      for (uint32_t e = 0; e < count; ++e) {
        const uint8_t* entry = b + 16 + e * 16;
        if (std::memcmp(entry, "TX2D", 4) != 0) {
          continue;
        }
        uint32_t offset = Be32(entry + 4);
        const uint8_t* header = b + 12 + offset;
        if (header + 52 > m + n) {
          continue;
        }
        char where[256];
        snprintf(where, sizeof(where), "%s+%zx #%u", path.filename().string().c_str(), i, e);
        TestTexture(where, header, data, data_end);
      }
      i += 12 + size_t(header_size) + data_size - 1;
    }
    UnmapViewOfFile(m);
  }
  if (map) {
    CloseHandle(map);
  }
  CloseHandle(file);
}

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: native_tex_test <game dir> [--per-format N] [--png dir] [--png-count N]\n");
    return 2;
  }
  fs::path game = argv[1];
  for (int i = 2; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--per-format")) {
      g_per_format = strtoull(argv[i + 1], nullptr, 10);
    } else if (!strcmp(argv[i], "--png")) {
      g_png_dir = argv[i + 1];
      fs::create_directories(g_png_dir);
    } else if (!strcmp(argv[i], "--dump")) {
      g_dump = argv[i + 1];
    } else if (!strcmp(argv[i], "--png-count")) {
      g_png_left = atoi(argv[i + 1]);
    }
  }
  uint64_t tiling_bad = CheckTilingFormulas();

  std::vector<fs::path> files;
  for (auto& e : fs::directory_iterator(game)) {
    if (e.is_regular_file() && e.path().extension() == ".ng2") {
      files.push_back(e.path());
    }
  }
  if (fs::exists(game / "dlc")) {
    for (auto& e : fs::directory_iterator(game / "dlc")) {
      if (e.is_regular_file()) {
        files.push_back(e.path());
      }
    }
  }
  DWORD t0 = GetTickCount();
  std::atomic<size_t> next{0};
  std::vector<std::thread> workers;
  unsigned threads = g_per_format ? 1 : std::max(4u, std::thread::hardware_concurrency() / 2);
  for (unsigned i = 0; i < threads; ++i) {
    workers.emplace_back([&] {
      for (size_t f; (f = next.fetch_add(1)) < files.size();) {
        TestFile(files[f]);
      }
    });
  }
  for (auto& w : workers) {
    w.join();
  }

  printf("tiling formula mismatches against crunch formula: %llu\n",
         (unsigned long long)tiling_bad);
  printf("%-4s %7s %7s %5s %5s %8s %8s %7s | %6s %5s %5s %5s %4s %5s\n", "fmt", "seen", "decoded",
         "unsup", "trunc", "levels", "sdk_lvls", "sdk_bad", "mips", ">=.9", ">=.7", ">=.5", "low",
         "flat");
  Stats total;
  for (auto& [format, s] : g_stats) {
    printf("%-4u %7llu %7llu %5llu %5llu %8llu %8llu %7llu | %6llu %5llu %5llu %5llu %4llu %5llu\n",
           format, (unsigned long long)s.seen, (unsigned long long)s.decoded,
           (unsigned long long)s.unsupported, (unsigned long long)s.truncated,
           (unsigned long long)s.levels, (unsigned long long)s.sdk_levels,
           (unsigned long long)s.sdk_bad_levels, (unsigned long long)s.mip_pairs,
           (unsigned long long)s.mip_ge90, (unsigned long long)s.mip_ge70,
           (unsigned long long)s.mip_ge50, (unsigned long long)s.mip_low,
           (unsigned long long)s.mip_flat);
    total.seen += s.seen, total.decoded += s.decoded, total.truncated += s.truncated;
    total.sdk_bad_levels += s.sdk_bad_levels, total.sdk_levels += s.sdk_levels;
    total.tail_pairs += s.tail_pairs, total.tail_low += s.tail_low;
    total.bytes += s.bytes;
  }
  printf("total seen %llu decoded %llu truncated %llu sdk_levels %llu sdk_bad %llu; "
         "tail mip pairs %llu low %llu; %.1f GB out in %.1f s\n",
         (unsigned long long)total.seen, (unsigned long long)total.decoded,
         (unsigned long long)total.truncated, (unsigned long long)total.sdk_levels,
         (unsigned long long)total.sdk_bad_levels, (unsigned long long)total.tail_pairs,
         (unsigned long long)total.tail_low, double(total.bytes) / 1e9,
         double(GetTickCount() - t0) / 1000.0);
  for (auto& s : g_issues) {
    printf("  %s\n", s.c_str());
  }
  return 0;
}
