// openNG2 launcher: checks GitHub for a newer openNG2 release and replaces itself with it.
//
// Install = download openNG2-<v>.zip (verified against GitHub's SHA-256 digest), extract it next
// to the launcher, rename the running launcher's files to *.old (a running .exe can be renamed,
// not overwritten), move the new ones in and restart. The *.old leftovers are removed on the
// next start. The game itself is then rebuilt from the files already in the install folder.
#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "update_check.h"

namespace launcher {

class SelfUpdater {
 public:
  enum class State { kIdle, kChecking, kUpToDate, kAvailable, kCheckFailed, kInstalling, kReady, kFailed };

  ~SelfUpdater();
  void Check();    // background
  void Install();  // background; only from kAvailable / kFailed
  State state() const { return state_; }
  openng2::ReleaseInfo release() const;
  std::string message() const;  // check error, install progress text or install error
  float progress() const { return progress_; }  // 0..1, < 0 = indeterminate
  bool busy() const { return state_ == State::kChecking || state_ == State::kInstalling; }

  // Starts the new launcher (with `args`); the caller then quits.
  static bool Restart(const std::wstring& args);
  // Removes *.old files left by the previous update (background, retries while they are locked).
  static void CleanupOldFiles();
  static std::filesystem::path LauncherDir();

 private:
  void Join();
  void SetMessage(const std::string& m, float progress);
  bool DoInstall(std::string* error);

  std::thread thread_;
  std::atomic<State> state_{State::kIdle};
  std::atomic<float> progress_{0.0f};
  mutable std::mutex mutex_;
  openng2::ReleaseInfo release_;
  std::string message_;
};

}  // namespace launcher
