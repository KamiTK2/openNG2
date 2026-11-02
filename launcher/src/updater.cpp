// openNG2 launcher: self-update (see updater.h).
#include "updater.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <vector>

#include "pipeline.h"
#include "util.h"

namespace launcher {
namespace fs = std::filesystem;

namespace {
constexpr wchar_t kStaging[] = L".openNG2-update";
constexpr wchar_t kOldSuffix[] = L".old";
std::string UserAgent() { return std::string("openNG2-launcher/") + openng2::kVersion; }
}  // namespace

SelfUpdater::~SelfUpdater() { Join(); }

void SelfUpdater::Join() {
  if (thread_.joinable()) thread_.join();
}

fs::path SelfUpdater::LauncherDir() {
  wchar_t exe[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  return fs::path(exe).parent_path();
}

openng2::ReleaseInfo SelfUpdater::release() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return release_;
}
std::string SelfUpdater::message() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_;
}
void SelfUpdater::SetMessage(const std::string& m, float progress) {
  std::lock_guard<std::mutex> lock(mutex_);
  message_ = m;
  progress_ = progress;
}

void SelfUpdater::Check() {
  if (busy()) return;
  Join();
  state_ = State::kChecking;
  SetMessage("", 0.0f);
  thread_ = std::thread([this] {
    openng2::ReleaseInfo r = openng2::FetchLatestRelease(UserAgent().c_str());
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_ = r;
      message_ = r.ok ? "" : r.error;
    }
    state_ = !r.ok ? State::kCheckFailed
             : openng2::IsNewer(r.version, openng2::kVersion) ? State::kAvailable
                                                              : State::kUpToDate;
  });
}

void SelfUpdater::Install() {
  if (state_ != State::kAvailable && state_ != State::kFailed) return;
  Join();
  state_ = State::kInstalling;
  SetMessage("Starting...", -1.0f);
  thread_ = std::thread([this] {
    std::string err;
    if (DoInstall(&err)) {
      SetMessage("Installed. Restarting the launcher...", 1.0f);
      state_ = State::kReady;
    } else {
      SetMessage(err, 0.0f);
      state_ = State::kFailed;
    }
  });
}

bool SelfUpdater::DoInstall(std::string* error) {
  const openng2::ReleaseInfo r = release();
  if (r.zip_url.empty()) {
    *error = "Release " + r.version + " has no openNG2 download attached yet. Try again later, or get it from the "
             "release page.";
    return false;
  }
  const fs::path dir = LauncherDir();
  const fs::path staging = dir / kStaging;
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec || !fs::is_directory(staging, ec)) {
    *error = "The launcher's folder (" + PathText(dir) +
             ") cannot be written to, so it cannot update itself. Download the new version from the release page "
             "and unpack it over this one (or move openNG2 to a folder you own).";
    return false;
  }

  // 1. Download (kept in %LOCALAPPDATA%\openNG2\updates, reused when already complete).
  const fs::path zip = Pipeline::AppDataDir() / "updates" / ("openNG2-" + r.version + ".zip");
  auto verified = [&] { return r.zip_sha256.empty() || Sha256File(zip) == r.zip_sha256; };
  if (!(fs::exists(zip, ec) && (!r.zip_size || fs::file_size(zip, ec) == r.zip_size) && verified())) {
    fs::remove(zip, ec);
    const std::string label = "Downloading openNG2 " + r.version;
    SetMessage(label + "...", -1.0f);
    if (!Download(
            r.zip_url, zip,
            [&](uint64_t done, uint64_t total) {
              char t[160];
              if (total) {
                std::snprintf(t, sizeof(t), "%s: %.0f / %.0f MB", label.c_str(), done / 1e6, total / 1e6);
              } else {
                std::snprintf(t, sizeof(t), "%s: %.0f MB", label.c_str(), done / 1e6);
              }
              SetMessage(t, total ? float(double(done) / double(total)) : -1.0f);
            },
            {}, error)) {
      return false;
    }
    SetMessage("Checking the download...", -1.0f);
    if (!verified()) {
      fs::remove(zip, ec);
      *error = "The download is damaged (SHA-256 does not match GitHub's). Try again.";
      return false;
    }
  }

  // 2. Extract next to the launcher (same drive, so the swap below is a rename).
  SetMessage("Unpacking...", -1.0f);
  const fs::path tar = fs::path(L"C:\\Windows\\System32\\tar.exe");
  const std::wstring cmd = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + staging.wstring() + L"\"";
  std::string tar_out;
  if (RunProcess(cmd, staging, {}, [&](const std::string& l) { tar_out += l + "\n"; }, {}) != 0) {
    *error = "Unpacking the update failed: " + tar_out;
    return false;
  }
  fs::path src = staging / "openNG2";
  if (!fs::is_directory(src, ec)) src = staging;
  if (!fs::exists(src / "openNG2.exe", ec) || !fs::is_directory(src / "payload", ec)) {
    *error = "The downloaded release does not look like an openNG2 release (no openNG2.exe / payload).";
    return false;
  }

  // 3. Swap: current entries become *.old, new ones move in. Roll back on any failure.
  SetMessage("Installing...", -1.0f);
  struct Moved {
    fs::path target, old;
    bool had_old;
  };
  std::vector<Moved> moved;
  auto rollback = [&] {
    for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
      std::error_code e;
      fs::remove_all(it->target, e);
      if (it->had_old) fs::rename(it->old, it->target, e);
    }
  };
  for (const auto& e : fs::directory_iterator(src, ec)) {
    const fs::path target = dir / e.path().filename();
    fs::path old = target;
    old += kOldSuffix;
    Moved m{target, old, false};
    if (fs::exists(target, ec)) {
      fs::remove_all(old, ec);  // leftover from an earlier update that could not be cleaned
      fs::rename(target, old, ec);
      if (ec) {
        rollback();
        *error = "Could not replace " + PathText(target) + ": " + ec.message();
        return false;
      }
      m.had_old = true;
    }
    fs::rename(e.path(), target, ec);
    if (ec) {
      if (m.had_old) {
        std::error_code e2;
        fs::rename(old, target, e2);
      }
      rollback();
      *error = "Could not install " + PathText(target) + ": " + ec.message();
      return false;
    }
    moved.push_back(m);
  }
  fs::remove_all(staging, ec);
  fs::remove(zip, ec);
  return true;
}

bool SelfUpdater::Restart(const std::wstring& args) {
  const fs::path exe = LauncherDir() / "openNG2.exe";
  std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, LauncherDir().c_str(), &si,
                      &pi)) {
    return false;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

void SelfUpdater::CleanupOldFiles() {
  std::thread([] {
    const fs::path dir = LauncherDir();
    std::error_code ec;
    fs::remove_all(dir / kStaging, ec);
    // The previous launcher may take a moment to exit and release openNG2.exe.old.
    for (int attempt = 0; attempt < 20; ++attempt) {
      bool left = false;
      for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() != kOldSuffix) continue;
        std::error_code e2;
        fs::remove_all(e.path(), e2);
        if (e2) left = true;
      }
      if (!left) return;
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }).detach();
}

}  // namespace launcher
