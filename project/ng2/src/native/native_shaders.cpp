#include "native_shaders.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#ifdef NG2_NATIVE_WITH_TRANSLATOR
#include <rex/graphics/pipeline/shader/spirv.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/string/buffer.h>
#endif

namespace ng2::native {
namespace {

constexpr uint32_t kPackMagic = 0x4E32474E;  // 'NG2N'
constexpr uint32_t kPackVersion = 2;

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const uint8_t*>(&v);
  out.insert(out.end(), p, p + 4);
}
void PutU64(std::vector<uint8_t>& out, uint64_t v) {
  PutU32(out, uint32_t(v));
  PutU32(out, uint32_t(v >> 32));
}
template <typename T>
void PutArray(std::vector<uint8_t>& out, const std::vector<T>& v) {
  PutU32(out, uint32_t(v.size()));
  const auto* p = reinterpret_cast<const uint8_t*>(v.data());
  out.insert(out.end(), p, p + v.size() * sizeof(T));
}

struct Reader {
  const uint8_t* p;
  const uint8_t* end;
  bool ok = true;
  uint32_t U32() {
    if (end - p < 4) {
      ok = false;
      return 0;
    }
    uint32_t v;
    std::memcpy(&v, p, 4);
    p += 4;
    return v;
  }
  uint64_t U64() {
    const uint64_t lo = U32();
    return lo | (uint64_t(U32()) << 32);
  }
  template <typename T>
  void Array(std::vector<T>& v) {
    const uint32_t n = U32();
    if (!ok || size_t(end - p) / sizeof(T) < n) {
      ok = false;
      return;
    }
    v.resize(n);
    if (n) std::memcpy(v.data(), p, n * sizeof(T));
    p += n * sizeof(T);
  }
};

}  // namespace

void ShaderPack::Add(NativeShader shader) {
  const uint64_t hash = shader.ucode_hash;
  const auto found = by_hash_.find(hash);
  if (found != by_hash_.end()) {
    shaders_[found->second] = std::move(shader);
    return;
  }
  by_hash_[hash] = shaders_.size();
  shaders_.push_back(std::move(shader));
}

const NativeShader* ShaderPack::Find(uint64_t ucode_hash) const {
  const auto found = by_hash_.find(ucode_hash);
  return found == by_hash_.end() ? nullptr : &shaders_[found->second];
}

bool ShaderPack::Save(const std::filesystem::path& path) const {
  std::vector<uint8_t> out;
  PutU32(out, kPackMagic);
  PutU32(out, kPackVersion);
  PutU32(out, uint32_t(shaders_.size()));
  for (const NativeShader& s : shaders_) {
    PutU64(out, s.ucode_hash);
    PutU32(out, uint32_t(s.stage));
    PutU32(out, s.guest_address);
    PutU32(out, s.ucode_dwords);
    PutU32(out, s.flags);
    PutU32(out, s.color_targets);
    PutU32(out, s.float_constant_count);
    for (uint64_t bits : s.float_bitmap) PutU64(out, bits);
    PutArray(out, s.vertex_fetch_slots);
    PutArray(out, s.textures);
    PutArray(out, s.samplers);
    PutArray(out, s.spirv);
  }
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(file);
}

bool ShaderPack::Load(const std::filesystem::path& path, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
  Reader r{data.data(), data.data() + data.size()};
  const uint32_t magic = r.U32(), version = r.U32(), count = r.U32();
  if (!r.ok || magic != kPackMagic || version != kPackVersion) {
    if (error) *error = "not a native shader pack, or an older version";
    return false;
  }
  shaders_.clear();
  by_hash_.clear();
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    NativeShader s;
    s.ucode_hash = r.U64();
    s.stage = ShaderStage(r.U32());
    s.guest_address = r.U32();
    s.ucode_dwords = r.U32();
    s.flags = r.U32();
    s.color_targets = r.U32();
    s.float_constant_count = r.U32();
    for (uint64_t& bits : s.float_bitmap) bits = r.U64();
    r.Array(s.vertex_fetch_slots);
    r.Array(s.textures);
    r.Array(s.samplers);
    r.Array(s.spirv);
    if (r.ok) Add(std::move(s));
  }
  if (!r.ok && error) *error = "truncated native shader pack";
  return r.ok;
}

#ifdef NG2_NATIVE_WITH_TRANSLATOR
namespace {
namespace rg = rex::graphics;

// The translator's system constants block has an emulator layout that is not valid std140
// (user_clip_planes at offset 28) and is mostly EDRAM fields the host render target path never
// reads. Rewriting the member offsets gives the block NativeSystemConstants' layout without
// touching the SDK; unused members land in its reserved tail.
struct MemberOffset {
  const char* name;
  uint32_t offset;
};
constexpr MemberOffset kSystemConstantOffsets[] = {
    {"flags", 0},
    {"vertex_index_endian", 4},
    {"vertex_base_index", 8},
    {"line_loop_closing_index", 12},
    {"vertex_index_min", 16},
    {"vertex_index_max", 20},
    {"alpha_test_reference", 24},
    {"alpha_to_mask", 28},
    {"ndc_scale", 32},
    {"point_vertex_diameter_min", 44},
    {"ndc_offset", 48},
    {"point_vertex_diameter_max", 60},
    {"color_exp_bias", 64},
    {"texture_swizzled_signs", 80},
    {"point_constant_diameter", 112},
    {"point_screen_diameter_to_ndc_radius", 120},
    // Reserved tail, never read by host render target shaders (audited, see findings).
    {"vertex_index_load_address", 128},
    {"vertex_index_reset", 132},
    {"compute_memexport_vertex_count", 136},
    {"textures_resolution_scaled", 140},
    {"user_clip_planes", 144},
    {"texture_swizzles", 240},
    {"edram_32bpp_tile_pitch_dwords_scaled", 304},
    {"edram_depth_base_dwords_scaled", 308},
    {"edram_poly_offset_front_scale", 312},
    {"edram_poly_offset_back_scale", 316},
    {"edram_poly_offset_front_offset", 320},
    {"edram_poly_offset_back_offset", 324},
    {"edram_stencil_front", 328},
    {"edram_stencil_back", 336},
    {"edram_rt_base_dwords_scaled", 352},
    {"edram_rt_format_flags", 368},
    {"edram_rt_blend_factors_ops", 384},
    {"edram_rt_keep_mask", 400},
    {"edram_rt_clamp", 432},
    {"edram_blend_constant", 496},
};
static_assert(sizeof(NativeSystemConstants) == 512);

std::string SpirvString(const std::vector<uint32_t>& spirv, size_t word) {
  return reinterpret_cast<const char*>(spirv.data() + word);
}

// Returns false if the block is not found or holds a member this table does not know.
bool RewriteSystemConstantLayout(std::vector<uint32_t>& spirv) {
  constexpr uint32_t kOpName = 5, kOpMemberName = 6, kOpMemberDecorate = 72, kDecorationOffset = 35;
  uint32_t block = 0;
  for (size_t i = 5; i < spirv.size(); i += spirv[i] >> 16) {
    if ((spirv[i] & 0xFFFF) == kOpName && SpirvString(spirv, i + 2) == "XeSystemConstants") {
      block = spirv[i + 1];
      break;
    }
  }
  if (!block) return false;
  std::unordered_map<uint32_t, uint32_t> offsets;
  for (size_t i = 5; i < spirv.size(); i += spirv[i] >> 16) {
    if ((spirv[i] & 0xFFFF) != kOpMemberName || spirv[i + 1] != block) continue;
    const std::string name = SpirvString(spirv, i + 3);
    bool known = false;
    for (const MemberOffset& m : kSystemConstantOffsets) {
      if (name == m.name) {
        offsets[spirv[i + 2]] = m.offset;
        known = true;
      }
    }
    if (!known) return false;
  }
  for (size_t i = 5; i < spirv.size(); i += spirv[i] >> 16) {
    if ((spirv[i] & 0xFFFF) == kOpMemberDecorate && spirv[i + 1] == block &&
        spirv[i + 3] == kDecorationOffset) {
      const auto found = offsets.find(spirv[i + 2]);
      if (found == offsets.end()) return false;
      spirv[i + 4] = found->second;
    }
  }
  return true;
}

rg::SpirvShaderTranslator& Translator() {
  // Host render target mode: fragment outputs, no interlock, no EDRAM access. Features(true) is
  // the desktop baseline, SPIR-V 1.5 with float controls. Denorm flushing is off: drivers don't
  // all offer it (the RTX 4090's doesn't), and declaring it is then invalid SPIR-V for the device.
  static rg::SpirvShaderTranslator::Features features = [] {
    rg::SpirvShaderTranslator::Features f(true);
    f.denorm_flush_to_zero_float32 = false;
    return f;
  }();
  static rg::SpirvShaderTranslator translator(features, true, true, false);
  return translator;
}
}  // namespace

bool TranslateShader(ShaderStage stage, const uint32_t* ucode_guest_order, uint32_t ucode_dwords,
                     uint64_t ucode_hash, NativeShader& out, std::string* error) {
  rg::SpirvShader shader(
      stage == ShaderStage::kVertex ? rg::xenos::ShaderType::kVertex : rg::xenos::ShaderType::kPixel,
      ucode_hash, ucode_guest_order, ucode_dwords, std::endian::big);
  rex::string::StringBuffer disasm;
  shader.AnalyzeUcode(disasm);
  if (!shader.is_ucode_analyzed()) {
    if (error) *error = "ucode analysis failed";
    return false;
  }

  // 16 interpolators on both sides: locations then equal interpolator indices and pairs always
  // link. 63 is the SQ_PROGRAM_CNTL maximum; only shaders with relative r# addressing use it.
  const uint32_t registers = shader.GetDynamicAddressableRegisterCount(63);
  rg::SpirvShaderTranslator::Modification mod;
  if (stage == ShaderStage::kVertex) {
    mod.value = Translator().GetDefaultVertexShaderModification(registers);
    mod.vertex.interpolator_mask = 0xFFFF;
  } else {
    mod.value = Translator().GetDefaultPixelShaderModification(registers);
    mod.pixel.interpolator_mask = 0xFFFF;
  }
  rg::Shader::Translation* translation = shader.GetOrCreateTranslation(mod.value);
  if (!Translator().TranslateAnalyzedShader(*translation) || !translation->is_valid()) {
    if (error) {
      *error = "translation failed:";
      for (const auto& e : translation->errors()) *error += " " + e.message;
    }
    return false;
  }
  const std::vector<uint8_t>& binary = translation->translated_binary();
  if (binary.empty() || binary.size() % 4) {
    if (error) *error = "translator returned no SPIR-V";
    return false;
  }

  out.ucode_hash = ucode_hash;
  out.stage = stage;
  out.ucode_dwords = ucode_dwords;
  out.flags = 0;
  out.color_targets = shader.writes_color_targets();
  out.float_constant_count = shader.constant_register_map().float_count;
  for (int i = 0; i < 4; ++i) out.float_bitmap[i] = shader.constant_register_map().float_bitmap[i];
  if (shader.kills_pixels()) out.flags |= kShaderFlagKillsPixels;
  if (shader.writes_depth()) out.flags |= kShaderFlagWritesDepth;
  if (shader.uses_register_dynamic_addressing()) out.flags |= kShaderFlagRelativeRegisters;

  out.vertex_fetch_slots.clear();
  for (const rg::Shader::VertexBinding& binding : shader.vertex_bindings()) {
    out.vertex_fetch_slots.push_back(binding.fetch_constant);
    for (const auto& attribute : binding.attributes) {
      if (uint32_t(attribute.fetch_instr.attributes.data_format) == 0) {
        out.flags |= kShaderFlagNeedsVfetchPatch;
      }
    }
  }
  out.textures.clear();
  for (const auto& b : shader.GetTextureBindingsAfterTranslation()) {
    out.textures.push_back(
        {uint32_t(b.fetch_constant), uint32_t(b.dimension), uint32_t(b.is_signed)});
  }
  out.samplers.clear();
  for (const auto& b : shader.GetSamplerBindingsAfterTranslation()) {
    out.samplers.push_back({uint32_t(b.fetch_constant), uint32_t(b.mag_filter),
                            uint32_t(b.min_filter), uint32_t(b.mip_filter),
                            uint32_t(b.aniso_filter)});
  }
  out.spirv.resize(binary.size() / 4);
  std::memcpy(out.spirv.data(), binary.data(), binary.size());
  if (!RewriteSystemConstantLayout(out.spirv)) {
    if (error) *error = "system constants layout rewrite failed";
    return false;
  }
  return true;
}
#endif  // NG2_NATIVE_WITH_TRANSLATOR

}  // namespace ng2::native
