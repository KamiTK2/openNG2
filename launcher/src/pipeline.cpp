// openNG2 launcher: the build pipeline (findings #67 for the toolchain details).
#include "pipeline.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>

#include "update_check.h"
#include "util.h"
#include "xbox.h"

namespace launcher {
namespace fs = std::filesystem;

namespace {

// Pinned toolchain (verified by SHA-256 after download).
struct Archive {
  const char* name;
  const char* url;
  const char* sha256;
  const char* folder;  // extracted top-level folder (or target folder for flat zips)
};
const Archive kLlvm = {
    "LLVM 19.1.5 (compiler)",
    "https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.5/"
    "clang%2Bllvm-19.1.5-x86_64-pc-windows-msvc.tar.xz",
    "467D1A73CA938F47734AF3BAAC2E78C5E730285469096EE088BB5C9590CABD70",
    "clang+llvm-19.1.5-x86_64-pc-windows-msvc"};
const Archive kCmake = {
    "CMake 4.4.5", "https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip",
    "326C826486D46A23CF9D5A1C9EE3A567D427BDD02C4A34071937A2A675CFF004", "cmake-4.4.5-windows-x86_64"};
const Archive kNinja = {"Ninja 1.13.2",
                        "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
                        "07FC8261B42B20E71D1720B39068C2E14FFCEE6396B76FB7A795FB460B78DC65", "ninja"};
const Archive kXwin = {
    "xwin 0.10.0 (fetches Microsoft's C++ build libraries)",
    "https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-pc-windows-msvc.tar.gz",
    "96E83665EF5C1406AABD0BE603A8D08E67E604F36FDA630B6807EE948A614E26", "xwin-0.10.0-x86_64-pc-windows-msvc"};
// Windows 10 / older Windows 11 tar.exe (bsdtar without liblzma) can't read .tar.xz itself and
// runs an external `xz -d -qq` instead, which Windows doesn't ship (findings #84). Fetched only then.
const Archive kXz = {"xz 5.8.4 (unpacks LLVM on older Windows)",
                     "https://github.com/tukaani-project/xz/releases/download/v5.8.4/xz-5.8.4-windows.zip",
                     "F31AF7638391ECF286D48BC8555CE6E131691A1C52A9E44303BCE57069E9DE56", "xz"};
constexpr char kToolchainMarker[] = "toolchain-v1.ok";

enum StepIndex : size_t { kStepTools, kStepGame, kStepUpdate, kStepDlc, kStepBuild, kStepFinish };

std::string Quote(const fs::path& p) { return "\"" + PathText(p) + "\""; }

std::string GiB(uint64_t b) {
  char s[32];
  std::snprintf(s, sizeof(s), "%.1f GB", double(b) / 1e9);
  return s;
}

fs::path FirstSubdir(const fs::path& dir) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.is_directory()) return e.path();
  }
  return {};
}

// True when this tar.exe decompresses xz itself ("liblzma/x.y.z" in its version line).
bool TarHasXz(const fs::path& tar) {
  std::string out;
  RunProcess(L"\"" + tar.wstring() + L"\" --version", {}, {}, [&](const std::string& l) { out += l; }, {});
  return out.find("liblzma") != std::string::npos;
}

// The current environment with `dir` put first on PATH.
std::vector<std::wstring> EnvWithPathFirst(const fs::path& dir) {
  std::vector<std::wstring> env;
  bool have_path = false;
  if (wchar_t* block = GetEnvironmentStringsW()) {
    for (const wchar_t* p = block; *p; p += std::wcslen(p) + 1) {
      std::wstring e = p;
      if (_wcsnicmp(e.c_str(), L"PATH=", 5) == 0) {
        e = L"PATH=" + dir.wstring() + L";" + e.substr(5);
        have_path = true;
      }
      env.push_back(std::move(e));
    }
    FreeEnvironmentStringsW(block);
  }
  if (!have_path) env.push_back(L"PATH=" + dir.wstring() + L";C:\\Windows\\System32");
  return env;
}

// Copies a folder tree (overwriting).
bool CopyTree(const fs::path& from, const fs::path& to, std::string* error) {
  std::error_code ec;
  fs::create_directories(to, ec);
  fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  if (ec) {
    *error = "Copying " + PathText(from) + " failed: " + ec.message();
    return false;
  }
  return true;
}

// Incremental updates (findings #93): the build folder is kept between builds, and files are
// only copied when their content changed, so unchanged sources and SDK headers keep their old
// timestamps and ninja rebuilds only what an update touched.
bool SameFile(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  if (!fs::exists(b, ec) || fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
  std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
  std::vector<char> ba(1 << 16), bb(1 << 16);
  while (fa && fb) {
    fa.read(ba.data(), ba.size());
    fb.read(bb.data(), bb.size());
    if (fa.gcount() != fb.gcount() || std::memcmp(ba.data(), bb.data(), size_t(fa.gcount())) != 0) return false;
  }
  return fa.eof() && fb.eof();
}
// Copies every file of `from` into `to` whose content differs (or is missing); returns the
// number copied, or -1 on error.
int SyncTree(const fs::path& from, const fs::path& to, std::string* error) {
  std::error_code ec;
  int copied = 0;
  for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const fs::path dst = to / fs::relative(it->path(), from, ec);
    if (SameFile(it->path(), dst)) continue;
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *error = "Copying " + PathText(it->path()) + " failed: " + ec.message();
      return -1;
    }
    ++copied;
  }
  if (ec) {
    *error = "Reading " + PathText(from) + " failed: " + ec.message();
    return -1;
  }
  return copied;
}

// Last-write times of the files under dir with their SHA-256, to give regenerated files whose
// content didn't change their old timestamp back (the recompiler rewrites everything).
struct FileStamp {
  std::string hash;
  fs::file_time_type time;
};
std::map<fs::path, FileStamp> SnapshotTree(const fs::path& dir) {
  std::map<fs::path, FileStamp> out;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    if (it->is_regular_file(ec)) out[it->path()] = {Sha256File(it->path()), fs::last_write_time(it->path(), ec)};
  }
  return out;
}
int RestoreUnchangedTimes(const std::map<fs::path, FileStamp>& before) {
  int restored = 0;
  std::error_code ec;
  for (const auto& [p, s] : before) {
    if (fs::exists(p, ec) && Sha256File(p) == s.hash) {
      fs::last_write_time(p, s.time, ec);
      if (!ec) ++restored;
    }
  }
  return restored;
}

// What the recompiler's output depends on: the game image, its title update, the recompiler
// and the project's recompiler config.
std::string CodegenInputs(const fs::path& game, const fs::path& sdk, const fs::path& project) {
  std::string s;
  for (const fs::path& p : {game / "default.xex", game / "default.xexp", sdk / "bin" / "rexglue.exe",
                            project / "ng2.toml", project / "ng2_manifest.toml"}) {
    s += PathText(p.filename()) + "=" + Sha256File(p) + "\n";
  }
  return s;
}

}  // namespace

fs::path Pipeline::ToolchainDir(const fs::path& install_dir) { return install_dir / "build-tools"; }

fs::path Pipeline::AppDataDir() {
  wchar_t* local = nullptr;
  fs::path p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) p = fs::path(local);
  CoTaskMemFree(local);
  return p / "openNG2";
}

std::string Pipeline::InstalledVersion(const fs::path& install_dir) {
  std::error_code ec;
  if (!fs::exists(install_dir / "ng2.exe", ec)) return {};
  std::ifstream f(install_dir / openng2::kInstallInfoFile);
  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("version=", 0) == 0 && line.size() > 8) return line.substr(8);
  }
  return "1.0";
}

bool Pipeline::CanRebuild(const fs::path& install_dir) {
  std::error_code ec;
  const fs::path game = install_dir / "game";
  return fs::exists(game / ".openng2-extracted", ec) && fs::exists(game / "default.xex", ec) &&
         fs::exists(game / "default.xexp", ec);
}

bool Pipeline::GameRunning(const fs::path& install_dir) {
  const fs::path exe = install_dir / "ng2.exe";
  HANDLE h = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_SHARING_VIOLATION;
  CloseHandle(h);
  return false;
}

Pipeline::Pipeline() {
  steps_ = {{"Download the build tools"},   {"Extract the game"}, {"Apply Title Update #2"},
            {"Add downloadable content"},   {"Recompile and build openNG2"}, {"Finish the install"}};
}

Pipeline::~Pipeline() {
  cancel_ = true;
  if (thread_.joinable()) thread_.join();
  if (log_file_) std::fclose(log_file_);
}

void Pipeline::Start(const BuildRequest& request) {
  if (running_) return;
  if (thread_.joinable()) thread_.join();
  for (auto& s : steps_) s = Step{s.title};
  {
    std::lock_guard<std::mutex> lock(mutex_);
    log_.clear();
    error_.clear();
  }
  cancel_ = false;
  finished_ = false;
  succeeded_ = false;
  running_ = true;
  thread_ = std::thread([this, request] { Run(request); });
}

void Pipeline::Reset() {
  if (running_) return;
  if (thread_.joinable()) thread_.join();
  for (auto& s : steps_) s = Step{s.title};
  finished_ = false;
  succeeded_ = false;
}

std::vector<Step> Pipeline::steps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return steps_;
}
std::vector<std::string> Pipeline::log_tail(size_t max_lines) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const size_t n = std::min(max_lines, log_.size());
  return std::vector<std::string>(log_.end() - std::ptrdiff_t(n), log_.end());
}
std::string Pipeline::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

void Pipeline::Begin(size_t i) {
  std::lock_guard<std::mutex> lock(mutex_);
  steps_[i].state = StepState::kRunning;
  steps_[i].progress = -1.0f;
  steps_[i].detail.clear();
}
void Pipeline::SetDetail(size_t i, const std::string& detail, float progress) {
  std::lock_guard<std::mutex> lock(mutex_);
  steps_[i].detail = detail;
  steps_[i].progress = progress;
}
void Pipeline::End(size_t i, StepState state) {
  std::lock_guard<std::mutex> lock(mutex_);
  steps_[i].state = state;
  if (state == StepState::kDone) steps_[i].progress = 1.0f;
}
void Pipeline::Log(const std::string& line) {
  std::lock_guard<std::mutex> lock(mutex_);
  log_.push_back(line);
  if (log_.size() > 4000) log_.erase(log_.begin(), log_.begin() + 1000);
  if (log_file_) {
    std::fputs(line.c_str(), log_file_);
    std::fputc('\n', log_file_);
    std::fflush(log_file_);
  }
}
bool Pipeline::Fail(size_t i, const std::string& message) {
  Log("ERROR: " + message);
  std::lock_guard<std::mutex> lock(mutex_);
  steps_[i].state = StepState::kFailed;
  steps_[i].detail = message;
  error_ = message;
  return false;
}

void Pipeline::Run(BuildRequest r) {
  std::error_code ec;
  fs::create_directories(r.install_dir, ec);
  log_path_ = r.install_dir / "openNG2-build.log";
  if (log_file_) std::fclose(log_file_);
  log_file_ = _wfopen(log_path_.c_str(), L"w");
  Log("openNG2 launcher build log");
  Log("Install folder: " + PathText(r.install_dir));
  bool ok = Toolchain(r) && Game(r) && Update(r) && Dlc(r) && Build(r) && Finish(r);
  if (!ok && Cancelled()) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = "The build was cancelled. Press Build to continue where it stopped.";
  }
  succeeded_ = ok;
  finished_ = true;
  running_ = false;
  Log(ok ? "Done." : "Stopped.");
}

// ---- 1. toolchain -------------------------------------------------------------------------
bool Pipeline::Toolchain(const BuildRequest& r) {
  const fs::path dir = ToolchainDir(r.install_dir);
  std::error_code ec;
  Begin(kStepTools);
  if (fs::exists(dir / kToolchainMarker, ec)) {
    SetDetail(kStepTools, "Already downloaded.", 1.0f);
    End(kStepTools, StepState::kSkipped);
    return true;
  }
  fs::create_directories(dir / "downloads", ec);
  const uint64_t need = 7'000'000'000ull;
  if (FreeDiskBytes(dir) < need) {
    return Fail(kStepTools, "Not enough free space on the drive with " + PathText(dir) + ": the build tools need about " +
                                GiB(need) + " (" + GiB(FreeDiskBytes(dir)) + " free).");
  }
  const fs::path tar = fs::path(L"C:\\Windows\\System32\\tar.exe");
  // Windows 10 before 1803 has no tar.exe: every unpack below would fail with a misleading
  // "free space / antivirus" message.
  if (!fs::exists(tar, ec)) {
    return Fail(kStepTools, "This Windows has no tar.exe (C:\\Windows\\System32\\tar.exe), which unpacking the "
                            "build tools needs. openNG2 needs Windows 10 version 1803 or newer.");
  }
  // OPENNG2_FORCE_XZ=1 takes the fallback path on any Windows (testing).
  const bool force_xz = GetEnvironmentVariableW(L"OPENNG2_FORCE_XZ", nullptr, 0) > 0;
  const bool need_xz = !(fs::is_directory(dir / kLlvm.folder, ec) && fs::exists(dir / kLlvm.folder / ".ok", ec)) &&
                       (force_xz || !TarHasXz(tar));
  if (need_xz) Log("This Windows tar.exe can't unpack .tar.xz by itself; fetching xz for it.");
  std::vector<const Archive*> archives;
  if (need_xz) archives.push_back(&kXz);  // first, so tar finds xz on PATH for LLVM
  for (const Archive* ar : {&kLlvm, &kCmake, &kNinja, &kXwin}) archives.push_back(ar);
  const std::vector<std::wstring> xz_env = need_xz ? EnvWithPathFirst(dir / "xz" / "bin_x86-64") : std::vector<std::wstring>{};
  const float share = 1.0f / float(archives.size());
  for (size_t a = 0; a < archives.size(); ++a) {
    const Archive& ar = *archives[a];
    if (fs::is_directory(dir / ar.folder, ec) && fs::exists(dir / ar.folder / ".ok", ec)) continue;
    const std::string url = ar.url;
    const fs::path file = dir / "downloads" / fs::u8path(url.substr(url.rfind('/') + 1));
    if (!fs::exists(file, ec) || Sha256File(file) != ar.sha256) {
      Log(std::string("Downloading ") + ar.name);
      std::string err;
      const bool ok = Download(
          url, file,
          [&](uint64_t done, uint64_t total) {
            char t[160];
            std::snprintf(t, sizeof(t), "Downloading %s: %.0f / %.0f MB", ar.name, done / 1e6, total / 1e6);
            SetDetail(kStepTools, t, total ? (float(double(done) / double(total)) + float(a)) * share : -1.0f);
          },
          [this] { return Cancelled(); }, &err);
      if (!ok) return Fail(kStepTools, err);
      if (Sha256File(file) != ar.sha256) {
        fs::remove(file, ec);
        return Fail(kStepTools, std::string("The download of ") + ar.name +
                                    " is damaged (checksum mismatch). Please try again.");
      }
    }
    SetDetail(kStepTools, std::string("Unpacking ") + ar.name + "...", -1.0f);
    Log(std::string("Unpacking ") + ar.name);
    // Flat zips (no top-level folder) go into their own folder.
    const bool flat = std::strcmp(ar.folder, "ninja") == 0 || &ar == &kXz;
    const fs::path target = flat ? dir / ar.folder : dir;
    fs::create_directories(target, ec);
    const std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + file.wstring() + L"\" -C \"" +
                             target.wstring() + L"\"";
    // bsdtar finds `xz` with SearchPath (current folder, then PATH), so give it both.
    const bool use_xz = need_xz && &ar == &kLlvm;
    if (RunProcess(cmd, use_xz ? dir / "xz" / "bin_x86-64" : dir, use_xz ? xz_env : std::vector<std::wstring>{},
                   [this](const std::string& l) { Log(l); }, [this] { return Cancelled(); }) != 0) {
      return Fail(kStepTools, std::string("Unpacking ") + ar.name +
                                  " failed (see the log). Check that the drive has free space and that "
                                  "antivirus software isn't blocking the launcher, then press Build again.");
    }
    if (&ar == &kXz && !fs::exists(dir / "xz" / "bin_x86-64" / "xz.exe", ec)) {
      return Fail(kStepTools, "Unpacking xz failed: xz.exe is missing.");
    }
    std::ofstream(dir / ar.folder / ".ok") << "ok\n";
    fs::remove(file, ec);  // keep the disk use down
  }
  // Microsoft's C++ runtime + Windows SDK (the player accepted the licence in the UI).
  if (!fs::exists(dir / "winsdk" / ".ok", ec)) {
    SetDetail(kStepTools,
              "Downloading Microsoft's C++ build libraries and Windows SDK (about 1 GB, needed to compile; "
              "not the Visual C++ Redistributable)...",
              -1.0f);
    Log("Running xwin");
    fs::remove_all(dir / "winsdk", ec);
    const fs::path xwin = dir / kXwin.folder / "xwin.exe";
    const std::wstring cmd = L"\"" + xwin.wstring() + L"\" --accept-license --arch x86_64 --cache-dir \"" +
                             (dir / "xwin-cache").wstring() +
                             L"\" splat --use-winsysroot-style --preserve-ms-arch-notation "
                             L"--disable-symlinks --output \"" +
                             (dir / "winsdk").wstring() + L"\"";
    if (RunProcess(cmd, dir, {}, [this](const std::string& l) { Log(l); }, [this] { return Cancelled(); }) != 0) {
      return Fail(kStepTools, "Downloading Microsoft's C++ build libraries failed (see the log). Check your "
                              "internet connection and try again.");
    }
    fs::remove_all(dir / "xwin-cache", ec);
    std::ofstream(dir / "winsdk" / ".ok") << "ok\n";
  }
  std::ofstream(dir / kToolchainMarker) << "ok\n";
  End(kStepTools, StepState::kDone);
  return true;
}

// ---- 2. game -------------------------------------------------------------------------------
bool Pipeline::Game(const BuildRequest& r) {
  Begin(kStepGame);
  const fs::path out = r.install_dir / "game";
  std::error_code ec;
  if (fs::exists(out / "default.xex", ec) && fs::exists(out / ".openng2-extracted", ec)) {
    SetDetail(kStepGame, "Already extracted.", 1.0f);
    End(kStepGame, StepState::kSkipped);
    return true;
  }
  if (r.game.path.empty()) {
    return Fail(kStepGame, "The game files in " + PathText(out) + " are missing. Add your game files again and build.");
  }
  const uint64_t need = 16'000'000'000ull;
  if (FreeDiskBytes(r.install_dir) < need) {
    return Fail(kStepGame, "Not enough free space in " + PathText(r.install_dir) + ": the game and the build need about " +
                               GiB(need) + " (" + GiB(FreeDiskBytes(r.install_dir)) + " free).");
  }
  auto progress = [&](uint64_t done, uint64_t total) {
    char t[96];
    std::snprintf(t, sizeof(t), "%.1f / %.1f GB", done / 1e9, total / 1e9);
    SetDetail(kStepGame, t, total ? float(double(done) / double(total)) : -1.0f);
  };
  std::string err;
  if (r.game.game_kind == GameSourceKind::kFolder) {
    // Copy the folder (with progress), skipping files already copied.
    uint64_t total = 0, done = 0;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(r.game.path, ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (it->is_regular_file(ec)) {
        const std::string rel = PathText(fs::relative(it->path(), r.game.path, ec));
        if (rel.rfind("dlc", 0) == 0 || rel.rfind("update", 0) == 0) continue;  // added separately
        files.push_back(it->path());
        total += it->file_size(ec);
      }
    }
    for (const auto& f : files) {
      if (Cancelled()) return false;
      const fs::path dst = out / fs::relative(f, r.game.path, ec);
      fs::create_directories(dst.parent_path(), ec);
      const uint64_t size = fs::file_size(f, ec);
      if (!(fs::exists(dst, ec) && fs::file_size(dst, ec) == size)) {
        fs::copy_file(f, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) return Fail(kStepGame, "Copying " + PathText(f) + " failed: " + ec.message());
      }
      done += size;
      progress(done, total);
    }
  } else {
    std::unique_ptr<ImageSource> src =
        r.game.game_kind == GameSourceKind::kGod ? OpenGod(r.game.path, &err) : OpenIso(r.game.path, &err);
    if (!src) return Fail(kStepGame, err);
    Log("Extracting " + PathText(r.game.path));
    if (!ExtractXdvdfs(*src, out, progress, [this] { return Cancelled(); }, &err)) {
      return Cancelled() ? false : Fail(kStepGame, err);
    }
  }
  if (Sha256File(out / "default.xex") != kXexSha256) {
    return Fail(kStepGame, "The extracted default.xex does not match the supported version - the source files may be damaged.");
  }
  std::ofstream(out / ".openng2-extracted") << "ok\n";
  End(kStepGame, StepState::kDone);
  return true;
}

// ---- 3. title update ------------------------------------------------------------------------
bool Pipeline::Update(const BuildRequest& r) {
  Begin(kStepUpdate);
  std::string err;
  const fs::path out = r.install_dir / "game" / "default.xexp";
  if (r.update.path.empty()) {
    std::error_code ec;
    if (!fs::exists(out, ec)) return Fail(kStepUpdate, "Title Update #2 is missing. Add it again and build.");
    SetDetail(kStepUpdate, "Already applied.", 1.0f);
    End(kStepUpdate, StepState::kSkipped);
    return true;
  }
  if (!ExtractStfsFile(r.update.path, "default.xexp", out, &err)) return Fail(kStepUpdate, err);
  Log("Title Update #2: default.xexp extracted");
  End(kStepUpdate, StepState::kDone);
  return true;
}

// ---- 4. DLC ----------------------------------------------------------------------------------
bool Pipeline::Dlc(const BuildRequest& r) {
  Begin(kStepDlc);
  std::error_code ec;
  const fs::path dir = r.install_dir / "game" / "dlc";
  if (r.dlc.empty()) {
    SetDetail(kStepDlc, r.rebuild ? "Kept as installed." : "None added (optional).", 1.0f);
    End(kStepDlc, StepState::kSkipped);
    return true;
  }
  fs::create_directories(dir, ec);
  for (const auto& d : r.dlc) {
    const fs::path dst = dir / kDlc[size_t(d.dlc_index)].file_name;
    fs::copy_file(d.path, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) return Fail(kStepDlc, "Copying " + PathText(d.path) + " failed: " + ec.message());
    Log(std::string("DLC added: ") + kDlc[size_t(d.dlc_index)].title);
  }
  End(kStepDlc, StepState::kDone);
  return true;
}

// ---- 5. recompile + build -------------------------------------------------------------------
bool Pipeline::Build(const BuildRequest& r) {
  Begin(kStepBuild);
  std::error_code ec;
  std::string err;
  const fs::path tc = ToolchainDir(r.install_dir);
  const fs::path project = r.install_dir / "build" / "project" / "ng2";
  const fs::path out = r.install_dir / "build" / "out";
  const fs::path payload_sdk = r.payload_dir / "sdk";
  // The SDK is mirrored into the build folder (copy-if-changed): every recompiled file includes
  // its headers, so a fresh copy with new timestamps would force a full rebuild each update.
  const fs::path sdk = r.install_dir / "build" / "sdk";
  const fs::path stamp_file = r.install_dir / "build" / "codegen.stamp";
  if (!fs::exists(r.payload_dir / "project" / "ng2" / "CMakeLists.txt", ec) || !fs::exists(payload_sdk, ec)) {
    return Fail(kStepBuild, "The launcher's payload folder is missing or incomplete (" + PathText(r.payload_dir) +
                                "). Re-download openNG2.");
  }
  const bool incremental = fs::exists(out / "build.ninja", ec);
  if (incremental) Log("Previous build found: only what changed is rebuilt");
  // Project sources (changed files only), with the manifest pointing at <install>\game.
  SetDetail(kStepBuild, "Preparing the project...", -1.0f);
  {
    const fs::path src = r.payload_dir / "project" / "ng2";
    int copied = 0;
    for (const auto& e : fs::directory_iterator(src, ec)) {
      const fs::path name = e.path().filename();
      if (name == "ng2_manifest.toml") continue;
      if (e.is_directory(ec)) {
        const int n = SyncTree(e.path(), project / name, &err);
        if (n < 0) return Fail(kStepBuild, err);
        copied += n;
      } else if (!SameFile(e.path(), project / name)) {
        fs::create_directories(project, ec);
        fs::copy_file(e.path(), project / name, fs::copy_options::overwrite_existing, ec);
        if (ec) return Fail(kStepBuild, "Copying " + PathText(e.path()) + " failed: " + ec.message());
        ++copied;
      }
    }
    std::ifstream in(src / "ng2_manifest.toml", std::ios::binary);
    std::string m((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    size_t pos;
    while ((pos = m.find("\"../../game")) != std::string::npos) m.replace(pos, 11, "\"../../../game");
    std::ifstream cur_in(project / "ng2_manifest.toml", std::ios::binary);
    const std::string cur((std::istreambuf_iterator<char>(cur_in)), std::istreambuf_iterator<char>());
    cur_in.close();
    if (cur != m) {
      std::ofstream(project / "ng2_manifest.toml", std::ios::binary | std::ios::trunc) << m;
      ++copied;
    }
    const int sdk_copied = SyncTree(payload_sdk, sdk, &err);
    if (sdk_copied < 0) return Fail(kStepBuild, err);
    Log("Project files updated: " + std::to_string(copied) + ", SDK files updated: " + std::to_string(sdk_copied));
  }
  // Clean build environment: only our toolchain (findings #67).
  const fs::path llvm = tc / kLlvm.folder, cmake = tc / kCmake.folder / "bin" / "cmake.exe";
  const fs::path ninja_dir = tc / "ninja";
  const fs::path vc = FirstSubdir(tc / "winsdk" / "VC" / "Tools" / "MSVC");
  const fs::path kits = tc / "winsdk" / "Windows Kits" / "10";
  const std::wstring sdkv = FirstSubdir(kits / "Include").filename().wstring();
  if (vc.empty() || sdkv.empty()) return Fail(kStepBuild, "The Microsoft libraries are incomplete; delete " + PathText(tc) + " and build again.");
  std::vector<std::wstring> env;
  for (const wchar_t* name : {L"SystemRoot", L"SystemDrive", L"TEMP", L"TMP", L"USERPROFILE", L"LOCALAPPDATA",
                              L"APPDATA", L"ComSpec", L"PATHEXT", L"NUMBER_OF_PROCESSORS", L"PROCESSOR_ARCHITECTURE",
                              L"windir", L"USERNAME"}) {
    wchar_t v[2048];
    if (GetEnvironmentVariableW(name, v, 2048)) env.push_back(std::wstring(name) + L"=" + v);
  }
  env.push_back(L"PATH=" + (llvm / "bin").wstring() + L";" + cmake.parent_path().wstring() + L";" + ninja_dir.wstring() +
                L";C:\\Windows\\System32;C:\\Windows");
  env.push_back(L"VCToolsInstallDir=" + vc.wstring() + L"\\");
  env.push_back(L"WindowsSdkDir=" + kits.wstring() + L"\\");
  env.push_back(L"WindowsSDKVersion=" + sdkv + L"\\");
  const fs::path inc = kits / "Include" / sdkv, lib = kits / "Lib" / sdkv;
  env.push_back(L"INCLUDE=" + (vc / "include").wstring() + L";" + (inc / "ucrt").wstring() + L";" + (inc / "um").wstring() +
                L";" + (inc / "shared").wstring() + L";" + (inc / "winrt").wstring() + L";" + (inc / "cppwinrt").wstring());
  env.push_back(L"LIB=" + (vc / "lib" / "x64").wstring() + L";" + (lib / "ucrt" / "x64").wstring() + L";" +
                (lib / "um" / "x64").wstring());

  auto log_line = [this](const std::string& l) { Log(l); };
  // A kept build can go stale in ways ninja can't see (install folder moved, toolchain
  // replaced): on failure, start over once from a clean build folder.
  auto retry_clean = [&](const std::string& message) {
    if (Cancelled()) return false;
    if (!incremental || clean_retry_) return Fail(kStepBuild, message);
    Log("The quick rebuild failed: starting a clean build");
    std::error_code rm;
    fs::remove_all(r.install_dir / "build", rm);
    if (rm || fs::exists(r.install_dir / "build" / "out" / "build.ninja", rm)) {
      // Something still holds the build folder (another build, an antivirus scan): retrying on
      // the same files fails the same way, so say what to do instead.
      return Fail(kStepBuild, "Some build files in " + PathText(r.install_dir / "build") +
                                  " are still in use - most likely another openNG2 build is still running. Wait a "
                                  "few minutes (or restart your PC) and build again.");
    }
    clean_retry_ = true;
    const bool ok = Build(r);
    clean_retry_ = false;
    return ok;
  };
  SetDetail(kStepBuild, "Configuring...", -1.0f);
  const std::wstring configure = L"\"" + cmake.wstring() + L"\" -G Ninja -S \"" + project.wstring() + L"\" -B \"" +
                                 out.wstring() +
                                 L"\" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "
                                 L"-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-rc "
                                 L"\"-Drexglue_DIR=" + (sdk / "lib" / "cmake" / "rexglue").wstring() + L"\" -DOPENNG2_VERSION=" +
                                 Widen(openng2::kVersion);
  if (RunProcess(configure, project, env, log_line, [this] { return Cancelled(); }) != 0) {
    return retry_clean("Configuring the build failed (see the log).");
  }
  // The recompiled sources are only added to the build when generated/default/sources.cmake
  // exists at configure time: run the recompiler first, then configure again. Skipped when
  // nothing it reads changed since the last build (most updates only touch our own code).
  const fs::path generated = project / "generated";
  const std::string inputs = CodegenInputs(r.install_dir / "game", sdk, project);
  std::string last_inputs;
  {
    std::ifstream f(stamp_file, std::ios::binary);
    last_inputs.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  const bool codegen_current = inputs == last_inputs && fs::exists(generated / "default" / "sources.cmake", ec);
  if (codegen_current) {
    Log("Game code unchanged since the last build: recompiler skipped");
  } else {
    SetDetail(kStepBuild, "Recompiling the game code (about a minute)...", -1.0f);
    // The recompiler rewrites every file; identical ones get their old timestamps back so they
    // aren't compiled again.
    const auto before = fs::exists(generated, ec) ? SnapshotTree(generated) : std::map<fs::path, FileStamp>{};
    fs::remove(stamp_file, ec);
    const std::wstring codegen = L"\"" + cmake.wstring() + L"\" --build \"" + out.wstring() + L"\" --target ng2_codegen";
    if (RunProcess(codegen, project, env, log_line, [this] { return Cancelled(); }) != 0) {
      return retry_clean("Recompiling the game code failed (see the log).");
    }
    if (!before.empty()) {
      Log("Recompiled files unchanged: " + std::to_string(RestoreUnchangedTimes(before)) + " of " +
          std::to_string(before.size()));
    }
    if (RunProcess(configure, project, env, log_line, [this] { return Cancelled(); }) != 0) {
      return retry_clean("Configuring the build failed (see the log).");
    }
  }
  std::error_code gen_ec;
  if (!fs::exists(project / "generated" / "default" / "sources.cmake", gen_ec)) {
    return Fail(kStepBuild, "The recompiler produced no code (see the log).");
  }
  if (!codegen_current) std::ofstream(stamp_file, std::ios::binary | std::ios::trunc) << inputs;
  // Each compiler job on the recompiled code needs ~2.5 GB of RAM at most.
  const unsigned by_ram = unsigned(std::max<uint64_t>(1, TotalRamBytes() / 2'500'000'000ull));
  const unsigned jobs = std::max(1u, std::min(CpuThreads(), by_ram));
  Log("Building with " + std::to_string(jobs) + " parallel jobs");
  static const std::regex progress_re(R"(^\[(\d+)/(\d+)\])");
  const std::wstring build = L"\"" + cmake.wstring() + L"\" --build \"" + out.wstring() + L"\" -j " + std::to_wstring(jobs);
  const int code = RunProcess(
      build, project, env,
      [&](const std::string& l) {
        std::smatch m;
        if (std::regex_search(l, m, progress_re)) {
          const double n = std::stod(m[1]), t = std::stod(m[2]);
          char d[96];
          std::snprintf(d, sizeof(d), "Compiling: step %.0f of %.0f", n, t);
          SetDetail(kStepBuild, d, float(n / t));
          // Only keep the interesting lines in the UI log (the full log is on disk).
          if (l.find("Generating") != std::string::npos || l.find("Linking") != std::string::npos) Log(l);
        } else {
          Log(l);
        }
      },
      [this] { return Cancelled(); });
  if (code != 0) return retry_clean("Building openNG2 failed (see the log).");

  End(kStepBuild, StepState::kDone);
  return true;
}

// ---- 6. finish ---------------------------------------------------------------------------------
bool Pipeline::Finish(const BuildRequest& r) {
  Begin(kStepFinish);
  std::error_code ec;
  const fs::path out = r.install_dir / "build" / "out";
  // An update can be built while the old version is still being played; its files can only be
  // replaced once it is closed. Meanwhile the ready marker tells the running game, which offers
  // Restart now / Restart after next save (GitHub issue #2).
  const fs::path ready_marker = r.install_dir / openng2::kUpdateReadyFile;
  const fs::path restart_request = r.install_dir / openng2::kUpdateRestartFile;
  if (GameRunning(r.install_dir)) {
    std::ofstream m(ready_marker, std::ios::trunc);
    m << "version=" << openng2::kVersion << "\n" << "pid=" << GetCurrentProcessId() << "\n";
  } else {
    fs::remove(restart_request, ec);  // stale request from an earlier session
  }
  while (GameRunning(r.install_dir)) {
    if (Cancelled()) {
      fs::remove(ready_marker, ec);
      return false;
    }
    SetDetail(kStepFinish, "Close Ninja Gaiden II to finish installing the update (it shows a restart notice)...",
              -1.0f);
    Sleep(1000);
  }
  fs::remove(ready_marker, ec);
  SetDetail(kStepFinish, "", -1.0f);
  for (const auto& e : fs::directory_iterator(out, ec)) {
    const fs::path& p = e.path();
    const std::string ext = p.extension().string();
    if (e.is_regular_file() && (ext == ".exe" || ext == ".dll" || p.filename() == "ng2.toml")) {
      fs::copy_file(p, r.install_dir / p.filename(), fs::copy_options::overwrite_existing, ec);
      if (ec) return Fail(kStepFinish, "Copying " + PathText(p.filename()) + " failed: " + ec.message() +
                                           " (is openNG2 running?)");
    }
  }
  std::string err;
  if (fs::is_directory(out / "shaders", ec) && !CopyTree(out / "shaders", r.install_dir / "shaders", &err)) {
    return Fail(kStepFinish, err);
  }
  // The build folder (~2 GB: recompiled sources, objects, SDK mirror) is kept so the next
  // update only rebuilds what changed (findings #93).
  exe_path_ = r.install_dir / "ng2.exe";
  if (!fs::exists(exe_path_, ec)) return Fail(kStepFinish, "ng2.exe was not produced.");
  if (!EnsureVcRuntime(r)) return false;
  {
    // Read by the launcher (installed version) and the game (which launcher to open to update).
    wchar_t self[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
    std::ofstream info(r.install_dir / openng2::kInstallInfoFile, std::ios::trunc);
    info << "version=" << openng2::kVersion << "\n" << "launcher=" << PathText(fs::path(self)) << "\n";
  }
  Log("Installed: " + PathText(exe_path_) + " (openNG2 " + openng2::kVersion + ")");
  if (fs::exists(restart_request, ec)) {
    // The game quit for this update: start the new version.
    fs::remove(restart_request, ec);
    Log("Restarting the game (it closed for the update).");
    ShellExecuteW(nullptr, L"open", exe_path_.c_str(), nullptr, r.install_dir.c_str(), SW_SHOW);
  }
  End(kStepFinish, StepState::kDone);
  return true;
}

// ng2.exe uses the Microsoft Visual C++ 2015-2022 runtime. Most PCs have it; otherwise install
// Microsoft's redistributable (Windows asks the player for permission).
bool Pipeline::EnsureVcRuntime(const BuildRequest& r) {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  std::error_code ec;
  const fs::path s(sys);
  if (fs::exists(s / "msvcp140.dll", ec) && fs::exists(s / "vcruntime140_1.dll", ec)) return true;
  SetDetail(kStepFinish, "Installing the Microsoft Visual C++ runtime...", -1.0f);
  const fs::path file = ToolchainDir(r.install_dir) / "downloads" / "vc_redist.x64.exe";
  std::string err;
  if (!Download("https://aka.ms/vs/17/release/vc_redist.x64.exe", file, [](uint64_t, uint64_t) {},
                [this] { return Cancelled(); }, &err)) {
    return Fail(kStepFinish, "Downloading the Microsoft Visual C++ runtime failed: " + err);
  }
  SHELLEXECUTEINFOW sei{sizeof(sei)};
  sei.fMask = SEE_MASK_NOCLOSEPROCESS;
  sei.lpVerb = L"runas";
  sei.lpFile = file.c_str();
  sei.lpParameters = L"/install /passive /norestart";
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) {
    return Fail(kStepFinish, "The Microsoft Visual C++ runtime is needed to run the game. Install it from "
                             "https://aka.ms/vs/17/release/vc_redist.x64.exe and press Build again.");
  }
  WaitForSingleObject(sei.hProcess, INFINITE);
  CloseHandle(sei.hProcess);
  Log("Microsoft Visual C++ runtime installed");
  return true;
}

}  // namespace launcher
