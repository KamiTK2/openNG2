#include "recomp_settings.h"

// Must come before <xinput.h>: its enum names collide with XInput's macros.
#include <rex/input/input_filter.h>

#include <windows.h>
#include <shellapi.h>
#include <xinput.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/input/input_filter.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "shell32.lib")

bool Ng2IngameRecompRowSelected();  // ng2_hooks.cpp

namespace ng2 {
namespace {

// ---- helpers -----------------------------------------------------------------------

std::string Trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}
bool ParseBool(const std::string& v, bool fallback) {
  if (v == "true") return true;
  if (v == "false") return false;
  return fallback;
}
int ParseInt(const std::string& v, int fallback, int lo, int hi) {
  try {
    return std::clamp(std::stoi(v), lo, hi);
  } catch (...) {
    return fallback;
  }
}
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
// Target aspect for the Aspect Ratio setting; Auto follows the window (never below 16:9).
double AspectForMode(int mode, float width, float height) {
  switch (mode) {
    case 1: return 16.0 / 9.0;
    case 2: return 21.0 / 9.0;
    case 3: return 32.0 / 9.0;
    default:
      return height > 0.0f ? std::max(16.0 / 9.0, double(width) / double(height)) : 16.0 / 9.0;
  }
}
const char* AaCvarValue(int aa) { return aa == 2 ? "fxaa_extreme" : aa == 1 ? "fxaa" : "none"; }
int AaFromCvar(const std::string& v) { return v == "fxaa_extreme" ? 2 : v == "fxaa" ? 1 : 0; }

int MonitorCount() {
  int n = 0;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR, HDC, LPRECT, LPARAM p) -> BOOL {
        ++*reinterpret_cast<int*>(p);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&n));
  return std::max(n, 1);
}

constexpr int kBaseWidth = 1280;
constexpr int kBaseHeight = 720;

// ---- fonts -------------------------------------------------------------------------

ImFont* g_serif_font = nullptr;  // Georgia (matches the game's Options text)

// ---- shared state (pane <-> input/present hooks) -----------------------------------

std::atomic<bool> g_pane_focused{false};
std::atomic<bool> g_block_until_release{false};
std::atomic<bool> g_invert_x{false};
std::atomic<bool> g_invert_y{false};
std::atomic<int> g_deadzone_pct{0};
std::atomic<int> g_vibration_pct{100};
std::atomic<bool> g_pause_in_background{false};
std::atomic<uint64_t> g_game_frames{0};

void PublishCheats(const RecompSettings& s) {
  rex::cvar::SetFlagByName("ng2_cheat_difficulties", s.cheat_difficulties ? "true" : "false");
  rex::cvar::SetFlagByName("ng2_cheat_costumes", s.cheat_costumes ? "true" : "false");
  rex::cvar::SetFlagByName("ng2_skip_intro", s.skip_intro ? "true" : "false");
}

void PublishInputSettings(const RecompSettings& s) {
  g_invert_x = s.invert_camera_x;
  g_invert_y = s.invert_camera_y;
  g_deadzone_pct = s.stick_deadzone;
  g_vibration_pct = s.vibration;
  g_pause_in_background = s.pause_in_background;
}

// "2026-11-01_01-53-09[_before-restore]" -> "2026-11-01 01:53" (+ " (pre-restore)").
std::string BackupLabel(const std::string& name) {
  if (name.size() < 16 || name[10] != '_') return name;
  std::string out = name.substr(0, 10) + " " + name.substr(11, 2) + ":" + name.substr(14, 2);
  if (name.find("before-restore") != std::string::npos) out += " (pre-restore)";
  return out;
}

// Windowed presets for Window Size (client area, physical pixels).
struct SizePreset {
  int w, h;
};
constexpr SizePreset kWindowPresets[] = {{1280, 720},  {1600, 900},  {1920, 1080},
                                         {2560, 1080}, {2560, 1440}, {3440, 1440},
                                         {3840, 2160}};

// Work area of the monitor the window is on (falls back to the primary).
RECT WorkAreaFor(HWND h) {
  MONITORINFO mi{sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi.rcWork;
}

// Outer window size for a client size, with the window's current style and DPI.
SIZE OuterSizeFor(HWND h, int cw, int ch) {
  RECT r{0, 0, cw, ch};
  AdjustWindowRectExForDpi(&r, DWORD(GetWindowLongW(h, GWL_STYLE)), FALSE,
                           DWORD(GetWindowLongW(h, GWL_EXSTYLE)), GetDpiForWindow(h));
  return {r.right - r.left, r.bottom - r.top};
}

// The game's 16:9 frame inside the window (the presenter pillar/letterboxes it).
struct Viewport {
  float x, y, w, h;
};
Viewport GameViewport(const ImGuiIO& io) {
  // Always 16:9: on ultrawide the game's menus are kept at 16:9 in the centre (2D squeeze,
  // findings #32-#35), so the pane follows the same frame.
  const float aspect = 16.0f / 9.0f;
  float w = std::min(io.DisplaySize.x, io.DisplaySize.y * aspect);
  float h = w / aspect;
  return {(io.DisplaySize.x - w) * 0.5f, (io.DisplaySize.y - h) * 0.5f, w, h};
}

bool OurWindowFocused() {
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId();
}

// Radial deadzone with rescale: inputs inside `dz` become 0, the rest is stretched back
// to the full range so there is no jump at the edge.
void ApplyDeadzone(rex::be<int16_t>& x, rex::be<int16_t>& y, float dz) {
  float fx = float(int16_t(x)) / 32767.0f, fy = float(int16_t(y)) / 32767.0f;
  float mag = std::sqrt(fx * fx + fy * fy);
  if (mag <= dz) {
    x = int16_t(0);
    y = int16_t(0);
    return;
  }
  float scale = std::min(1.0f, (mag - dz) / (1.0f - dz)) / mag;
  x = int16_t(std::clamp(fx * scale * 32767.0f, -32767.0f, 32767.0f));
  y = int16_t(std::clamp(fy * scale * 32767.0f, -32767.0f, 32767.0f));
}

}  // namespace

// ---- input -------------------------------------------------------------------------

bool RecompSettingsBlocksGameInput() {
  return g_pane_focused.load() || g_block_until_release.load();
}

void InstallRecompInputFilters() {
  rex::input::SetStateFilter([](uint32_t, rex::input::X_INPUT_STATE* st) {
    auto& g = st->gamepad;
    // In-game menu, cursor on Recomp Settings (the old NINJA CINEMA row): Left/Right/A would
    // change the hidden NINJA CINEMA value; A opens our panel instead (read directly by
    // PadInput), so the game sees none of them.
    if (Ng2IngameRecompRowSelected()) {
      g.buttons = uint16_t(uint16_t(g.buttons) & ~(XINPUT_GAMEPAD_DPAD_LEFT |
                                                   XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A));
      g.thumb_lx = int16_t(0);
    }
    const int dz = g_deadzone_pct.load();
    if (dz > 0) {
      ApplyDeadzone(g.thumb_lx, g.thumb_ly, dz / 100.0f);
      ApplyDeadzone(g.thumb_rx, g.thumb_ry, dz / 100.0f);
    }
    // NG2's camera is on the right stick.
    if (g_invert_x) g.thumb_rx = int16_t(std::clamp(-int(int16_t(g.thumb_rx)), -32767, 32767));
    if (g_invert_y) g.thumb_ry = int16_t(std::clamp(-int(int16_t(g.thumb_ry)), -32767, 32767));
  });
  rex::input::SetVibrationFilter([](uint32_t, rex::input::X_INPUT_VIBRATION* v) {
    const int pct = g_vibration_pct.load();
    v->left_motor_speed = uint16_t(uint32_t(uint16_t(v->left_motor_speed)) * pct / 100);
    v->right_motor_speed = uint16_t(uint32_t(uint16_t(v->right_motor_speed)) * pct / 100);
  });
}

void RecompCountGameFrame() { g_game_frames.fetch_add(1, std::memory_order_relaxed); }

void PadInput::Poll() {
  enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kAccept = 16, kBack = 32 };
  uint32_t cur = 0;
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
    const auto& g = st.Gamepad;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > 16000) cur |= kUp;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -16000) cur |= kDown;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16000) cur |= kLeft;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16000) cur |= kRight;
    if (g.wButtons & XINPUT_GAMEPAD_A) cur |= kAccept;
    if (g.wButtons & XINPUT_GAMEPAD_B) cur |= kBack;
  }
  if (OurWindowFocused()) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (down(VK_UP)) cur |= kUp;
    if (down(VK_DOWN)) cur |= kDown;
    if (down(VK_LEFT)) cur |= kLeft;
    if (down(VK_RIGHT)) cur |= kRight;
    if (down(VK_RETURN) || down(VK_SPACE)) cur |= kAccept;
    if (down(VK_ESCAPE) || down(VK_BACK)) cur |= kBack;
  }
  const uint32_t pressed = primed_ ? (cur & ~prev_) : 0;
  const uint32_t dirs = kUp | kDown | kLeft | kRight;
  const int64_t now = NowMs();
  uint32_t fire = pressed;
  if (pressed & dirs) {
    repeat_at_ms_ = now + 350;
  } else if ((cur & dirs) && primed_ && now >= repeat_at_ms_) {
    fire |= cur & dirs;
    repeat_at_ms_ = now + 90;
  }
  up = fire & kUp;
  down = fire & kDown;
  left = fire & kLeft;
  right = fire & kRight;
  accept = pressed & kAccept;
  back = pressed & kBack;
  any_held = cur != 0;
  prev_ = cur;
  primed_ = true;
}

// ---- fonts -------------------------------------------------------------------------

void LoadRecompSettingsFonts(ImFontAtlas* atlas) {
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 2;
  const char* serif = "C:\\Windows\\Fonts\\georgia.ttf";
  if (std::filesystem::exists(serif)) g_serif_font = atlas->AddFontFromFileTTF(serif, 48.0f, &cfg);
}

// ---- model -------------------------------------------------------------------------

bool RecompSettings::Load(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    auto hash = line.find('#');
    if (hash != std::string::npos) line.resize(hash);
    auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = Trim(line.substr(0, eq));
    const std::string v = Trim(line.substr(eq + 1));
    if (k == "fullscreen") fullscreen = ParseBool(v, fullscreen);
    // `present_vsync` = host present sync. The old `vsync` key (guest vblank) is ignored.
    else if (k == "present_vsync") vsync = ParseBool(v, vsync);
    else if (k == "monitor") monitor = ParseInt(v, monitor, 0, 16);
    else if (k == "resolution_scale") resolution_scale = ParseInt(v, resolution_scale, 1, 3);
    else if (k == "swap_post_effect") anti_aliasing = AaFromCvar(v);
    else if (k == "ng2_aspect_mode") aspect_mode = ParseInt(v, aspect_mode, 0, 3);
    else if (k == "ng2_framerate_cap") {
      int cap = ParseInt(v, framerate_cap, 30, 120);
      framerate_cap = cap >= 120 ? 120 : cap <= 30 ? 30 : 60;
    }
    else if (k == "ng2_master_volume") master_volume = ParseInt(v, master_volume, 0, 100);
    else if (k == "ng2_mute_in_background") mute_in_background = ParseBool(v, mute_in_background);
    else if (k == "ng2_invert_camera_x") invert_camera_x = ParseBool(v, invert_camera_x);
    else if (k == "ng2_invert_camera_y") invert_camera_y = ParseBool(v, invert_camera_y);
    else if (k == "ng2_stick_deadzone") stick_deadzone = ParseInt(v, stick_deadzone, 0, 30);
    else if (k == "ng2_vibration") vibration = ParseInt(v, vibration, 0, 100);
    else if (k == "ng2_pause_in_background")
      pause_in_background = ParseBool(v, pause_in_background);
    else if (k == "ng2_window_width") window_width = ParseInt(v, window_width, 0, 16384);
    else if (k == "ng2_window_height") window_height = ParseInt(v, window_height, 0, 16384);
    else if (k == "ng2_window_x") window_x = ParseInt(v, window_x, -65536, 65536);
    else if (k == "ng2_window_y") window_y = ParseInt(v, window_y, -65536, 65536);
    else if (k == "ng2_cheat_difficulties") cheat_difficulties = ParseBool(v, cheat_difficulties);
    else if (k == "ng2_cheat_costumes") cheat_costumes = ParseBool(v, cheat_costumes);
    else if (k == "ng2_skip_intro") skip_intro = ParseBool(v, skip_intro);
  }
  return true;
}

bool RecompSettings::Save(const std::filesystem::path& path) const {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  auto b = [](bool v) { return v ? "true" : "false"; };
  out << "# RECOMP SETTINGS for the Ninja Gaiden II recomp. Written by the in-game menu.\n"
      << "# Plain keys are runtime cvars; ng2_* keys are read by the app itself.\n"
      << "fullscreen = " << b(fullscreen) << "\n"
      << "present_vsync = " << b(vsync) << "\n"
      << "monitor = " << monitor << "\n"
      << "resolution_scale = " << resolution_scale << "\n"
      << "swap_post_effect = \"" << AaCvarValue(anti_aliasing) << "\"\n"
      << "ng2_aspect_mode = " << aspect_mode << "\n"
      << "ng2_framerate_cap = " << framerate_cap << "\n"
      << "frame_generation = " << b(framerate_cap == 120) << "\n"
      << "present_half_rate = " << b(framerate_cap == 30) << "\n"
      << "ng2_master_volume = " << master_volume << "\n"
      << "ng2_mute_in_background = " << b(mute_in_background) << "\n"
      << "ng2_invert_camera_x = " << b(invert_camera_x) << "\n"
      << "ng2_invert_camera_y = " << b(invert_camera_y) << "\n"
      << "ng2_stick_deadzone = " << stick_deadzone << "\n"
      << "ng2_vibration = " << vibration << "\n"
      << "ng2_pause_in_background = " << b(pause_in_background) << "\n"
      << "ng2_window_width = " << window_width << "\n"
      << "ng2_window_height = " << window_height << "\n";
  if (window_x != INT_MIN && window_y != INT_MIN) {
    out << "ng2_window_x = " << window_x << "\n"
        << "ng2_window_y = " << window_y << "\n";
  }
  out
      << "ng2_cheat_difficulties = " << b(cheat_difficulties) << "\n"
      << "ng2_cheat_costumes = " << b(cheat_costumes) << "\n"
      << "ng2_skip_intro = " << b(skip_intro) << "\n";
  return bool(out);
}

void RecompSettings::ApplyStartup(const std::filesystem::path& path) {
  RecompSettings s;
  if (!s.Load(path)) return;
  rex::cvar::LoadConfig(path);  // cvar keys (handles plugin cvars registered later)
  // The guest `vsync` cvar is the emulated 60 Hz vblank that paces NG2's fixed-step
  // simulation; off makes the game run 2-6x too fast (findings #28). Older settings files
  // wrote it from the VSync row, so force it on; VSync now maps to present_vsync.
  rex::cvar::SetFlagByName("vsync", "true");
  rex::cvar::SetFlagByName("present_vsync", s.vsync ? "true" : "false");
  // Framerate Cap owns these (older files wrote frame_generation from its own row).
  rex::cvar::SetFlagByName("frame_generation", s.framerate_cap == 120 ? "true" : "false");
  rex::cvar::SetFlagByName("present_half_rate", s.framerate_cap == 30 ? "true" : "false");
  rex::audio::SetOutputGain(s.master_volume / 100.0f);
  PublishInputSettings(s);
  PublishCheats(s);
  REXLOG_INFO("RECOMP SETTINGS loaded from {}", path.string());
}

// ---- Options > Recomp Settings pane ---------------------------------------------------

namespace {

// Row layout as fractions of the 16:9 frame.
struct Layout {
  float label_x, first_row_y, row_step;
  float pair_left_x, pair_slash_x, pair_right_x;
  float arrow_left_x, value_x, arrow_right_x;
};
// Main menu: the Options right-hand pane (measured from the Game Settings pane).
constexpr Layout kPaneLayout = {0.357f, 0.200f, 0.061f, 0.641f, 0.716f,
                                0.786f, 0.646f, 0.714f, 0.785f};
// In-game menu: full-screen panel with the in-game Options list's spacing.
constexpr Layout kOverlayLayout = {0.089f, 0.257f, 0.057f, 0.425f, 0.500f,
                                   0.575f, 0.430f, 0.500f, 0.570f};

const ImU32 kWhite = IM_COL32(238, 234, 228, 255);
const ImU32 kGrey = IM_COL32(176, 170, 164, 255);
const ImU32 kDisabled = IM_COL32(110, 106, 100, 255);
const ImU32 kShadow = IM_COL32(0, 0, 0, 150);
const ImU32 kRedMarker = IM_COL32(200, 24, 16, 255);

const char* kCategories[] = {"Display", "Graphics", "Audio", "Controls", "System", "Cheats"};
constexpr int kCategoryCount = 6;

enum class Kind { kStepper, kPair, kAction };
struct RowDef {
  const char* label;
  Kind kind;
  std::string value;          // stepper value
  bool can_dec = true, can_inc = true;
  const char* a = nullptr;    // pair choices
  const char* b = nullptr;
  bool a_selected = false;
  std::function<bool(int dir, bool accept)> change;  // returns true if settings changed
  bool disabled = false;  // greyed out and skipped by the cursor
};

}  // namespace

RecompOptionsPane::RecompOptionsPane(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                                     std::filesystem::path settings_path,
                                     std::filesystem::path user_data_root,
                                     std::function<bool()> pane_visible,
                                     std::function<bool()> ingame_row_selected)
    : ImGuiDialog(drawer),
      window_(window),
      settings_path_(std::move(settings_path)),
      user_data_root_(std::move(user_data_root)),
      pane_visible_(std::move(pane_visible)),
      ingame_row_selected_(std::move(ingame_row_selected)) {
  settings_.Load(settings_path_);
  // Restart-only settings: what this session actually runs with.
  settings_.resolution_scale =
      std::clamp(std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()), 1, 3);
  settings_.monitor = std::clamp(std::atoi(rex::cvar::GetFlagByName("monitor").c_str()), 0, 16);
  settings_.anti_aliasing = AaFromCvar(rex::cvar::GetFlagByName("swap_post_effect"));
  startup_ = settings_;
  applied_monitor_ = settings_.monitor;
  RefreshBackups();
  // Pause in Background: XN_SYS_UI is the notification the console sends when the Guide
  // opens; the game pauses itself as it would on a console.
  focus_thread_ = std::thread([this] {
    bool paused = false;
    while (!focus_thread_quit_) {
      const bool want = g_pause_in_background && !OurWindowFocused();
      if (want != paused) {
        if (auto* ks = REX_KERNEL_STATE()) {
          paused = want;
          ks->BroadcastNotification(0x9 /* XN_SYS_UI */, want ? 1 : 0);
          REXLOG_INFO("RECOMP SETTINGS: window {} focus, XN_SYS_UI = {}",
                      want ? "lost" : "regained", want);
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
}

RecompOptionsPane::~RecompOptionsPane() {
  g_pane_focused = false;
  focus_thread_quit_ = true;
  if (focus_thread_.joinable()) focus_thread_.join();
}

void RecompOptionsPane::SyncFromRuntime() {
  if (window_) settings_.fullscreen = window_->IsFullscreen();
  std::string vsync = rex::cvar::GetFlagByName("present_vsync");
  if (!vsync.empty()) settings_.vsync = vsync == "true" || vsync == "1";
}

void RecompOptionsPane::ApplyAndSave() {
  if (window_ && window_->IsFullscreen() != settings_.fullscreen) {
    window_->SetFullscreen(settings_.fullscreen);
  }
  rex::cvar::SetFlagByName("present_vsync", settings_.vsync ? "true" : "false");
  rex::cvar::SetFlagByName("fullscreen", settings_.fullscreen ? "true" : "false");
  rex::cvar::SetFlagByName("resolution_scale", std::to_string(settings_.resolution_scale));
  rex::cvar::SetFlagByName("monitor", std::to_string(settings_.monitor));
  if (window_ && settings_.monitor != applied_monitor_) {
    window_->MoveToMonitor(settings_.monitor);  // live: no restart needed
    applied_monitor_ = settings_.monitor;
  }
  rex::cvar::SetFlagByName("swap_post_effect", AaCvarValue(settings_.anti_aliasing));
  rex::cvar::SetFlagByName("frame_generation", settings_.framerate_cap == 120 ? "true" : "false");
  rex::cvar::SetFlagByName("present_half_rate", settings_.framerate_cap == 30 ? "true" : "false");
  if (!muted_for_background_) rex::audio::SetOutputGain(settings_.master_volume / 100.0f);
  PublishInputSettings(settings_);
  PublishCheats(settings_);
  if (!settings_.Save(settings_path_)) {
    REXLOG_WARN("RECOMP SETTINGS: failed to write {}", settings_path_.string());
  }
}

void RecompOptionsPane::UpdateBackgroundMute() {
  const bool want_mute = settings_.mute_in_background && !OurWindowFocused();
  if (want_mute != muted_for_background_) {
    muted_for_background_ = want_mute;
    rex::audio::SetOutputGain(want_mute ? 0.0f : settings_.master_volume / 100.0f);
  }
}

std::string RecompOptionsPane::BackUpSaves(const char* suffix) {
  char stamp[48];
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_s(&tm, &t);
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm);
  strncat_s(stamp, suffix, _TRUNCATE);
  std::error_code ec;
  const auto dst = user_data_root_ / "save_backups" / stamp;
  std::filesystem::create_directories(dst, ec);
  int copied = 0;
  for (const auto& entry : std::filesystem::directory_iterator(user_data_root_, ec)) {
    // Profile/save folders are the 16-hex-digit profile IDs (e.g. B13EBABEBABEBABE).
    const auto name = entry.path().filename().string();
    if (!entry.is_directory() || name.size() != 16) continue;
    std::filesystem::copy(entry.path(), dst / name,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (!ec) ++copied;
  }
  if (!copied) std::filesystem::remove(dst, ec);
  status_ = copied ? std::string("Saves backed up to save_backups\\") + stamp
                   : std::string("No saves found to back up.");
  status_until_ms_ = NowMs() + 4000;
  REXLOG_INFO("RECOMP SETTINGS: {}", status_);
  RefreshBackups();
  return copied ? std::string(stamp) : std::string();
}

void RecompOptionsPane::RefreshBackups() {
  backups_.clear();
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(user_data_root_ / "save_backups", ec)) {
    if (e.is_directory()) backups_.push_back(e.path().filename().string());
  }
  std::sort(backups_.rbegin(), backups_.rend());  // timestamps: newest first
  backup_index_ = std::clamp(backup_index_, 0, std::max(0, int(backups_.size()) - 1));
}

void RecompOptionsPane::RestoreBackup(const std::string& name) {
  const auto src = user_data_root_ / "save_backups" / name;
  std::error_code ec;
  if (!std::filesystem::is_directory(src, ec)) {
    status_ = "That backup no longer exists.";
    status_until_ms_ = NowMs() + 4000;
    RefreshBackups();
    return;
  }
  // Keep what is there now, so a restore can itself be undone.
  BackUpSaves("_before-restore");
  int restored = 0;
  for (const auto& e : std::filesystem::directory_iterator(src, ec)) {
    if (!e.is_directory()) continue;
    std::error_code cec;
    std::filesystem::copy(e.path(), user_data_root_ / e.path().filename(),
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          cec);
    if (cec) {
      REXLOG_WARN("RECOMP SETTINGS: restore of {} failed: {}", e.path().string(), cec.message());
    } else {
      ++restored;
    }
  }
  status_ = restored ? "Restored " + name + ". Load it from the title screen."
                     : std::string("Nothing was restored.");
  status_until_ms_ = NowMs() + 6000;
  REXLOG_INFO("RECOMP SETTINGS: {}", status_);
}

// Windowed mode: put the window back where it was last time, apply Window Size, and
// remember moves/resizes the player makes (saved after the window settles for 1 s).
void RecompOptionsPane::UpdateWindowGeometry(int64_t now) {
  HWND h = window_ ? static_cast<HWND>(window_->GetNativeWindowHandle()) : nullptr;
  if (!h) return;
  if (window_->IsFullscreen() || IsIconic(h) || IsZoomed(h)) {
    geometry_ready_at_ms_ = now + 500;  // let the window settle after leaving fullscreen
    return;
  }
  if (now < geometry_ready_at_ms_) return;
  if (!geometry_restored_) {
    geometry_restored_ = true;
    ApplyWindowGeometry(true);
    return;
  }
  if (pending_size_) {
    pending_size_ = false;
    ApplyWindowGeometry(false);
    return;
  }
  RECT wr, cr;
  if (!GetWindowRect(h, &wr) || !GetClientRect(h, &cr)) return;
  const int x = wr.left, y = wr.top, w = cr.right - cr.left, hh = cr.bottom - cr.top;
  if (w <= 0 || hh <= 0) return;
  if (x != seen_x_ || y != seen_y_ || w != seen_w_ || hh != seen_h_) {
    seen_x_ = x, seen_y_ = y, seen_w_ = w, seen_h_ = hh;
    seen_at_ms_ = now;
    return;
  }
  auto& s = settings_;
  if (now - seen_at_ms_ >= 1000 &&
      (s.window_x != x || s.window_y != y || s.window_width != w || s.window_height != hh)) {
    s.window_x = x, s.window_y = y, s.window_width = w, s.window_height = hh;
    s.Save(settings_path_);
  }
}

void RecompOptionsPane::ApplyWindowGeometry(bool with_position) {
  HWND h = window_ ? static_cast<HWND>(window_->GetNativeWindowHandle()) : nullptr;
  if (!h) return;
  const auto& s = settings_;
  RECT wr;
  GetWindowRect(h, &wr);
  SIZE outer{wr.right - wr.left, wr.bottom - wr.top};
  const bool sized = s.window_width > 0 && s.window_height > 0;
  if (sized) outer = OuterSizeFor(h, s.window_width, s.window_height);
  int x = wr.left + ((wr.right - wr.left) - outer.cx) / 2;  // keep the centre
  int y = wr.top + ((wr.bottom - wr.top) - outer.cy) / 2;
  bool placed = false;
  if (with_position && s.window_x != INT_MIN && s.window_y != INT_MIN) {
    // Only if the saved spot is still on a connected monitor.
    RECT saved{s.window_x, s.window_y, s.window_x + outer.cx, s.window_y + outer.cy};
    if (MonitorFromRect(&saved, MONITOR_DEFAULTTONULL)) {
      x = s.window_x, y = s.window_y;
      placed = true;
    }
  }
  if (!placed) {
    const RECT work = WorkAreaFor(h);
    x = std::clamp(x, int(work.left), std::max(int(work.left), int(work.right) - int(outer.cx)));
    y = std::clamp(y, int(work.top), std::max(int(work.top), int(work.bottom) - int(outer.cy)));
  }
  if (!sized && !placed) return;
  SetWindowPos(h, nullptr, x, y, outer.cx, outer.cy,
               SWP_NOZORDER | SWP_NOACTIVATE | (sized ? 0 : SWP_NOSIZE));
}

// Logs the game's frame rate and the host present rate every 5 s (used by test runs; the
// on-screen counter was removed: the game always runs at 60).
void RecompOptionsPane::LogFrameRate() {
  static uint64_t last_frames = 0;
  static int64_t last_ms = 0;
  static float fps = 0.0f;
  static uint64_t host_draws = 0, last_host_draws = 0;
  static float host_fps = 0.0f;
  ++host_draws;
  const int64_t now = NowMs();
  if (!last_ms) last_ms = now;
  if (now - last_ms >= 500) {
    const uint64_t frames = g_game_frames.load();
    fps = float(frames - last_frames) * 1000.0f / float(now - last_ms);
    host_fps = float(host_draws - last_host_draws) * 1000.0f / float(now - last_ms);
    last_host_draws = host_draws;
    last_frames = frames;
    last_ms = now;
    static int64_t last_log_ms = 0;
    if (now - last_log_ms >= 5000) {
      last_log_ms = now;
      REXLOG_INFO("NG2 game fps {:.1f} (frames {}), host ui/present fps {:.1f}", fps, frames,
                  host_fps);
    }
  }
}

void RecompOptionsPane::OnDraw(ImGuiIO& io) {
  pad_.Poll();
  {
    // Aspect Ratio (Auto re-evaluates when the window changes shape).
    const double want = AspectForMode(settings_.aspect_mode, io.DisplaySize.x, io.DisplaySize.y);
    const double wide = want > 16.0 / 9.0 + 0.01 ? want : 0.0;
    if (std::abs(wide - applied_aspect_) > 1e-3) {
      applied_aspect_ = wide;
      rex::cvar::SetFlagByName("ng2_aspect_ratio", std::to_string(wide));
    }
  }
  if (g_block_until_release && !pad_.any_held && !focused_) g_block_until_release = false;
  UpdateBackgroundMute();
  LogFrameRate();
  UpdateWindowGeometry(NowMs());

  // In-game menu: A on Recomp Settings opens the full-screen panel.
  if (!overlay_open_ && ingame_row_selected_ && ingame_row_selected_() && pad_.accept) {
    overlay_open_ = true;
    focused_ = true;
    g_pane_focused = true;
    row_ = 0;
    pane_since_ms_ = 0;
    pad_.accept = false;  // this press only opens the panel
    Ng2QueueSystemSound(kSoundCursor);
  }
  if (!overlay_open_ && !pane_visible_()) {
    pane_since_ms_ = 0;
    if (focused_) {
      focused_ = false;
      g_pane_focused = false;
    }
    return;
  }
  const int64_t now = NowMs();
  if (!pane_since_ms_) {
    pane_since_ms_ = now;
    SyncFromRuntime();
    RefreshBackups();
  }

  // ---- rows for the current category -------------------------------------------------
  auto& s = settings_;
  char buf[64];
  std::vector<RowDef> rows;
  rows.push_back({"Category", Kind::kStepper, kCategories[category_], category_ > 0,
                  category_ < kCategoryCount - 1, nullptr, nullptr, false,
                  [this](int dir, bool) {
                    category_ = std::clamp(category_ + dir, 0, kCategoryCount - 1);
                    return false;
                  }});
  auto pair_row = [&](const char* label, const char* a, const char* b, bool a_sel,
                      std::function<void(bool a_chosen)> set) {
    rows.push_back({label, Kind::kPair, "", true, true, a, b, a_sel,
                    [a_sel, set](int dir, bool accept) {
                      if (!dir && !accept) return false;
                      set(dir ? dir < 0 : !a_sel);  // Left picks the first choice
                      return true;
                    }});
  };
  auto step_row = [&](const char* label, std::string value, int cur, int lo, int hi, int step,
                      std::function<void(int)> set) {
    rows.push_back({label, Kind::kStepper, std::move(value), cur > lo, cur < hi, nullptr, nullptr,
                    false, [cur, lo, hi, step, set](int dir, bool) {
                      if (!dir) return false;
                      int v = std::clamp(cur + dir * step, lo, hi);
                      if (v == cur) return false;
                      set(v);
                      return true;
                    }});
  };
  auto action_row = [&](const char* label, std::function<bool()> act) {
    rows.push_back({label, Kind::kAction, "", true, true, nullptr, nullptr, false,
                    [act](int, bool accept) { return accept ? act() : false; }});
  };

  switch (category_) {
    case 0: {  // Display
      pair_row("Display Mode", "Windowed", "Fullscreen", !s.fullscreen,
               [&s](bool a) { s.fullscreen = !a; });
      pair_row("VSync", "On", "Off", s.vsync, [&s](bool a) { s.vsync = a; });
      const int monitors = MonitorCount();
      std::snprintf(buf, sizeof(buf), s.monitor == 0 ? "Default" : "Display %d", s.monitor);
      step_row("Monitor", buf, s.monitor, 0, monitors, 1, [&s](int v) { s.monitor = v; });
      const char* aspects[] = {"Auto", "16:9", "21:9", "32:9"};
      step_row("Aspect Ratio", aspects[s.aspect_mode], s.aspect_mode, 0, 3, 1,
               [&s](int v) { s.aspect_mode = v; });
      {
        // Window Size: presets that fit this monitor; a dragged size shows as-is.
        std::vector<SizePreset> fit;
        HWND h = window_ ? static_cast<HWND>(window_->GetNativeWindowHandle()) : nullptr;
        const RECT work = h ? WorkAreaFor(h) : RECT{0, 0, 1 << 15, 1 << 15};
        for (const auto& p : kWindowPresets) {
          const SIZE o = h ? OuterSizeFor(h, p.w, p.h) : SIZE{p.w, p.h};
          if (o.cx <= work.right - work.left && o.cy <= work.bottom - work.top) fit.push_back(p);
        }
        if (fit.empty()) fit.push_back(kWindowPresets[0]);
        const int cur_w = s.window_width, cur_h = s.window_height;
        const int64_t cur_key = int64_t(cur_w) * 100000 + cur_h;
        auto key = [](const SizePreset& p) { return int64_t(p.w) * 100000 + p.h; };
        int lower = -1, higher = -1;
        for (int i = 0; i < int(fit.size()); ++i) {
          if (key(fit[i]) < cur_key) lower = i;
          if (key(fit[i]) > cur_key && higher < 0) higher = i;
        }
        if (cur_w > 0) std::snprintf(buf, sizeof(buf), "%d x %d", cur_w, cur_h);
        else std::snprintf(buf, sizeof(buf), "Default");
        rows.push_back({"Window Size", Kind::kStepper, buf, lower >= 0, higher >= 0, nullptr,
                        nullptr, false, [this, fit, lower, higher](int dir, bool) {
                          const int i = dir < 0 ? lower : dir > 0 ? higher : -1;
                          if (i < 0) return false;
                          settings_.window_width = fit[i].w;
                          settings_.window_height = fit[i].h;
                          pending_size_ = true;
                          return true;
                        }});
        // Only meaningful in a window: greyed out and skipped while fullscreen.
        if (s.fullscreen) {
          rows.back().disabled = true;
          rows.back().can_dec = rows.back().can_inc = false;
          rows.back().change = nullptr;
        }
      }
      break;
    }
    case 1: {  // Graphics
      std::snprintf(buf, sizeof(buf), "%d x %d", kBaseWidth * s.resolution_scale,
                    kBaseHeight * s.resolution_scale);
      step_row("Internal Resolution", buf, s.resolution_scale, 1, 3, 1,
               [&s](int v) { s.resolution_scale = v; });
      const char* aa[] = {"Off", "FXAA", "FXAA Extreme"};
      step_row("Anti-Aliasing", aa[s.anti_aliasing], s.anti_aliasing, 0, 2, 1,
               [&s](int v) { s.anti_aliasing = v; });
      {
        const int caps[] = {30, 60, 120};
        const char* cap_names[] = {"30 FPS", "60 FPS", "120 FPS (Interpolation)"};
        const int ci = s.framerate_cap == 30 ? 0 : s.framerate_cap == 120 ? 2 : 1;
        step_row("Framerate Cap", cap_names[ci], ci, 0, 2, 1,
                 [&s, caps](int v) { s.framerate_cap = caps[v]; });
      }
      break;
    }
    case 2: {  // Audio
      std::snprintf(buf, sizeof(buf), "%d", s.master_volume);
      step_row("Master Volume", buf, s.master_volume, 0, 100, 5,
               [&s](int v) { s.master_volume = v; });
      pair_row("Mute in Background", "On", "Off", s.mute_in_background,
               [&s](bool a) { s.mute_in_background = a; });
      break;
    }
    case 3: {  // Controls
      pair_row("Invert Camera X", "On", "Off", s.invert_camera_x,
               [&s](bool a) { s.invert_camera_x = a; });
      pair_row("Invert Camera Y", "On", "Off", s.invert_camera_y,
               [&s](bool a) { s.invert_camera_y = a; });
      std::snprintf(buf, sizeof(buf), "%d%%", s.stick_deadzone);
      step_row("Stick Deadzone", buf, s.stick_deadzone, 0, 30, 5,
               [&s](int v) { s.stick_deadzone = v; });
      if (s.vibration) std::snprintf(buf, sizeof(buf), "%d%%", s.vibration);
      else std::snprintf(buf, sizeof(buf), "Off");
      step_row("Vibration", buf, s.vibration, 0, 100, 10, [&s](int v) { s.vibration = v; });
      break;
    }
    case 5: {  // Cheats
      pair_row("Unlock Difficulties", "On", "Off", s.cheat_difficulties,
               [&s](bool a) { s.cheat_difficulties = a; });
      pair_row("Unlock Costumes", "On", "Off", s.cheat_costumes,
               [&s](bool a) { s.cheat_costumes = a; });
      break;
    }
    case 4: {  // System
      pair_row("Skip Intro", "On", "Off", s.skip_intro, [&s](bool a) { s.skip_intro = a; });
      pair_row("Pause in Background", "On", "Off", s.pause_in_background,
               [&s](bool a) { s.pause_in_background = a; });
      action_row("Open Save Folder", [this] {
        ShellExecuteW(nullptr, L"open", user_data_root_.wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return false;
      });
      action_row("Back Up Saves", [this] {
        BackUpSaves();
        return false;
      });
      {
        // Restore Backup: Left/Right picks a backup, A twice restores it.
        const int n = int(backups_.size());
        rows.push_back({"Restore Backup", Kind::kStepper, n ? BackupLabel(backups_[backup_index_]) : "None",
                        backup_index_ > 0, backup_index_ < n - 1, nullptr, nullptr, false,
                        [this, n](int dir, bool accept) {
                          if (dir) {
                            backup_index_ = std::clamp(backup_index_ + dir, 0, std::max(0, n - 1));
                            restore_armed_until_ms_ = 0;
                            return false;
                          }
                          if (!accept || !n) return false;
                          const std::string name = backups_[backup_index_];
                          if (NowMs() < restore_armed_until_ms_) {
                            restore_armed_until_ms_ = 0;
                            RestoreBackup(name);
                          } else {
                            restore_armed_until_ms_ = NowMs() + 4000;
                            status_ = "Press A again to restore " + name +
                                      " (current saves are backed up first).";
                            status_until_ms_ = restore_armed_until_ms_;
                          }
                          return false;
                        }});
      }
      break;
    }
  }
  action_row("Restore Defaults", [this] {
    const RecompSettings old = settings_;
    settings_ = RecompSettings{};
    settings_.window_width = old.window_width;  // geometry is remembered, not a preference
    settings_.window_height = old.window_height;
    settings_.window_x = old.window_x;
    settings_.window_y = old.window_y;
    return true;
  });

  // ---- input -----------------------------------------------------------------------
  bool changed = false;
  if (!focused_) {
    // Only accept an A that starts after the pane has been showing for a moment, so the
    // press that opens Options (with its cursor on this item) doesn't also enter it.
    if (pad_.accept && now - pane_since_ms_ > 250) {
      focused_ = true;
      g_pane_focused = true;
      row_ = 0;
    }
  } else {
    const int n = int(rows.size());
    row_ = std::clamp(row_, 0, n - 1);
    const int old_row = row_, old_category = category_;
    // Up/Down skip disabled rows (row 0, Category, is never disabled).
    if (pad_.up) do row_ = (row_ + n - 1) % n; while (rows[row_].disabled);
    if (pad_.down) do row_ = (row_ + 1) % n; while (rows[row_].disabled);
    if (rows[row_].disabled) row_ = 0;
    const int dir = pad_.right ? 1 : (pad_.left ? -1 : 0);
    if ((dir || pad_.accept) && rows[row_].change) changed = rows[row_].change(dir, pad_.accept);
    // The game's own cursor sound for row moves and value changes.
    if (row_ != old_row || category_ != old_category || changed) Ng2QueueSystemSound(kSoundCursor);
    if (pad_.back) {
      focused_ = false;
      g_pane_focused = false;
      g_block_until_release = true;  // don't let this B reach the game
      if (overlay_open_) {
        overlay_open_ = false;  // back to the in-game list
        Ng2QueueSystemSound(kSoundCursor);
      }
    }
  }
  if (changed) ApplyAndSave();
  if (!overlay_open_ && !pane_visible_()) return;

  // ---- drawing -----------------------------------------------------------------------
  const Layout& L = overlay_open_ ? kOverlayLayout : kPaneLayout;
  const Viewport vp = GameViewport(io);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = g_serif_font ? g_serif_font : ImGui::GetFont();
  const float px = vp.h * 0.036f;
  auto X = [&](float fx) { return vp.x + vp.w * fx; };
  auto Y = [&](int row) { return vp.y + vp.h * (L.first_row_y + L.row_step * row); };
  auto text_at = [&](float x, float y, ImU32 col, const char* t, bool centered, float size) {
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
    float tx = centered ? x - sz.x * 0.5f : x;
    float ty = y - sz.y * 0.5f;
    dl->AddText(font, size, ImVec2(tx + size * 0.05f, ty + size * 0.05f), kShadow, t);
    dl->AddText(font, size, ImVec2(tx, ty), col, t);
  };
  // Red brush stroke behind the selected choice (approximates the game's blood smear).
  auto brush = [&](float cx, float cy, float w) {
    const float h = px * 0.95f;
    for (int i = 0; i < 4; ++i) {
      float jx = (i - 1.5f) * px * 0.08f, jy = ((i * 37) % 5 - 2) * px * 0.04f;
      dl->AddRectFilled(ImVec2(cx - w * 0.5f - px * 0.25f + jx, cy - h * 0.45f + jy),
                        ImVec2(cx + w * 0.5f + px * 0.35f + jx, cy + h * 0.40f + jy),
                        IM_COL32(120 + i * 12, 8, 6, 70), h * 0.45f);
    }
  };

  if (overlay_open_) {
    // Full-screen panel over the in-game menu, titled like the game's screens.
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize, IM_COL32(10, 5, 12, 238));
    text_at(X(0.127f), vp.y + vp.h * 0.100f, kWhite, "RECOMP SETTINGS", false, vp.h * 0.05f);
    dl->AddLine(ImVec2(X(0.127f), vp.y + vp.h * 0.135f), ImVec2(X(0.930f), vp.y + vp.h * 0.135f),
                IM_COL32(150, 140, 150, 160), std::max(1.0f, vp.h * 0.002f));
    const float hx = X(0.865f), hy = vp.y + vp.h * 0.853f;
    dl->AddCircleFilled(ImVec2(hx, hy), px * 0.45f, IM_COL32(176, 22, 18, 255));
    text_at(hx, hy, kWhite, "B", true, px * 0.62f);
    text_at(hx + px * 0.75f, hy, kWhite, "Back", false, px);
  }

  // The row list can change after input (category switch), so rebuild-free drawing uses
  // the category that was current when the rows were built; a switch shows next frame.
  for (int i = 0; i < int(rows.size()); ++i) {
    const RowDef& r = rows[i];
    const float y = Y(i);
    const bool on = focused_ && row_ == i;
    if (on) {
      const float m = px * 0.18f, cx = X(L.label_x) - px * 0.55f;
      dl->AddQuadFilled(ImVec2(cx, y - m), ImVec2(cx + m, y), ImVec2(cx, y + m),
                        ImVec2(cx - m, y), kRedMarker);
    }
    text_at(X(L.label_x), y, r.disabled ? kDisabled : on ? kWhite : kGrey, r.label, false, px);
    if (r.kind == Kind::kPair) {
      ImVec2 sa = font->CalcTextSizeA(px, FLT_MAX, 0.0f, r.a);
      ImVec2 sb = font->CalcTextSizeA(px, FLT_MAX, 0.0f, r.b);
      if (r.a_selected) brush(X(L.pair_left_x), y, sa.x);
      else brush(X(L.pair_right_x), y, sb.x);
      text_at(X(L.pair_left_x), y, r.a_selected ? kWhite : kGrey, r.a, true, px);
      text_at(X(L.pair_slash_x), y, kGrey, "/", true, px);
      text_at(X(L.pair_right_x), y, r.a_selected ? kGrey : kWhite, r.b, true, px);
    } else if (r.kind == Kind::kStepper) {
      const float t = px * 0.30f;
      dl->AddTriangleFilled(ImVec2(X(L.arrow_left_x) - t, y), ImVec2(X(L.arrow_left_x) + t * 0.8f, y - t),
                            ImVec2(X(L.arrow_left_x) + t * 0.8f, y + t),
                            r.can_dec ? kWhite : kDisabled);
      dl->AddTriangleFilled(ImVec2(X(L.arrow_right_x) + t, y),
                            ImVec2(X(L.arrow_right_x) - t * 0.8f, y - t),
                            ImVec2(X(L.arrow_right_x) - t * 0.8f, y + t),
                            r.can_inc ? kWhite : kDisabled);
      // Shrink long values (e.g. "120 FPS (Interpolation)") to fit between the arrows.
      const float room = X(L.arrow_right_x) - X(L.arrow_left_x) - t * 3.0f;
      const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, r.value.c_str()).x;
      const float vpx = w > room ? std::max(px * room / w, px * 0.5f) : px;
      text_at(X(L.value_x), y, r.disabled ? kDisabled : kWhite, r.value.c_str(), true, vpx);
    }
  }

  // Footer: restart note / status message (below the rows, like the game's play time).
  const float note_px = vp.h * 0.026f;
  float fy = Y(int(rows.size())) + px * 0.2f;
  const bool needs_restart = s.resolution_scale != startup_.resolution_scale ||
                             s.anti_aliasing != startup_.anti_aliasing ||
                             s.cheat_difficulties != startup_.cheat_difficulties ||
                             s.cheat_costumes != startup_.cheat_costumes ||
                             s.skip_intro != startup_.skip_intro;
  if (needs_restart) {
    text_at(X(L.label_x), fy, kGrey, "Some changes apply the next time the game starts.", false,
            note_px);
    fy += note_px * 1.5f;
  }
  if (now < status_until_ms_) text_at(X(L.label_x), fy, kWhite, status_.c_str(), false, note_px);
}

}  // namespace ng2

