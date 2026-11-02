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
#include <fstream>
#include <regex>
#include <sstream>

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
    "xwin 0.10.0 (fetches Microsoft's C++ libraries)",
    "https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-pc-windows-msvc.tar.gz",
    "96E83665EF5C1406AABD0BE603A8D08E67E604F36FDA630B6807EE948A614E26", "xwin-0.10.0-x86_64-pc-windows-msvc"};
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

}  // namespace

fs::path Pipeline::ToolchainDir(const fs::path& install_dir) { return install_dir / "build-tools"; }

fs::path Pipeline::AppDataDir() {
  wchar_t* local = nullptr;
  fs::path p;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) p = fs::path(local);
  CoTaskMemFree(local);
  return p / "openNG2";
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
  const Archive* archives[] = {&kLlvm, &kCmake, &kNinja, &kXwin};
  for (size_t a = 0; a < 4; ++a) {
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
            SetDetail(kStepTools, t, total ? float(double(done) / double(total)) * 0.25f + 0.25f * a : -1.0f);
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
    const fs::path target = (std::strcmp(ar.folder, "ninja") == 0) ? dir / "ninja" : dir;
    fs::create_directories(target, ec);
    const std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + file.wstring() + L"\" -C \"" +
                             target.wstring() + L"\"";
    if (RunProcess(cmd, dir, {}, [this](const std::string& l) { Log(l); }, [this] { return Cancelled(); }) != 0) {
      return Fail(kStepTools, std::string("Unpacking ") + ar.name + " failed.");
    }
    std::ofstream(dir / ar.folder / ".ok") << "ok\n";
    fs::remove(file, ec);  // keep the disk use down
  }
  // Microsoft's C++ runtime + Windows SDK (the player accepted the licence in the UI).
  if (!fs::exists(dir / "winsdk" / ".ok", ec)) {
    SetDetail(kStepTools, "Downloading Microsoft's C++ libraries and Windows SDK (about 1 GB)...", -1.0f);
    Log("Running xwin");
    fs::remove_all(dir / "winsdk", ec);
    const fs::path xwin = dir / kXwin.folder / "xwin.exe";
    const std::wstring cmd = L"\"" + xwin.wstring() + L"\" --accept-license --arch x86_64 --cache-dir \"" +
                             (dir / "xwin-cache").wstring() +
                             L"\" splat --use-winsysroot-style --preserve-ms-arch-notation "
                             L"--disable-symlinks --output \"" +
                             (dir / "winsdk").wstring() + L"\"";
    if (RunProcess(cmd, dir, {}, [this](const std::string& l) { Log(l); }, [this] { return Cancelled(); }) != 0) {
      return Fail(kStepTools, "Downloading Microsoft's C++ libraries failed (see the log). Check your "
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
    SetDetail(kStepDlc, "None added (optional).", 1.0f);
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
  const fs::path sdk = r.payload_dir / "sdk";
  if (!fs::exists(r.payload_dir / "project" / "ng2" / "CMakeLists.txt", ec) || !fs::exists(sdk, ec)) {
    return Fail(kStepBuild, "The launcher's payload folder is missing or incomplete (" + PathText(r.payload_dir) +
                                "). Re-download openNG2.");
  }
  // Project sources, with the manifest pointing at <install>\game.
  SetDetail(kStepBuild, "Preparing the project...", -1.0f);
  if (!CopyTree(r.payload_dir / "project" / "ng2", project, &err)) return Fail(kStepBuild, err);
  {
    std::ifstream in(project / "ng2_manifest.toml", std::ios::binary);
    std::string m((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    size_t pos;
    while ((pos = m.find("\"../../game")) != std::string::npos) m.replace(pos, 11, "\"../../../game");
    std::ofstream(project / "ng2_manifest.toml", std::ios::binary | std::ios::trunc) << m;
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
  SetDetail(kStepBuild, "Configuring...", -1.0f);
  const std::wstring configure = L"\"" + cmake.wstring() + L"\" -G Ninja -S \"" + project.wstring() + L"\" -B \"" +
                                 out.wstring() +
                                 L"\" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TRY_COMPILE_CONFIGURATION=Release "
                                 L"-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_RC_COMPILER=llvm-rc "
                                 L"\"-Drexglue_DIR=" + (sdk / "lib" / "cmake" / "rexglue").wstring() + L"\"";
  if (RunProcess(configure, project, env, log_line, [this] { return Cancelled(); }) != 0) {
    return Cancelled() ? false : Fail(kStepBuild, "Configuring the build failed (see the log).");
  }
  // The recompiled sources are only added to the build when generated/default/sources.cmake
  // exists at configure time: run the recompiler first, then configure again.
  SetDetail(kStepBuild, "Recompiling the game code (about a minute)...", -1.0f);
  const std::wstring codegen = L"\"" + cmake.wstring() + L"\" --build \"" + out.wstring() + L"\" --target ng2_codegen";
  if (RunProcess(codegen, project, env, log_line, [this] { return Cancelled(); }) != 0) {
    return Cancelled() ? false : Fail(kStepBuild, "Recompiling the game code failed (see the log).");
  }
  if (RunProcess(configure, project, env, log_line, [this] { return Cancelled(); }) != 0) {
    return Cancelled() ? false : Fail(kStepBuild, "Configuring the build failed (see the log).");
  }
  std::error_code gen_ec;
  if (!fs::exists(project / "generated" / "default" / "sources.cmake", gen_ec)) {
    return Fail(kStepBuild, "The recompiler produced no code (see the log).");
  }
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
  if (code != 0) return Cancelled() ? false : Fail(kStepBuild, "Building openNG2 failed (see the log).");
  End(kStepBuild, StepState::kDone);
  return true;
}

// ---- 6. finish ---------------------------------------------------------------------------------
bool Pipeline::Finish(const BuildRequest& r) {
  Begin(kStepFinish);
  std::error_code ec;
  const fs::path out = r.install_dir / "build" / "out";
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
  // The recompiled sources and objects are only needed to build (several GB).
  SetDetail(kStepFinish, "Removing temporary build files...", -1.0f);
  fs::remove_all(r.install_dir / "build", ec);
  exe_path_ = r.install_dir / "ng2.exe";
  if (!fs::exists(exe_path_, ec)) return Fail(kStepFinish, "ng2.exe was not produced.");
  if (!EnsureVcRuntime(r)) return false;
  Log("Installed: " + PathText(exe_path_));
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
