// openNG2 launcher: small Windows helpers (hashing, text, processes, downloads).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace launcher {

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& wide);
std::string PathText(const std::filesystem::path& p);  // UTF-8 for display

// Upper-case hex SHA-256.
std::string Sha256(const void* data, size_t size);
std::string Sha256File(const std::filesystem::path& file, const std::function<bool()>& cancel = {});

// Runs a command line; every output line (stdout + stderr) goes to on_line. `env` replaces the
// environment when non-empty ("NAME=value" entries). Returns the exit code, or -1 when it could
// not start / was cancelled.
int RunProcess(const std::wstring& command_line, const std::filesystem::path& work_dir,
               const std::vector<std::wstring>& env, const std::function<void(const std::string&)>& on_line,
               const std::function<bool()>& cancel);

// HTTPS download with progress(bytes_done, bytes_total or 0). False + error on failure.
bool Download(const std::string& url, const std::filesystem::path& out_file,
              const std::function<void(uint64_t, uint64_t)>& progress,
              const std::function<bool()>& cancel, std::string* error);

uint64_t FreeDiskBytes(const std::filesystem::path& path);
uint64_t TotalRamBytes();
unsigned CpuThreads();

}  // namespace launcher
