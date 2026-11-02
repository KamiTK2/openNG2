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

#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
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

// True while the player is in control in gameplay (ng2_hooks.cpp render pass).
bool Ng2InGameplay();
// NG2 DIAG: on-screen name of the F9 aspect test mode, or nullptr when off.
const char* Ng2AspectTestModeName();

namespace ng2 {

// Keyboard & Mouse bindings: one row per controller input (SDK cvar keybind_*), shown with
// NG2's action names. A value is a comma-separated list of key names (SDK keybinds.cpp).
struct BindDef {
  const char* cvar;    // SDK keybind cvar, or an ng2_bind_* settings key when app_buttons != 0
  const char* label;
  const char* def;
  uint16_t app_buttons = 0;  // app-side bind: XInput buttons it presses (Cast Ninpo = Y+B)
};
inline constexpr int kBindCount = 16;
// Bump when the default layout changes: older saved bindings are replaced by the defaults.
inline constexpr int kBindLayoutVersion = 2;
extern const BindDef kBindDefs[kBindCount];
std::array<std::string, kBindCount> DefaultBinds();

// Keyboard & mouse prompts (findings #54): the game's @[pad:NAME] tags are shown as the
// player's key bindings while the last input came from the keyboard or mouse.
bool PromptsUseKeyboard();
uint32_t PromptLabelsVersion();  // changes when bindings change
std::string PromptLabel(const std::string& tag, bool gameplay);  // "" = keep the icon
void UpdatePromptDevice(const ImGuiIO& io);
void PushPromptGlyph(float x0, float y0, float x1, float y1, const std::string& label);
void DrawPromptGlyphs(const ImGuiIO& io);

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
  int resolution_scale = 1;  // 1..5 (live: rebuilds the GPU context)
  int anti_aliasing = 0;     // 0 off, 1 FXAA, 2 FXAA extreme (live)
  // Scaling of the game image to the window: 0 bilinear, 1 AMD FSR 1.0 (EASU + RCAS),
  // 2 AMD CAS. Drives cvar present_effect (applied live through the presenter).
  int upscaler = 0;
  int sharpness = 8;  // 0..10 for FSR / CAS (present_fsr_sharpness_reduction / cas sharpness)
  // 30 = present every other frame (game still runs at 60), 60, 120 = frame generation.
  // Drives cvars present_half_rate / frame_generation (live).
  int framerate_cap = 60;
  // 0 = Auto (match the window), 1 = 16:9, 2 = 21:9, 3 = 32:9. Drives cvar ng2_aspect_ratio
  // (ultrawide: Hor+ camera, 2D kept at 16:9; live).
  int aspect_mode = 0;
  // Gameplay camera field of view, percent of the game's own (cvar ng2_fov_percent; live).
  int fov_percent = 100;
  // Audio
  int master_volume = 100;  // 0..100
  bool mute_in_background = false;
  // Controls
  bool invert_camera_x = false;
  bool invert_camera_y = false;
  int stick_deadzone = 0;  // extra radial deadzone, percent 0..30
  int vibration = 100;     // percent 0..100
  // Keyboard & Mouse (SDK MnK driver cvars; live)
  bool mnk_enabled = true;      // mnk_mode
  bool mouse_camera = true;     // mnk_mouse (camera from the mouse in gameplay)
  int mouse_sens = 10;          // 1..50 = 0.1..5.0 (mnk_sensitivity)
  int mouse_sens_y = 10;        // 1..50 (mnk_sensitivity_y)
  int aim_sens = 10;            // 1..50 = 0.1..5.0, projectile aim (ng2_bow_aim_gain)
  bool invert_mouse_y = false;  // mnk_invert_y
  bool mouse_auto_center = false;  // game swings the camera back behind Ryu between mouse moves
  std::array<std::string, kBindCount> binds = DefaultBinds();
  // System
  bool skip_any_cutscene = true;  // cvar ng2_skip_any_cutscene (live)
  bool fast_loading = true;  // cvar ng2_fast_loading: loading screens run unthrottled (live)
  bool skip_intro = false;  // skip the boot movie (cvar ng2_skip_intro; next launch)
  bool pause_in_background = false;  // sends XN_SYS_UI (like the Guide button) on focus loss
  bool check_updates = true;  // ask GitHub for a newer openNG2 release (update_check.h)
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

struct UpdateCheckState;  // recomp_settings.cpp

class RecompOptionsPane : public rex::ui::ImGuiDialog {
 public:
  RecompOptionsPane(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                    std::filesystem::path settings_path, std::filesystem::path user_data_root,
                    std::function<bool()> pane_visible,
                    std::function<bool()> ingame_row_selected);
  ~RecompOptionsPane() override;

  // Draw only when the guest presents, so the host presents exactly once per game
  // frame (lets driver frame generation interpolate real frames).
  // While shaders build before the game starts nothing presents, so the overlay paints itself.
  bool WantsContinuousPaint() const override;

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
  void StartUpdateCheck();
  void DrawUpdateNotice(ImGuiIO& io, int64_t now);
  void OpenUpdater();
  // Launcher finished building an update and waits for the game to close (issue #2).
  void PollUpdateReady(int64_t now);
  bool DrawUpdateReadyNotice(ImGuiIO& io, int64_t now);  // true when it drew (or owns the spot)
  void RestartForUpdate();
  void RestartAfterNextSave();
  int64_t LatestSaveWriteStamp() const;

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
  int scroll_ = 0;            // first visible row below Category (long lists scroll)
  int capture_bind_ = -1;     // binding row waiting for a key (-1 = none)
  bool capture_armed_ = false;  // all keys released since capture started
  int64_t capture_until_ms_ = 0;
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
  // Update check (System > Update): shared with its detached worker thread.
  std::shared_ptr<UpdateCheckState> update_;
  int64_t update_notice_until_ms_ = -1;  // 0 = hidden for good (gameplay was reached)
  std::string update_label_;  // row label storage for the current frame
  // Update ready (launcher waiting): version, or empty when no launcher is waiting.
  std::string ready_version_;
  int64_t ready_poll_ms_ = 0;
  bool ready_notice_hidden_ = false;      // LATER clicked (System > Update still offers it)
  int64_t ready_toast_until_ms_ = -1;     // gameplay toast (-1 = not shown yet)
  bool restart_after_save_ = false;
  int64_t save_baseline_ = 0;             // newest save write when "after next save" was chosen
  int64_t save_seen_ = 0;                 // newest save write seen since
  int64_t save_changed_at_ms_ = 0;        // when save_seen_ last changed (waits for writes to settle)
  bool restarting_ = false;
};

}  // namespace ng2

