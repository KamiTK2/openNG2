// See native_textures.h. The addressing rules are the Xenos hardware's (32x32 block tiles, mips
// padded to power-of-two sizes, a packed tail for small levels), as documented in the SDK's
// texture utilities; this file depends on nothing from the SDK.
#include "native_textures.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace ng2::native {
namespace {

constexpr uint32_t kFormat8 = 2;
constexpr uint32_t kFormat8888 = 6;
constexpr uint32_t kFormatDxt1 = 18;
constexpr uint32_t kFormatDxt23 = 19;
constexpr uint32_t kFormatDxt45 = 20;
constexpr uint32_t kFormat2101010As16 = 54;

constexpr uint32_t kFetchTypeTexture = 2;
constexpr uint32_t kDimension2D = 1;

constexpr uint32_t kTileSize = 32;
constexpr uint32_t kLinearRowAlign = 256;
constexpr uint32_t kSubresourceAlign = 4096;

uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
uint32_t Log2Floor(uint32_t v) { return v ? 31 - uint32_t(std::countl_zero(v)) : 0; }
uint32_t NextPow2(uint32_t v) { return uint32_t(1) << Log2Ceil(v); }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
uint32_t LevelSize(uint32_t size, uint32_t level) { return std::max(size >> level, uint32_t(1)); }

uint32_t BlocksOf(uint32_t texels, uint32_t block_size) {
  return AlignUp(texels, block_size) / block_size;
}

// First level stored in the packed tail, for textures that have one.
uint32_t PackedLevel(uint32_t width, uint32_t height) {
  uint32_t log2_size = Log2Ceil(std::min(width, height));
  return log2_size > 4 ? log2_size - 4 : 0;
}

// Position of a level inside the 32x32-block tail, in blocks. False if the level isn't in it.
bool PackedOffset(const TextureDesc& d, uint32_t level, uint32_t& x_blocks, uint32_t& y_blocks) {
  uint32_t log2_width = Log2Ceil(d.width);
  uint32_t log2_height = Log2Ceil(d.height);
  uint32_t log2_size = std::min(log2_width, log2_height);
  x_blocks = y_blocks = 0;
  if (log2_size > 4 + level) {
    return false;
  }
  uint32_t tail_base = log2_size > 4 ? log2_size - 4 : 0;
  uint32_t sublevel = level - tail_base;
  if (sublevel < 3) {
    if (log2_width > log2_height) {
      y_blocks = 16 >> sublevel;
    } else {
      x_blocks = 16 >> sublevel;
    }
  } else if (log2_width > log2_height) {
    x_blocks = (uint32_t(1) << (log2_width - tail_base)) >> (sublevel - 2);
  } else {
    y_blocks = (uint32_t(1) << (log2_height - tail_base)) >> (sublevel - 2);
  }
  x_blocks /= d.block_width;
  y_blocks /= d.block_height;
  return true;
}

// Where one stored level lives.
struct Placement {
  bool in_base = false;
  uint32_t offset = 0;        // From the base or mip address.
  uint32_t pitch_blocks = 0;  // Tile aligned.
  uint32_t row_bytes = 0;     // Linear stride.
  uint32_t x_blocks = 0, y_blocks = 0;
  uint32_t data_end = 0;  // Readable extent from the start of the base or mip data.
};

uint32_t TiledUpperBound(uint32_t right, uint32_t bottom, uint32_t pitch, uint32_t log2_bytes) {
  if (!right || !bottom) {
    return 0;
  }
  uint32_t bound = TiledBlockOffset((right - 1) & ~(kTileSize - 1), (bottom - 1) & ~(kTileSize - 1),
                                    pitch, log2_bytes);
  switch (log2_bytes) {
    case 0:
      return bound + 0xA00;
    case 1:
      return bound + 0xC00;
    default:
      return bound + (uint32_t(0x400) << log2_bytes);
  }
}

// Row stride of a mip's stored level, which comes from the power-of-two padded size.
uint32_t MipPitchBlocks(const TextureDesc& d, uint32_t stored_level) {
  uint32_t texels = std::max(NextPow2(d.width) >> stored_level, uint32_t(1));
  return AlignUp(BlocksOf(texels, d.block_width), kTileSize);
}

uint32_t MipRowsBlocks(const TextureDesc& d, uint32_t stored_level) {
  uint32_t texels = std::max(NextPow2(d.height) >> stored_level, uint32_t(1));
  return AlignUp(BlocksOf(texels, d.block_height), kTileSize);
}

uint32_t LinearRowBytes(uint32_t pitch_blocks, uint32_t bytes_per_block, bool base) {
  uint32_t bytes = pitch_blocks * bytes_per_block;
  return base ? bytes : AlignUp(bytes, kLinearRowAlign);
}

Placement PlaceLevel(const TextureDesc& d, uint32_t level) {
  Placement p;
  uint32_t packed = d.packed_mips ? PackedLevel(d.width, d.height) : UINT32_MAX;
  p.in_base = level == 0;
  uint32_t stored = std::min(level, packed);
  if (p.in_base) {
    p.pitch_blocks = d.base_pitch_blocks;
  } else {
    // A tail at level 0 means the mips have their own tail at the start of the mip data.
    if (packed != 0) {
      for (uint32_t l = 1; l < stored; ++l) {
        uint32_t row_bytes = d.tiled
                                 ? MipPitchBlocks(d, l) * d.bytes_per_block
                                 : LinearRowBytes(MipPitchBlocks(d, l), d.bytes_per_block, false);
        p.offset += AlignUp(row_bytes * MipRowsBlocks(d, l), kSubresourceAlign);
      }
    } else {
      stored = 0;
    }
    p.pitch_blocks = MipPitchBlocks(d, stored);
  }
  p.row_bytes = d.tiled ? p.pitch_blocks * d.bytes_per_block
                        : LinearRowBytes(p.pitch_blocks, d.bytes_per_block, p.in_base);
  if (level >= packed) {
    PackedOffset(d, level, p.x_blocks, p.y_blocks);
  }
  uint32_t right = p.x_blocks + BlocksOf(LevelSize(d.width, level), d.block_width);
  uint32_t bottom = p.y_blocks + BlocksOf(LevelSize(d.height, level), d.block_height);
  uint32_t log2_bytes = Log2Floor(d.bytes_per_block);
  p.data_end = p.offset + (d.tiled ? TiledUpperBound(right, bottom, p.pitch_blocks, log2_bytes)
                                   : p.row_bytes * (bottom - 1) + right * d.bytes_per_block);
  return p;
}

void SwapBlock(uint32_t endian, uint8_t* dst, const uint8_t* src, uint32_t bytes) {
  switch (endian) {
    case 1:
      for (uint32_t i = 0; i < bytes; i += 2) {
        dst[i] = src[i + 1];
        dst[i + 1] = src[i];
      }
      break;
    case 2:
      for (uint32_t i = 0; i < bytes; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
      }
      break;
    case 3:
      for (uint32_t i = 0; i < bytes; i += 4) {
        dst[i] = src[i + 2];
        dst[i + 1] = src[i + 3];
        dst[i + 2] = src[i];
        dst[i + 3] = src[i + 1];
      }
      break;
    default:
      std::memcpy(dst, src, bytes);
      break;
  }
}

uint32_t MapSwizzle(uint32_t xenos_swizzle, uint32_t component) {
  switch (xenos_swizzle) {
    case 0:
      return kVkSwizzleR;
    case 1:
      return kVkSwizzleG;
    case 2:
      return kVkSwizzleB;
    case 3:
      return kVkSwizzleA;
    case 4:
      return kVkSwizzleZero;
    case 5:
      return kVkSwizzleOne;
    default:
      return kVkSwizzleR + component;  // Keep.
  }
}

}  // namespace

uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t log2_bytes) {
  uint32_t pitch = AlignUp(pitch_blocks, kTileSize);
  uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2_bytes + 7);
  uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  uint32_t offset = macro + ((micro & ~uint32_t(0xF)) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~uint32_t(0x1FF)) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         ((((y & 8) >> 2) + (x >> 3)) & 3) * 64 + (offset & 0x3F);
}

bool ParseTextureFetch(const uint32_t words[6], TextureDesc& d) {
  d = TextureDesc{};
  if ((words[0] & 3) != kFetchTypeTexture) {
    return false;
  }
  d.tiled = (words[0] >> 31) != 0;
  uint32_t pitch_texels = ((words[0] >> 22) & 0x1FF) << 5;
  d.xenos_format = words[1] & 0x3F;
  d.endian = (words[1] >> 6) & 3;
  bool stacked = ((words[1] >> 10) & 1) != 0;
  uint32_t base_page = (words[1] >> 12) & 0x1FFFF;
  d.width = (words[2] & 0x1FFF) + 1;
  d.height = ((words[2] >> 13) & 0x1FFF) + 1;
  uint32_t mip_min = (words[4] >> 2) & 0xF;
  uint32_t mip_max = (words[4] >> 6) & 0xF;
  uint32_t dimension = (words[5] >> 9) & 3;
  d.packed_mips = ((words[5] >> 11) & 1) != 0;
  uint32_t mip_page = (words[5] >> 12) & 0x1FFFF;

  switch (d.xenos_format) {
    case kFormat8:
      d.vk_format = kVkFormatR8Unorm;
      d.bytes_per_block = 1;
      break;
    case kFormat8888:
      d.vk_format = kVkFormatR8G8B8A8Unorm;
      d.bytes_per_block = 4;
      break;
    case kFormat2101010As16:
      d.vk_format = kVkFormatA2B10G10R10UnormPack32;
      d.bytes_per_block = 4;
      break;
    case kFormatDxt1:
      d.vk_format = kVkFormatBc1RgbaUnormBlock;
      d.bytes_per_block = 8;
      d.block_width = d.block_height = 4;
      break;
    case kFormatDxt23:
      d.vk_format = kVkFormatBc2UnormBlock;
      d.bytes_per_block = 16;
      d.block_width = d.block_height = 4;
      break;
    case kFormatDxt45:
      d.vk_format = kVkFormatBc3UnormBlock;
      d.bytes_per_block = 16;
      d.block_width = d.block_height = 4;
      break;
    default:
      return true;  // Valid constant, unsupported format.
  }
  if (dimension != kDimension2D || stacked) {
    return true;
  }

  uint32_t swizzle = (words[3] >> 1) & 0xFFF;
  for (uint32_t i = 0; i < 4; ++i) {
    d.vk_swizzle[i] = MapSwizzle((swizzle >> (i * 3)) & 7, i);
  }

  uint32_t size_max_level = Log2Floor(std::max(d.width, d.height));
  uint32_t first = 0, last = 0;
  if (mip_page != 0) {
    first = std::min(mip_min, size_max_level);
    last = std::max(std::min(mip_max, size_max_level), first);
  }
  if (last != 0) {
    if (base_page == 0) {
      first = std::max(first, uint32_t(1));
    }
    if (first != 0) {
      base_page = 0;
    }
  } else {
    mip_page = 0;
  }
  d.first_level = first;
  d.last_level = last;
  d.base_address = base_page << 12;
  d.mip_address = mip_page << 12;
  d.base_pitch_blocks = AlignUp(BlocksOf(pitch_texels, d.block_width), kTileSize);

  for (uint32_t level = first; level <= last; ++level) {
    Placement p = PlaceLevel(d, level);
    uint32_t& extent = p.in_base ? d.base_bytes : d.mip_bytes;
    extent = std::max(extent, p.data_end);
  }
  d.supported = true;
  return true;
}

bool LoadTexture(const TextureDesc& d, const uint8_t* base_data, size_t base_size,
                 const uint8_t* mip_data, size_t mip_size, LoadedTexture& out) {
  out = LoadedTexture{};
  if (!d.supported) {
    return false;
  }
  size_t total = 0;
  for (uint32_t level = d.first_level; level <= d.last_level; ++level) {
    LoadedMip m;
    m.level = level;
    m.width = LevelSize(d.width, level);
    m.height = LevelSize(d.height, level);
    m.row_bytes = BlocksOf(m.width, d.block_width) * d.bytes_per_block;
    m.rows = BlocksOf(m.height, d.block_height);
    m.offset = total;
    m.size = size_t(m.row_bytes) * m.rows;
    total += m.size;
    out.mips.push_back(m);
  }
  out.data.assign(total, 0);

  const uint32_t bytes = d.bytes_per_block;
  const uint32_t log2_bytes = Log2Floor(bytes);
  for (const LoadedMip& m : out.mips) {
    Placement p = PlaceLevel(d, m.level);
    const uint8_t* src = p.in_base ? base_data : mip_data;
    size_t src_size = p.in_base ? base_size : mip_size;
    if (!src) {
      out.truncated = true;
      continue;
    }
    uint32_t blocks_wide = m.row_bytes / bytes;
    for (uint32_t y = 0; y < m.rows; ++y) {
      uint8_t* dst = out.data.data() + m.offset + size_t(y) * m.row_bytes;
      for (uint32_t x = 0; x < blocks_wide; ++x, dst += bytes) {
        uint32_t sx = x + p.x_blocks, sy = y + p.y_blocks;
        size_t at = p.offset;
        at += d.tiled ? TiledBlockOffset(sx, sy, p.pitch_blocks, log2_bytes)
                      : size_t(p.row_bytes) * sy + size_t(sx) * bytes;
        if (at + bytes > src_size) {
          out.truncated = true;
          continue;
        }
        SwapBlock(d.endian, dst, src + at, bytes);
      }
    }
  }
  return true;
}

}  // namespace ng2::native
