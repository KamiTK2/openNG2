// Native renderer shaders (M2). Xenos ucode from the game image becomes SPIR-V once, offline or at
// first use. This is the native contract: the renderer binds plain Vulkan descriptors, and nothing
// here needs PM4, EDRAM or the SDK's shared memory.
//
// Pipeline layout every translated shader expects (design: notes/native/design.md):
//   set 0  binding 0  storage buffer, uint[]: vertex data heap. A vfetch reads
//                     heap[(fetch word 0 >> 2) + index * stride + offset], so the renderer copies
//                     stream bytes in guest byte order and rewrites word 0 of each vertex fetch
//                     constant to its heap dword offset << 2. Endian swap happens in the shader
//                     from bits 0:1 of fetch word 1.
//          binding 1  storage buffer, vec4[]: clip positions kept for motion vectors
//   set 1  binding 0  uniform: NativeSystemConstants
//          binding 1  uniform: vertex float constants, vec4[], from dev+1920, packed by float_bitmap
//          binding 2  uniform: pixel float constants, vec4[], from dev+6016, packed by float_bitmap
//          binding 3  uniform: bool + loop constants (256 bits, then 32 dwords in uvec4s)
//          binding 4  uniform: fetch constants, 32 x 6 dwords, from dev+1152
//   set 2  vertex textures, set 3 pixel textures: per shader, see NativeShader::textures/samplers.
//          Bindings 0..n-1 are sampled images, then samplers. 2D images are 2D_ARRAY views.
//   Vertex outputs and pixel inputs are always all 16 interpolators at locations 0-15 (unused ones
//   are zero), so any VS links with any PS with no per-pair variant.
//   Colour outputs are locations 0-3 per NativeShader::color_targets, depth is gl_FragDepth.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ng2::native {

enum class ShaderStage : uint32_t { kVertex = 0, kPixel = 1 };

constexpr uint32_t kShaderFlagNeedsVfetchPatch = 1u << 0;  // placeholder vfetch, see findings
constexpr uint32_t kShaderFlagKillsPixels = 1u << 1;
constexpr uint32_t kShaderFlagWritesDepth = 1u << 2;
constexpr uint32_t kShaderFlagRelativeRegisters = 1u << 3;
constexpr uint32_t kShaderFlagWmv = 1u << 4;

// Set 1 binding 0, valid std140. Flag bits are SpirvShaderTranslator::kSysFlag_*.
struct NativeSystemConstants {
  uint32_t flags;
  uint32_t vertex_index_endian;  // only with kSysFlag_VertexIndexLoad; leave 0
  int32_t vertex_base_index;
  uint32_t line_loop_closing_index;  // UINT32_MAX: NG2 draws no line loops
  uint32_t vertex_index_min;
  uint32_t vertex_index_max;
  float alpha_test_reference;
  uint32_t alpha_to_mask;  // 0: alpha to coverage off
  float ndc_scale[3];
  float point_vertex_diameter_min;
  float ndc_offset[3];
  float point_vertex_diameter_max;
  float color_exp_bias[4];
  uint32_t texture_swizzled_signs[8];
  float point_constant_diameter[2];
  float point_screen_diameter_to_ndc_radius[2];
  uint32_t reserved[50];  // bound but never read
  // Motion vector pipelines only. Offsets are fixed by the translator's edram_stencil_front.
  uint32_t history_bases[2];  // this frame, last frame: vec4 index of vertex 0, UINT32_MAX for none
  float motion_jitter_delta[2];  // shaders/motion.frag
  float motion_debug;
  uint32_t reserved2[41];
};

struct NativeTextureBinding {
  uint32_t fetch_constant;
  uint32_t dimension;  // xenos::FetchOpDimension
  uint32_t is_signed;
};

struct NativeSamplerBinding {
  uint32_t fetch_constant;
  uint32_t mag_filter, min_filter, mip_filter;  // xenos::TextureFilter
  uint32_t aniso_filter;                        // xenos::AnisoFilter
};

struct NativeShader {
  uint64_t ucode_hash = 0;  // XXH3 of the ucode in guest byte order, as the SDK cache keys it
  ShaderStage stage = ShaderStage::kVertex;
  uint32_t guest_address = 0;
  uint32_t ucode_dwords = 0;
  uint32_t flags = 0;
  uint32_t color_targets = 0;  // bit per colour output written
  uint32_t float_constant_count = 0;
  // Constants are tightly packed: the shader's vec4 n is the n-th set bit here (all ones when the
  // shader addresses them dynamically). See PackFloatConstants in the renderer.
  uint64_t float_bitmap[4] = {};
  std::vector<uint32_t> vertex_fetch_slots;  // fetch constant indices read, 0-95
  std::vector<NativeTextureBinding> textures;
  std::vector<NativeSamplerBinding> samplers;
  std::vector<uint32_t> spirv;
};

// Precompiled pack written by tools/native-shader-tool. Game-derived: generated on the player's
// machine from their own image, never shipped or committed.
class ShaderPack {
 public:
  bool Load(const std::filesystem::path& path, std::string* error = nullptr);
  bool Save(const std::filesystem::path& path) const;
  void Add(NativeShader shader);
  const NativeShader* Find(uint64_t ucode_hash) const;
  size_t size() const { return shaders_.size(); }
  const std::vector<NativeShader>& shaders() const { return shaders_; }

 private:
  std::vector<NativeShader> shaders_;
  std::unordered_map<uint64_t, size_t> by_hash_;
};

// Translation is only compiled where the SDK translator sources are linked (the tool, and later
// ng2 for bind-time patched vertex shaders).
#ifdef NG2_NATIVE_WITH_TRANSLATOR
// ucode is guest byte order. Returns false and fills error if translation failed.
bool TranslateShader(ShaderStage stage, const uint32_t* ucode_guest_order, uint32_t ucode_dwords,
                     uint64_t ucode_hash, NativeShader& out, std::string* error = nullptr);

// One shader container found in the game image.
struct ImageShader {
  ShaderStage stage;
  uint32_t ucode_address;
  uint32_t ucode_dwords;
  uint64_t ucode_hash;
  bool wmv;
};
// Finds every unique shader in a guest image (big endian, as loaded at `base`).
std::vector<ImageShader> ScanImageShaders(const uint8_t* image, size_t size, uint32_t base);
#endif

}  // namespace ng2::native
