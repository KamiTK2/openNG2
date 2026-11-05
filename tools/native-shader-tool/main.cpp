// Converts every shader in the game image to native-interface SPIR-V (notes/native/design.md, M2).
// Usage: native_shader_tool <image.bin> <image base hex> <out dir> [cache.xsh]
// Output is game-derived and stays on this machine: <out>/spv/*.spv, <out>/report.csv,
// <out>/ng2_native_shaders.pack.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include "native/native_shaders.h"

namespace fs = std::filesystem;
using namespace ng2::native;

namespace {
constexpr uint32_t kRegionStart = 0x8200AC98;

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
std::vector<uint8_t> ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool LooksLikeWmv(const uint8_t* header, size_t size) {
  static const char kNeedle[] = "wmv";
  return std::search(header, header + size, kNeedle, kNeedle + 3) != header + size;
}

struct CachedShader {
  uint64_t hash;
  ShaderStage stage;
  std::vector<uint32_t> ucode;
};

// The SDK shader cache holds what the GPU really received, so it includes bind-time patched
// vertex shaders that are not in the image as such.
std::vector<CachedShader> ReadCache(const fs::path& path) {
  std::vector<CachedShader> shaders;
  const std::vector<uint8_t> xsh = ReadFile(path);
  for (size_t off = 8; off + 12 <= xsh.size();) {
    uint64_t hash;
    uint32_t cw;
    std::memcpy(&hash, xsh.data() + off, 8);
    std::memcpy(&cw, xsh.data() + off + 8, 4);
    const size_t dwords = cw & 0x7FFFFFFF;
    off += 12;
    if (off + dwords * 4 > xsh.size()) break;
    CachedShader& s = shaders.emplace_back();
    s.hash = hash;
    s.stage = (cw >> 31) ? ShaderStage::kPixel : ShaderStage::kVertex;
    s.ucode.resize(dwords);
    std::memcpy(s.ucode.data(), xsh.data() + off, dwords * 4);
    off += dwords * 4;
  }
  return shaders;
}
}  // namespace

int RunVulkanCheck(const char* pack_path);

int main(int argc, char** argv) {
  if (argc == 3 && std::strcmp(argv[1], "--vk-check") == 0) return RunVulkanCheck(argv[2]);
  if (argc < 4) {
    std::puts("usage: native_shader_tool <image.bin> <base hex> <out dir> [cache.xsh]");
    return 2;
  }
  const std::vector<uint8_t> image = ReadFile(argv[1]);
  const uint32_t base = uint32_t(std::strtoul(argv[2], nullptr, 16));
  const fs::path out_dir = argv[3];
  fs::create_directories(out_dir / "spv");
  std::vector<CachedShader> cache;
  if (argc > 4) cache = ReadCache(argv[4]);
  std::unordered_set<uint64_t> cache_hashes;
  for (const CachedShader& c : cache) cache_hashes.insert(c.hash);

  std::ofstream report(out_dir / "report.csv");
  report << "address,hash,stage,dwords,status,class,flags,textures,samplers,slots,in_cache,detail\n";
  ShaderPack pack;
  std::unordered_set<uint64_t> seen;
  std::map<std::string, int> counts;
  int containers = 0, cache_hits = 0;

  const auto record = [&](ShaderStage stage, const uint32_t* ucode, uint32_t dwords, uint64_t hash,
                          uint32_t address, const char* cls, bool in_cache) {
    NativeShader shader;
    std::string error;
    const bool ok = TranslateShader(stage, ucode, dwords, hash, shader, &error);
    shader.guest_address = address;
    if (std::strcmp(cls, "wmv") == 0) shader.flags |= kShaderFlagWmv;
    const char* stage_name = stage == ShaderStage::kVertex ? "vs" : "ps";
    ++counts[std::string(cls) + "_" + stage_name + (ok ? "_ok" : "_fail")];
    if (ok && (shader.flags & kShaderFlagNeedsVfetchPatch)) {
      ++counts[std::string(cls) + "_needs_patch"];
    }
    for (char& c : error) {
      if (c == ',' || c < ' ') c = ' ';
    }
    char line[256];
    std::snprintf(line, sizeof(line), "%08X,%016llX,%s,%u,%s,%s,%X,%zu,%zu,%zu,%d,", address,
                  (unsigned long long)hash, stage_name, dwords, ok ? "ok" : "fail", cls,
                  shader.flags, shader.textures.size(), shader.samplers.size(),
                  shader.vertex_fetch_slots.size(), int(in_cache));
    report << line << error << '\n';
    if (!ok) return;
    char name[64];
    std::snprintf(name, sizeof(name), "%016llX.%s.spv", (unsigned long long)hash, stage_name);
    std::ofstream spv(out_dir / "spv" / name, std::ios::binary | std::ios::trunc);
    spv.write(reinterpret_cast<const char*>(shader.spirv.data()),
              std::streamsize(shader.spirv.size() * 4));
    pack.Add(std::move(shader));
  };

  for (uint32_t addr = kRegionStart;; addr += 4) {
    const uint32_t off = addr - base;
    if (off + 12 > image.size()) break;
    const uint32_t magic = Be32(&image[off]);
    if (magic != 0x102A1100 && magic != 0x102A1101) continue;
    const uint32_t a = Be32(&image[off + 4]), b = Be32(&image[off + 8]);
    if (!b || b % 4 || off + a + b > image.size()) continue;
    ++containers;
    const ShaderStage stage = magic == 0x102A1101 ? ShaderStage::kVertex : ShaderStage::kPixel;
    // Non-zero field 5 means a 0x40-byte block precedes the ucode (checked against the cache hash).
    const uint32_t lead = Be32(&image[off + 20]) ? 0x40 : 0;
    if (b <= lead) {
      ++counts["bad_layout"];
      continue;
    }
    const uint32_t ucode_addr = addr + a + lead;
    const uint32_t dwords = (b - lead) / 4;
    std::vector<uint32_t> ucode(dwords);
    std::memcpy(ucode.data(), &image[off + a + lead], size_t(dwords) * 4);
    const uint64_t hash = XXH3_64bits(ucode.data(), size_t(dwords) * 4);
    if (!seen.insert(hash).second) continue;
    const bool in_cache = cache_hashes.count(hash) != 0;
    cache_hits += in_cache;
    const bool wmv = LooksLikeWmv(&image[off], a);

    record(stage, ucode.data(), dwords, hash, ucode_addr, wmv ? "wmv" : "game", in_cache);
  }

  // Shaders the GPU received that the image does not hold unchanged (bind-time patched VS).
  for (const CachedShader& c : cache) {
    if (!seen.insert(c.hash).second) continue;
    record(c.stage, c.ucode.data(), uint32_t(c.ucode.size()), c.hash, 0, "cache", true);
  }
  pack.Save(out_dir / "ng2_native_shaders.pack");

  std::printf("containers %d, unique %zu, in cache %d of %zu\n", containers, seen.size(),
              cache_hits, cache.size());
  for (const auto& [name, count] : counts) std::printf("%-20s %d\n", name.c_str(), count);
  return 0;
}
