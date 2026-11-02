// Shader precompile from the shipped manifest (findings #58).
#pragma once

#include <cstdint>
#include <filesystem>

namespace ng2 {

// Adds shaders/pipelines from <shipped_dir>/shader_manifest.bin + shader_pipelines.xpso that
// the cache lacks. Call after the game image is loaded, before shader storage is initialized.
void PrepareShaderCache(const std::filesystem::path& shipped_dir,
                        const std::filesystem::path& cache_root, uint32_t title_id);

// Shader storage load progress (stage 0 = translating shaders, 1 = building pipelines).
void SetShaderProgress(int stage, uint32_t done, uint32_t total);
void SetShaderProgressActive(bool active);
bool ShaderProgress(int* stage, uint32_t* done, uint32_t* total);  // returns active

}  // namespace ng2
