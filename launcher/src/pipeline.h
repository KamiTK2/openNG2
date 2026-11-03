// openNG2 launcher: the build pipeline (runs on a worker thread).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "inputs.h"

namespace launcher {

struct BuildRequest {
  Identified game;
  Identified update;
  std::vector<Identified> dlc;
  std::filesystem::path install_dir;
  std::filesystem::path payload_dir;  // shipped project + SDK (next to the launcher)
  // Update rebuild: game, title update and DLC are already in <install>\game (no inputs).
  bool rebuild = false;
};

enum class StepState { kPending, kRunning, kDone, kSkipped, kFailed };

struct Step {
  std::string title;
  StepState state = StepState::kPending;
  std::string detail;
  float progress = 0.0f;  // 0..1, < 0 = indeterminate
};

class Pipeline {
 public:
  Pipeline();
  ~Pipeline();
  void Start(const BuildRequest& request);
  void Cancel() { cancel_ = true; }
  bool cancelling() const { return cancel_ && running_; }
  // Back to the start (only when not running).
  void Reset();
  bool running() const { return running_; }
  bool finished() const { return finished_; }
  bool succeeded() const { return succeeded_; }

  // Snapshot for the UI.
  std::vector<Step> steps() const;
  std::vector<std::string> log_tail(size_t max_lines) const;
  std::string error() const;
  std::filesystem::path exe_path() const { return exe_path_; }
  std::filesystem::path log_path() const { return log_path_; }

  // Build tools live inside the install folder (the player picks the drive); the launcher's own
  // settings live in %LOCALAPPDATA%\openNG2.
  static std::filesystem::path ToolchainDir(const std::filesystem::path& install_dir);
  static std::filesystem::path AppDataDir();

  // Installed openNG2 version in that folder: "" = none, else from openNG2-install.ini (an
  // install without it predates the file: 1.0).
  static std::string InstalledVersion(const std::filesystem::path& install_dir);
  // True when <install>\game holds everything a rebuild needs (no files to add again).
  static bool CanRebuild(const std::filesystem::path& install_dir);
  // True while ng2.exe in that folder is running (its files cannot be replaced).
  static bool GameRunning(const std::filesystem::path& install_dir);

 private:
  void Run(BuildRequest request);
  bool Toolchain(const BuildRequest& r);
  bool Game(const BuildRequest& r);
  bool Update(const BuildRequest& r);
  bool Dlc(const BuildRequest& r);
  bool Build(const BuildRequest& r);
  bool Finish(const BuildRequest& r);
  bool EnsureVcRuntime(const BuildRequest& r);

  void Begin(size_t i);
  void SetDetail(size_t i, const std::string& detail, float progress);
  void End(size_t i, StepState state);
  void Log(const std::string& line);
  bool Fail(size_t i, const std::string& message);
  bool Cancelled() const { return cancel_; }

  mutable std::mutex mutex_;
  std::vector<Step> steps_;
  std::vector<std::string> log_;
  std::string error_;
  std::filesystem::path exe_path_;
  bool clean_retry_ = false;  // Build() is retrying from a clean build folder
  std::filesystem::path log_path_;
  std::thread thread_;
  std::atomic<bool> cancel_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> succeeded_{false};
  FILE* log_file_ = nullptr;
};

}  // namespace launcher
