// Shader precompile (findings #58). NG2's shaders are compiled into its executable. openNG2 ships
// a manifest (shader hash, type, size, address in the executable) and the pipeline list (shader
// hashes + render state) recorded from a playthrough - no game code. Before the game starts, any
// shader missing from the player's shader cache is read from their own game image in memory,
// hash-checked and appended to the cache, and missing pipelines are appended to the pipeline
// storage. The SDK then translates every shader and builds every pipeline up front, while the
// settings overlay shows progress (stage / done / total from ReXApp::OnShaderStorageProgress).
#include "shader_precompile.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>

namespace ng2 {
namespace {

std::atomic<bool> g_active{false};
std::atomic<int> g_stage{0};
std::atomic<uint32_t> g_done{0};
std::atomic<uint32_t> g_total{0};

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

constexpr uint32_t kShaderStorageMagic = 0x48534558;  // 'XESH'
constexpr uint32_t kShaderStorageVersion = 0x20201219;

uint32_t ByteSwap32(uint32_t v) {
  return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

// Appends the manifest's shaders that the cache lacks. Returns how many were added.
int MergeShaders(const std::filesystem::path& manifest_path, const std::filesystem::path& xsh_path) {
  const std::vector<uint8_t> manifest = ReadFile(manifest_path);
  if (manifest.size() < 12 || std::memcmp(manifest.data(), "NG2S", 4) != 0) return 0;
  uint32_t version, count;
  std::memcpy(&version, manifest.data() + 4, 4);
  std::memcpy(&count, manifest.data() + 8, 4);
  if (version != 1 || manifest.size() < 12 + size_t(count) * 24) return 0;

  // Hashes already in the cache (entries: u64 hash, u32 count:31|type:1, ucode).
  std::unordered_set<uint64_t> have;
  std::vector<uint8_t> xsh = ReadFile(xsh_path);
  bool valid = xsh.size() >= 8;
  if (valid) {
    uint32_t magic, ver;
    std::memcpy(&magic, xsh.data(), 4);
    std::memcpy(&ver, xsh.data() + 4, 4);
    valid = magic == kShaderStorageMagic && ByteSwap32(ver) == kShaderStorageVersion;
  }
  size_t off = 8;
  while (valid && off + 12 <= xsh.size()) {
    uint64_t hash;
    uint32_t cw;
    std::memcpy(&hash, xsh.data() + off, 8);
    std::memcpy(&cw, xsh.data() + off + 8, 4);
    const size_t bytes = size_t(cw & 0x7FFFFFFF) * 4;
    if (off + 12 + bytes > xsh.size()) break;
    have.insert(hash);
    off += 12 + bytes;
  }

  std::vector<uint8_t> append;
  if (!valid) {
    const uint32_t header[2] = {kShaderStorageMagic, ByteSwap32(kShaderStorageVersion)};
    append.insert(append.end(), reinterpret_cast<const uint8_t*>(header),
                  reinterpret_cast<const uint8_t*>(header) + 8);
  }
  auto* mem = REX_KERNEL_STATE()->memory();
  int added = 0, mismatched = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = manifest.data() + 12 + size_t(i) * 24;
    uint64_t hash;
    uint32_t address, dwords, type, swap;
    std::memcpy(&hash, e, 8);
    std::memcpy(&address, e + 8, 4);
    std::memcpy(&dwords, e + 12, 4);
    std::memcpy(&type, e + 16, 4);
    std::memcpy(&swap, e + 20, 4);
    if (have.count(hash) || !dwords || dwords > 0x10000) continue;
    std::vector<uint32_t> ucode(dwords);
    std::memcpy(ucode.data(), mem->TranslateVirtual<uint8_t*>(address), size_t(dwords) * 4);
    if (swap) {
      for (auto& w : ucode) w = ByteSwap32(w);
    }
    if (XXH3_64bits(ucode.data(), size_t(dwords) * 4) != hash) {
      ++mismatched;  // not this game version: leave it on-demand
      continue;
    }
    const uint32_t cw = (dwords & 0x7FFFFFFF) | (type ? 0x80000000u : 0u);
    const uint8_t* h = reinterpret_cast<const uint8_t*>(&hash);
    append.insert(append.end(), h, h + 8);
    append.insert(append.end(), reinterpret_cast<const uint8_t*>(&cw),
                  reinterpret_cast<const uint8_t*>(&cw) + 4);
    append.insert(append.end(), reinterpret_cast<const uint8_t*>(ucode.data()),
                  reinterpret_cast<const uint8_t*>(ucode.data()) + size_t(dwords) * 4);
    ++added;
  }
  if (!append.empty() && (added || !valid)) {
    if (!valid) {
      std::ofstream(xsh_path, std::ios::binary | std::ios::trunc)
          .write(reinterpret_cast<const char*>(append.data()), std::streamsize(append.size()));
    } else {
      // Drop any partial tail first so new entries start on a record boundary.
      std::error_code ec;
      std::filesystem::resize_file(xsh_path, off, ec);
      std::ofstream(xsh_path, std::ios::binary | std::ios::app)
          .write(reinterpret_cast<const char*>(append.data()), std::streamsize(append.size()));
    }
  }
  if (mismatched) REXLOG_WARN("NG2 shaders: {} manifest shaders did not match this game image", mismatched);
  return added;
}

// sizeof(PipelineStoredDescription) in the SDK's D3D12 pipeline cache (observed: the 207-pipeline
// storage is 12 + 207 * 72 bytes). The 12-byte header carries the format version, so a
// different record size always comes with a different header.
constexpr size_t kRecord = 72;

// Appends the shipped pipelines that the pipeline storage lacks (same file format: 12-byte
// header, then fixed-size records starting with the u64 description hash).
int MergePipelines(const std::filesystem::path& shipped_path, const std::filesystem::path& xpso_path) {
  const std::vector<uint8_t> shipped = ReadFile(shipped_path);
  if (shipped.size() < 12) return 0;
  std::vector<uint8_t> have_file = ReadFile(xpso_path);
  const bool same_header = have_file.size() >= 12 && std::memcmp(have_file.data(), shipped.data(), 12) == 0;
  if (!same_header) {
    // No (or an older-format) storage: start from the shipped list.
    std::ofstream(xpso_path, std::ios::binary | std::ios::trunc)
        .write(reinterpret_cast<const char*>(shipped.data()), std::streamsize(shipped.size()));
    return int((shipped.size() - 12) / kRecord);
  }
  std::unordered_set<std::string> have;
  for (size_t off = 12; off + kRecord <= have_file.size(); off += kRecord) {
    have.insert(std::string(reinterpret_cast<const char*>(have_file.data() + off), kRecord));
  }
  std::vector<uint8_t> append;
  for (size_t off = 12; off + kRecord <= shipped.size(); off += kRecord) {
    std::string rec(reinterpret_cast<const char*>(shipped.data() + off), kRecord);
    if (have.insert(rec).second) append.insert(append.end(), rec.begin(), rec.end());
  }
  const size_t whole = 12 + (have_file.size() - 12) / kRecord * kRecord;
  if (!append.empty()) {
    std::error_code ec;
    std::filesystem::resize_file(xpso_path, whole, ec);
    std::ofstream(xpso_path, std::ios::binary | std::ios::app)
        .write(reinterpret_cast<const char*>(append.data()), std::streamsize(append.size()));
  }
  return int(append.size() / kRecord);
}

}  // namespace

void PrepareShaderCache(const std::filesystem::path& shipped_dir,
                        const std::filesystem::path& cache_root, uint32_t title_id) {
  const auto manifest = shipped_dir / "shader_manifest.bin";
  const auto pipelines = shipped_dir / "shader_pipelines.xpso";
  std::error_code ec;
  if (!std::filesystem::exists(manifest, ec)) return;
  const auto dir = cache_root / "shaders" / "shareable";
  std::filesystem::create_directories(dir, ec);
  char title[16];
  std::snprintf(title, sizeof(title), "%08X", title_id);
  const int shaders = MergeShaders(manifest, dir / (std::string(title) + ".xsh"));
  const int pipes = std::filesystem::exists(pipelines, ec)
                        ? MergePipelines(pipelines, dir / (std::string(title) + ".rov.d3d12.xpso"))
                        : 0;
  REXLOG_INFO("NG2 shaders: precompile manifest added {} shaders, {} pipeline records", shaders,
              pipes);
}

void SetShaderProgress(int stage, uint32_t done, uint32_t total) {
  g_stage = stage;
  g_done = done;
  g_total = total;
}
void SetShaderProgressActive(bool active) { g_active = active; }
bool ShaderProgress(int* stage, uint32_t* done, uint32_t* total) {
  *stage = g_stage.load();
  *done = g_done.load();
  *total = g_total.load();
  return g_active.load();
}

}  // namespace ng2
