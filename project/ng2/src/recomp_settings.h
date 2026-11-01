#pragma once
// RECOMP SETTINGS: user-facing settings for the NG2 recomp, shown inline in the game's
// Options menu (the "Recomp Settings" item that replaced Copyright Information).
//
// - RecompSettings: the model, persisted to <user data>\recomp_settings.toml (separate
//   from the shipped ng2.toml so rebuilds never overwrite user choices). Cvar-backed
//   values are plain cvar keys, applied at startup by the runtime's config loader;
//   ng2_* keys are app-side.
// - RecompOptionsPane: draws the settings in the Options right-hand pane in the game's
//   style (Category stepper + rows) and handles navigation.

#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <rex/ui/imgui_dialog.h>

namespace rex::ui {
class Window;
}
struct ImFontAtlas;

namespace ng2 {

void LoadRecompSettingsFonts(ImFontAtlas* atlas);

// True while the game should not see controller/keyboard input.
bool RecompSettingsBlocksGameInput();

// Install the controller state / vibration filters (deadzone, inversion, vibration).
void InstallRecompInputFilters();

// Called from the guest present hook (one call per game frame).
void RecompCountGameFrame();

}  // namespace ng2

// Queue one of the game's system sounds (ng2_hooks.cpp); played on the game thread from
// the Options menu update. 17 = menu cursor move.
inline constexpr uint32_t kSoundCursor = 17;
void Ng2QueueSystemSound(uint32_t id);

namespace ng2 {

struct RecompSettings {
  // Display
  bool fullscreen = true;
  bool vsync = true;
  int monitor = 0;  // 0 = default, 1.. = display index (applied live)
  // Windowed client size in physical pixels (0 = app default) and window position; the
  // size is picked by Window Size or by dragging, both are remembered across launches.
  int window_width = 0;
  int window_height = 0;
  int window_x = INT_MIN;  // INT_MIN = never saved
  int window_y = INT_MIN;
  // Graphics
  int resolution_scale = 1;  // 1..3 (restart)
  int anti_aliasing = 0;     // 0 off, 1 FXAA, 2 FXAA extreme (restart)
  // 30 = present every other frame (game still runs at 60), 60, 120 = frame generation.
  // Drives cvars present_half_rate / frame_generation (live).
  int framerate_cap = 60;
  // 0 = Auto (match the window), 1 = 16:9, 2 = 21:9, 3 = 32:9. Drives cvar ng2_aspect_ratio
  // (ultrawide: Hor+ camera, 2D kept at 16:9; live).
  int aspect_mode = 0;
  // Audio
  int master_volume = 100;  // 0..100
  bool mute_in_background = false;
  // Controls
  bool invert_camera_x = false;
  bool invert_camera_y = false;
  int stick_deadzone = 0;  // extra radial deadzone, percent 0..30
  int vibration = 100;     // percent 0..100
  // System
  bool pause_in_background = false;  // sends XN_SYS_UI (like the Guide button) on focus loss
  // Cheats (cvars ng2_cheat_*; apply after a restart, nothing is written to the game save)
  bool cheat_difficulties = false;
  bool cheat_costumes = false;

  bool Load(const std::filesystem::path& path);
  bool Save(const std::filesystem::path& path) const;
  static void ApplyStartup(const std::filesystem::path& path);
};

// Edge-triggered menu input from XInput pads and the keyboard (game window focused).
struct PadInput {
  bool up = false, down = false, left = false, right = false;  // with auto-repeat
  bool accept = false, back = false;                           // press edges
  bool any_held = false;
  void Poll();

 private:
  uint32_t prev_ = 0;
  int64_t repeat_at_ms_ = 0;
  bool primed_ = false;
};

class RecompOptionsPane : public rex::ui::ImGuiDialog {
 public:
  RecompOptionsPane(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                    std::filesystem::path settings_path, std::filesystem::path user_data_root,
                    std::function<bool()> pane_visible,
                    std::function<bool()> ingame_row_selected);
  ~RecompOptionsPane() override;

  // Draw only when the guest presents, so the host presents exactly once per game
  // frame (lets driver frame generation interpolate real frames).
  bool WantsContinuousPaint() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  void SyncFromRuntime();
  void ApplyAndSave();
  void LogFrameRate();
  void UpdateBackgroundMute();
  std::string BackUpSaves(const char* suffix = "");
  void RefreshBackups();
  void RestoreBackup(const std::string& name);
  void UpdateWindowGeometry(int64_t now);
  void ApplyWindowGeometry(bool with_position);

  rex::ui::Window* window_;
  std::filesystem::path settings_path_;
  std::filesystem::path user_data_root_;
  std::function<bool()> pane_visible_;
  // In-game menu: true while its cursor is on Recomp Settings; A opens the full-screen panel.
  std::function<bool()> ingame_row_selected_;
  bool overlay_open_ = false;
  RecompSettings settings_;
  RecompSettings startup_;  // values the running session started with (restart checks)
  PadInput pad_;
  int64_t pane_since_ms_ = 0;
  bool focused_ = false;
  int category_ = 0;
  int row_ = 0;
  bool muted_for_background_ = false;
  int applied_monitor_ = 0;  // display the window is currently placed on
  double applied_aspect_ = -1.0;  // ng2_aspect_ratio last set by Aspect Ratio
  std::string status_;
  int64_t status_until_ms_ = 0;
  // Window geometry tracking (windowed mode only).
  bool geometry_restored_ = false;
  bool pending_size_ = false;
  int64_t geometry_ready_at_ms_ = 0;
  int seen_x_ = INT_MIN, seen_y_ = INT_MIN, seen_w_ = 0, seen_h_ = 0;
  int64_t seen_at_ms_ = 0;
  // Save backups (newest first) for Restore Backup.
  std::vector<std::string> backups_;
  int backup_index_ = 0;
  int64_t restore_armed_until_ms_ = 0;
  // Pause in Background runs on its own thread: the game may stop presenting while
  // paused, and OnDraw only runs when the game presents.
  std::thread focus_thread_;
  std::atomic<bool> focus_thread_quit_{false};
};

}  // namespace ng2

