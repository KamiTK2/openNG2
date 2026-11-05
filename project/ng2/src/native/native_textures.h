// Native renderer: Xenos texture loading with no dependency on the emulated texture cache.
//
// Input is a six-dword texture fetch constant (host-endian words) and pointers to the guest data.
// Output is untiled, byte-swapped data per mip level, tightly packed, ready for
// vkCmdCopyBufferToImage, plus the VkFormat and component mapping to create the image with.
//
// Typical use:
//   ng2::native::TextureDesc desc;
//   if (ng2::native::ParseTextureFetch(words, desc) && desc.supported) {
//     // Translate desc.base_address / desc.mip_address (guest physical) to host pointers.
//     // desc.base_bytes / desc.mip_bytes are how many bytes each one must have readable.
//     ng2::native::LoadedTexture out;
//     ng2::native::LoadTexture(desc, base_ptr, desc.base_bytes, mip_ptr, desc.mip_bytes, out);
//   }
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ng2::native {

// Numeric values of VkFormat, so this header doesn't need the Vulkan headers.
constexpr uint32_t kVkFormatUndefined = 0;
constexpr uint32_t kVkFormatR8Unorm = 9;
constexpr uint32_t kVkFormatR8G8B8A8Unorm = 37;
constexpr uint32_t kVkFormatA2B10G10R10UnormPack32 = 64;
constexpr uint32_t kVkFormatBc1RgbaUnormBlock = 133;
constexpr uint32_t kVkFormatBc2UnormBlock = 135;
constexpr uint32_t kVkFormatBc3UnormBlock = 137;

// Numeric values of VkComponentSwizzle.
constexpr uint32_t kVkSwizzleIdentity = 0;
constexpr uint32_t kVkSwizzleZero = 1;
constexpr uint32_t kVkSwizzleOne = 2;
constexpr uint32_t kVkSwizzleR = 3;
constexpr uint32_t kVkSwizzleG = 4;
constexpr uint32_t kVkSwizzleB = 5;
constexpr uint32_t kVkSwizzleA = 6;

struct TextureDesc {
  uint32_t width = 0, height = 0;  // Texels.
  uint32_t xenos_format = 0;       // xenos::TextureFormat value.
  uint32_t endian = 0;             // 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
  bool tiled = false;
  bool packed_mips = false;
  // False for a format outside the six static formats, or a non-2D / stacked texture.
  bool supported = false;

  uint32_t vk_format = kVkFormatUndefined;
  uint32_t vk_swizzle[4] = {kVkSwizzleIdentity, kVkSwizzleIdentity, kVkSwizzleIdentity,
                            kVkSwizzleIdentity};  // VkComponentMapping r, g, b, a.

  uint32_t bytes_per_block = 0;
  uint32_t block_width = 1, block_height = 1;

  // Guest physical addresses; 0 when that part doesn't exist. The fetch constant's min level can
  // exclude the base, in which case first_level is above 0.
  uint32_t base_address = 0;
  uint32_t mip_address = 0;
  uint32_t first_level = 0, last_level = 0;
  uint32_t base_pitch_blocks = 0;  // Tile-aligned row pitch of the base level.

  // Bytes that have to be readable at each address.
  uint32_t base_bytes = 0;
  uint32_t mip_bytes = 0;
};

struct LoadedMip {
  uint32_t level = 0;
  uint32_t width = 0, height = 0;  // Texels; the size to give vkCmdCopyBufferToImage.
  uint32_t row_bytes = 0;          // Tightly packed block rows.
  uint32_t rows = 0;               // Block rows.
  size_t offset = 0;               // Into LoadedTexture::data.
  size_t size = 0;
};

struct LoadedTexture {
  std::vector<LoadedMip> mips;
  std::vector<uint8_t> data;
  bool truncated = false;  // Some blocks were past the supplied bytes and are left zero.
};

// Fills desc from six host-endian words. Returns false when the words aren't a texture constant.
bool ParseTextureFetch(const uint32_t words[6], TextureDesc& desc);

// Decodes every level from first_level to last_level. Either pointer may be null if that part
// doesn't exist; a level whose data is missing comes back zeroed with truncated set.
bool LoadTexture(const TextureDesc& desc, const uint8_t* base_data, size_t base_size,
                 const uint8_t* mip_data, size_t mip_size, LoadedTexture& out);

// Byte offset of a block in a tiled surface whose pitch is in blocks. Exposed for tests.
uint32_t TiledBlockOffset(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t log2_bytes);

}  // namespace ng2::native
