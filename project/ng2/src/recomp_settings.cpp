#include "recomp_settings.h"
#include "shader_precompile.h"
#include "update_check.h"

// Must come before <xinput.h>: its enum names collide with XInput's macros.
#include <rex/input/input_filter.h>

#include <windows.h>
#include <shellapi.h>
#include <xinput.h>

#include <algorithm>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <mutex>
#include <vector>

#include <imgui.h>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/input/input_filter.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#pragma comment(lib, "xinput.lib")
#pragma comment(lib, "shell32.lib")

bool Ng2IngameRecompRowSelected();  // ng2_hooks.cpp
int64_t Ng2IngameMenuHiddenMs();     // ng2_hooks.cpp
bool Ng2MainMenuUp();                // ng2_hooks.cpp: the main menu (NEW GAME ...) is on screen
int Ng2MainMenuCursor();             // ng2_hooks.cpp: its cursor 0..6, -1 when not up

REXCVAR_DEFINE_BOOL(ng2_prompt_device_diag, false, "NG2",
                    "Log what switches button prompts between controller and keyboard");

namespace ng2 {

int CurrentPromptStyle();  // ps_prompts.cpp
bool PromptsUseKeyboard();

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
const char* UpscalerCvarValue(int u) { return u == 2 ? "cas" : u == 1 ? "fsr" : "bilinear"; }
int UpscalerFromCvar(std::string v) {
  if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
  return v == "cas" ? 2 : v == "fsr" ? 1 : 0;
}
// Sharpness 0..10 -> FSR RCAS reduction in stops (0 = sharpest, 2 = off-ish; AMD default
// 0.2 = level 9) and CAS additional sharpness (0..1).
double FsrSharpnessReduction(int level) { return (10 - std::clamp(level, 0, 10)) * 0.2; }
double CasAdditionalSharpness(int level) { return std::clamp(level, 0, 10) / 10.0; }

// Push the upscaler settings into the running presenter (UI thread), so they apply live.
void ApplyUpscalerLive(rex::ui::ImGuiDrawer* drawer, const RecompSettings& s) {
  rex::ui::Presenter* presenter = drawer ? drawer->presenter() : nullptr;
  if (!presenter) return;
  using Config = rex::ui::Presenter::GuestOutputPaintConfig;
  Config config = presenter->GetGuestOutputPaintConfigFromUIThread();
  config.SetEffect(s.upscaler == 2   ? Config::Effect::kCas
                   : s.upscaler == 1 ? Config::Effect::kFsr
                                     : Config::Effect::kBilinear);
  config.SetFsrSharpnessReduction(float(FsrSharpnessReduction(s.sharpness)));
  config.SetCasAdditionalSharpness(float(CasAdditionalSharpness(s.sharpness)));
  presenter->SetGuestOutputPaintConfigFromUIThread(config);
  REXLOG_INFO("RECOMP SETTINGS: upscaling {} sharpness {}", UpscalerCvarValue(s.upscaler),
              s.sharpness);
}

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


// ---- fonts -------------------------------------------------------------------------

ImFont* g_serif_font = nullptr;  // Georgia (matches the game's Options text)

// ---- shared state (pane <-> input/present hooks) -----------------------------------

std::atomic<bool> g_pane_focused{false};
std::atomic<bool> g_block_until_release{false};
std::atomic<bool> g_invert_x{false};
std::atomic<bool> g_invert_y{false};
std::atomic<int> g_deadzone_pct{0};
std::atomic<int> g_vibration_pct{100};
std::atomic<int> g_button_prompts{0};
std::atomic<uint32_t> g_quit_presses{0};  // B presses on the main menu (Exit to Desktop)
std::atomic<bool> g_main_menu_message{false};  // a message (Leaderboards / Marketplace) is over it
std::atomic<bool> g_pause_in_background{false};
std::atomic<uint64_t> g_game_frames{0};

void PublishCheats(const RecompSettings& s) {
  rex::cvar::SetFlagByName("ng2_cheat_difficulties", s.cheat_difficulties ? "true" : "false");
  rex::cvar::SetFlagByName("ng2_cheat_costumes", s.cheat_costumes ? "true" : "false");
  rex::cvar::SetFlagByName("ng2_skip_intro", s.skip_intro ? "true" : "false");
  rex::cvar::SetFlagByName("ng2_skip_any_cutscene", s.skip_any_cutscene ? "true" : "false");
  rex::cvar::SetFlagByName("ng2_fast_loading", s.fast_loading ? "true" : "false");
}

void PublishInputSettings(const RecompSettings& s) {
  g_invert_x = s.invert_camera_x;
  g_invert_y = s.invert_camera_y;
  g_deadzone_pct = s.stick_deadzone;
  g_vibration_pct = s.vibration;
  g_button_prompts = s.button_prompts;
  g_pause_in_background = s.pause_in_background;
}

// ---- keyboard & mouse ------------------------------------------------------------------

std::atomic<bool> g_mnk_enabled{true};

// PC defaults chosen by the user (2026-11-01). Labels are NG2's action names (game text and
// Controller Settings, default Type A shoulder layout).
}  // namespace
const BindDef kBindDefs[kBindCount] = {
    {"keybind_lstick_up", "Move Forward", "W"},
    {"keybind_lstick_down", "Move Back", "S"},
    {"keybind_lstick_left", "Move Left", "A"},
    {"keybind_lstick_right", "Move Right", "D"},
    {"keybind_a", "Jump", "Space"},
    {"keybind_x", "Quick Attack", "LMB"},
    {"keybind_y", "Strong Attack", "RMB"},
    {"keybind_b", "Projectile Weapon", "MMB"},
    {"ng2_bind_ninpo", "Cast Ninpo", "X", XINPUT_GAMEPAD_Y | XINPUT_GAMEPAD_B},
    {"keybind_left_trigger", "Block", "Shift"},
    {"keybind_right_trigger", "Center Camera", "C"},
    {"keybind_right_shoulder", "Interact", "E"},
    {"keybind_left_shoulder", "1st-Person View", "V"},
    {"keybind_dpad_up", "Quick Menu", "Q"},
    {"keybind_back", "Options Menu", "Tab"},
    {"keybind_start", "Pause Menu", "Escape"},
};
// SDK keybinds no row uses: cleared so their SDK defaults (arrows, K, Shift+arrows) stay off.
const char* const kUnusedBindCvars[] = {
    "keybind_rstick_up",   "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right",
    "keybind_rstick_press", "keybind_lstick_press", "keybind_dpad_down",  "keybind_dpad_left",
    "keybind_dpad_right",  "keybind_guide"};

struct MouseCameraParams {
  bool enabled;
  float sens_x, sens_y;
  bool invert_y;
  bool auto_center;
};
MouseCameraParams GetMouseCameraParams();

std::array<std::string, kBindCount> DefaultBinds() {
  std::array<std::string, kBindCount> out;
  for (int i = 0; i < kBindCount; ++i) out[i] = kBindDefs[i].def;
  return out;
}
namespace {

// Key names the SDK parser accepts (keybinds.cpp); the first name per key wins.
const char* const kKeyNameList[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R",
    "S", "T", "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12", "Backtick",
    "Minus", "Plus", "Comma", "Period", "Semicolon", "Slash", "Backslash", "LBracket",
    "RBracket", "Quote", "Escape", "Return", "Space", "Tab", "Backspace", "Delete", "Insert",
    "Home", "End", "PageUp", "PageDown", "Left", "Right", "Up", "Down", "Shift", "Control",
    "Alt", "Numpad0", "Numpad1", "Numpad2", "Numpad3", "Numpad4", "Numpad5", "Numpad6",
    "Numpad7", "Numpad8", "Numpad9", "NumpadPlus", "NumpadMinus", "NumpadStar", "NumpadSlash",
    "CapsLock", "LMB", "RMB", "MMB", "Mouse4", "Mouse5", "WheelUp", "WheelDown"};

// Virtual key -> bind name (for key capture).
const std::string& NameForVk(int vk) {
  static const std::array<std::string, 256> table = [] {
    std::array<std::string, 256> t;
    for (const char* n : kKeyNameList) {
      const int v = int(rex::ui::ParseVirtualKey(n));
      if (v > 0 && v < 256 && t[v].empty()) t[v] = n;
    }
    return t;
  }();
  static const std::string none;
  return vk > 0 && vk < 256 ? table[vk] : none;
}

std::string PrettyKey(const std::string& n) {
  static const std::pair<const char*, const char*> kPretty[] = {
      {"LMB", "Left Mouse"},   {"RMB", "Right Mouse"},  {"MMB", "Middle Mouse"},
      {"Mouse4", "Mouse 4"},   {"Mouse5", "Mouse 5"},   {"WheelUp", "Wheel Up"},
      {"WheelDown", "Wheel Down"}, {"Control", "Ctrl"}, {"Return", "Enter"},
      {"Escape", "Esc"},       {"Backtick", "`"},       {"Minus", "-"},
      {"Plus", "="},           {"Comma", ","},          {"Period", "."},
      {"Semicolon", ";"},      {"Slash", "/"},          {"Backslash", "\\"},
      {"LBracket", "["},       {"RBracket", "]"},       {"Quote", "'"},
      {"PageUp", "Page Up"},   {"PageDown", "Page Down"}, {"CapsLock", "Caps Lock"},
      {"NumpadPlus", "Num +"}, {"NumpadMinus", "Num -"}, {"NumpadStar", "Num *"},
      {"NumpadSlash", "Num /"}};
  for (const auto& [k, v] : kPretty) {
    if (n == k) return v;
  }
  if (n.rfind("Numpad", 0) == 0) return "Num " + n.substr(6);
  return n;
}

// "Shift+W,Up" -> "Shift+W / Up"; "" -> "None".
std::string PrettyBind(const std::string& v) {
  std::string out, tok;
  auto flush = [&] {
    while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
    while (!tok.empty() && tok.back() == ' ') tok.pop_back();
    if (tok.empty()) return;
    std::string pretty;
    size_t start = 0, plus;
    while ((plus = tok.find('+', start)) != std::string::npos && plus > start) {
      pretty += PrettyKey(tok.substr(start, plus - start)) + "+";
      start = plus + 1;
    }
    pretty += PrettyKey(tok.substr(start));
    if (!out.empty()) out += " / ";
    out += pretty;
    tok.clear();
  };
  for (char c : v) {
    if (c == ',') flush();
    else tok += c;
  }
  flush();
  return out.empty() ? "None" : out;
}

std::string FormatSens(int v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%.1f", v / 10.0);
  return b;
}

// App-side binds (keys that press several buttons): virtual keys per alternative.
struct AppBind {
  std::vector<int> vks;  // any of these held = pressed (modifier prefixes are not supported)
  uint16_t buttons;
};
std::mutex g_app_binds_mutex;
std::vector<AppBind> g_app_binds;

std::vector<int> ParseBindVks(const std::string& v) {
  std::vector<int> out;
  std::string tok;
  auto flush = [&] {
    while (!tok.empty() && tok.front() == ' ') tok.erase(0, 1);
    while (!tok.empty() && tok.back() == ' ') tok.pop_back();
    const size_t plus = tok.rfind('+');
    if (plus != std::string::npos) tok = tok.substr(plus + 1);
    const int vk = tok.empty() ? 0 : int(rex::ui::ParseVirtualKey(tok));
    if (vk > 0 && vk < 256) out.push_back(vk);
    tok.clear();
  };
  for (char c : v) {
    if (c == ',') flush();
    else tok += c;
  }
  flush();
  return out;
}

std::atomic<bool> g_mouse_camera{true};
std::atomic<float> g_mouse_sens_x{1.0f};
std::atomic<float> g_mouse_sens_y{1.0f};
std::atomic<bool> g_mouse_invert_y{false};
std::atomic<bool> g_mouse_auto_center{false};

std::atomic<uint32_t> g_prompt_labels_version{1};
std::mutex g_prompt_binds_mutex;
std::array<std::string, kBindCount> g_prompt_binds;
// Keyboard prompts: true after the last input came from the keyboard or mouse.
std::atomic<bool> g_prompts_keyboard{false};

// Movement keys (first key of Move Forward / Back / Left / Right): also menu up/down/left/right.
std::atomic<int> g_move_vk[4] = {{'W'}, {'S'}, {'A'}, {'D'}};

void PublishMnk(const RecompSettings& s) {
  {
    std::lock_guard<std::mutex> lock(g_prompt_binds_mutex);
    g_prompt_binds = s.binds;
  }
  for (int i = 0; i < 4; ++i) {
    const std::vector<int> vks = ParseBindVks(s.binds[i]);
    g_move_vk[i] = vks.empty() ? 0 : vks.front();
  }
  g_prompt_labels_version.fetch_add(1);
  g_mnk_enabled = s.mnk_enabled;
  g_mouse_camera = s.mouse_camera;
  g_mouse_sens_x = s.mouse_sens / 10.0f;
  g_mouse_sens_y = s.mouse_sens_y / 10.0f;
  rex::cvar::SetFlagByName("ng2_bow_aim_gain", std::to_string(s.aim_sens / 10.0));
  g_mouse_invert_y = s.invert_mouse_y;
  g_mouse_auto_center = s.mouse_auto_center;
  // Mouse camera: motion goes straight into the game's stick floats (ng2_hooks.cpp), not
  // through the SDK's capped right-stick emulation.
  rex::cvar::SetFlagByName("mnk_mouse_raw", "true");
  rex::cvar::SetFlagByName("mnk_mode", s.mnk_enabled ? "true" : "false");
  rex::cvar::SetFlagByName("mnk_mouse", s.mouse_camera ? "true" : "false");
  rex::cvar::SetFlagByName("mnk_sensitivity", std::to_string(s.mouse_sens / 10.0));
  rex::cvar::SetFlagByName("mnk_sensitivity_y", std::to_string(s.mouse_sens_y / 10.0));
  rex::cvar::SetFlagByName("mnk_invert_y", s.invert_mouse_y ? "true" : "false");
  {
    std::lock_guard<std::mutex> lock(g_app_binds_mutex);
    g_app_binds.clear();
    for (int i = 0; i < kBindCount; ++i) {
      if (kBindDefs[i].app_buttons) {
        g_app_binds.push_back({ParseBindVks(s.binds[i]), kBindDefs[i].app_buttons});
      } else {
        rex::cvar::SetFlagByName(kBindDefs[i].cvar, s.binds[i]);
      }
    }
  }
  for (const char* c : kUnusedBindCvars) rex::cvar::SetFlagByName(c, "");
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

REXCVAR_DEFINE_BOOL(ng2_input_log, false, "NG2", "Diagnostic: log every pad state change the game gets");

bool RecompSettingsBlocksGameInput() {
  return g_pane_focused.load() || g_block_until_release.load();
}

void InstallRecompInputFilters() {
  rex::input::SetStateFilter([](uint32_t user_index, rex::input::X_INPUT_STATE* st) {
    auto& g = st->gamepad;
    // In-game menu, cursor on Recomp Settings (the old NINJA CINEMA row): Left/Right/A would
    // change the hidden NINJA CINEMA value; A opens our panel instead (read directly by
    // PadInput), so the game sees none of them.
    if (Ng2IngameRecompRowSelected()) {
      g.buttons = uint16_t(uint16_t(g.buttons) & ~(XINPUT_GAMEPAD_DPAD_LEFT |
                                                   XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_A));
      g.thumb_lx = int16_t(0);
    }
    // Keyboard in the game's menus: fixed PC keys (arrows, Enter = A, Esc / Backspace = B).
    // In gameplay Esc is the Pause binding (Start); in menus it must not reach Start.
    // Esc acts only in the context it was pressed in: held across a switch (pause menu opens
    // while it is still down, or closes), it does nothing until released, otherwise one press
    // opened and closed the pause menu.
    const bool gameplay_now = Ng2InGameplay();
    static int esc_context = -1;  // -1 up, 0 pressed in a menu, 1 pressed in gameplay
    {
      const bool esc_down = OurWindowFocused() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
      if (!esc_down) esc_context = -1;
      else if (esc_context < 0) esc_context = gameplay_now ? 1 : 0;
    }
    if (g_mnk_enabled && gameplay_now && esc_context == 0) {
      g.buttons = uint16_t(uint16_t(g.buttons) & ~XINPUT_GAMEPAD_START);
    }
    auto key_down = [](int vk) { return vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    // Arrows are the D-pad everywhere (in gameplay too: the quick-select wheel runs with the
    // HUD up, and arrows have no gameplay binding).
    if (g_mnk_enabled && OurWindowFocused() && !RecompSettingsBlocksGameInput()) {
      uint16_t b = uint16_t(g.buttons);
      if (key_down(VK_UP)) b |= XINPUT_GAMEPAD_DPAD_UP;
      if (key_down(VK_DOWN)) b |= XINPUT_GAMEPAD_DPAD_DOWN;
      if (key_down(VK_LEFT)) b |= XINPUT_GAMEPAD_DPAD_LEFT;
      if (key_down(VK_RIGHT)) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
      g.buttons = b;
    }
    if (g_mnk_enabled && !gameplay_now && OurWindowFocused() &&
        !RecompSettingsBlocksGameInput()) {
      auto down = key_down;
      uint16_t b = uint16_t(g.buttons);
      // Movement keys (WASD) are the D-pad in menus too; their left-stick output is dropped so
      // menus that read both don't move twice.
      static const uint16_t kDpad[4] = {XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
                                        XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
      bool move_key = false;
      for (int i = 0; i < 4; ++i) {
        if (down(g_move_vk[i].load())) {
          b |= kDpad[i];
          move_key = true;
        }
      }
      // The SDK keyboard driver sees a key go up a frame later than GetAsyncKeyState, so right
      // after a release it still reports the stick pushed for a frame and the menu moved a
      // second time (findings #73: L 0 -32767 one frame after the D-pad dropped). Keep the
      // stick blanked for a moment after the last movement key went up.
      static int64_t move_key_until_ms = 0;
      if (move_key) move_key_until_ms = NowMs() + 150;
      if (move_key || NowMs() < move_key_until_ms) {
        g.thumb_lx = int16_t(0);
        g.thumb_ly = int16_t(0);
      }
      if (down(VK_RETURN)) b |= XINPUT_GAMEPAD_A;
      if (esc_context >= 0) b = uint16_t(b & ~XINPUT_GAMEPAD_START);
      if (esc_context == 0) b |= XINPUT_GAMEPAD_B;
      if (down(VK_BACK)) b |= XINPUT_GAMEPAD_B;
      if (down('F')) b |= XINPUT_GAMEPAD_X;  // menu X / Y (mouse buttons are off in menus)
      if (down('R')) b |= XINPUT_GAMEPAD_Y;
      g.buttons = b;
    }
    // App-side binds (Cast Ninpo = Y+B): gameplay only (in menus B would back out).
    if (g_mnk_enabled && gameplay_now && OurWindowFocused() && !RecompSettingsBlocksGameInput()) {
      const bool gameplay = true;
      std::lock_guard<std::mutex> lock(g_app_binds_mutex);
      for (const auto& ab : g_app_binds) {
        for (int vk : ab.vks) {
          const bool mouse = vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON ||
                             vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
          if (mouse && !gameplay) continue;
          if (GetAsyncKeyState(vk) & 0x8000) {
            g.buttons = uint16_t(uint16_t(g.buttons) | ab.buttons);
            break;
          }
        }
      }
    }
    // Main menu [B] Exit to Desktop (GitHub #7): B there is ours (the game would go back to the
    // title screen); each new press is counted for the pane's confirm step. Keyboard Esc /
    // Backspace are B in menus (added above).
    // A on LEADERBOARDS (3) or Xbox LIVE Marketplace (6) opens a message over the menu (offline:
    // "You must be signed in ..."), with the menu rows still drawn behind it; B there closes the
    // message, so it goes to the game until that message is dismissed (next A or B; user report).
    static bool b_was_down[4] = {}, a_was_down[4] = {};
    static bool menu_message = false;
    const int cursor = Ng2MainMenuCursor();
    const bool b = (uint16_t(g.buttons) & XINPUT_GAMEPAD_B) != 0;
    const bool a = (uint16_t(g.buttons) & XINPUT_GAMEPAD_A) != 0;
    const bool a_edge = a && !a_was_down[user_index & 3], b_edge = b && !b_was_down[user_index & 3];
    a_was_down[user_index & 3] = a;
    b_was_down[user_index & 3] = b;
    if (cursor < 0) {
      menu_message = false;
    } else if (!RecompSettingsBlocksGameInput()) {
      if (menu_message) {
        if (a_edge || b_edge) menu_message = false;  // this press closes the message
      } else if (a_edge && (cursor == 3 || cursor == 6)) {
        menu_message = true;
      } else {
        if (b_edge) g_quit_presses.fetch_add(1);
        g.buttons = uint16_t(uint16_t(g.buttons) & ~XINPUT_GAMEPAD_B);
      }
    }
    g_main_menu_message = menu_message;
    const int dz = g_deadzone_pct.load();
    if (dz > 0) {
      ApplyDeadzone(g.thumb_lx, g.thumb_ly, dz / 100.0f);
      ApplyDeadzone(g.thumb_rx, g.thumb_ry, dz / 100.0f);
    }
    // NG2's camera is on the right stick.
    if (g_invert_x) g.thumb_rx = int16_t(std::clamp(-int(int16_t(g.thumb_rx)), -32767, 32767));
    if (g_invert_y) g.thumb_ry = int16_t(std::clamp(-int(int16_t(g.thumb_ry)), -32767, 32767));
    if (REXCVAR_GET(ng2_input_log)) {
      // NG2 DIAG (findings #73, double keyboard inputs): every change of what the game gets,
      // per user index, with the game frame and the number of polls since the last change.
      struct Seen {
        uint16_t buttons = 0;
        int16_t lx = 0, ly = 0, rx = 0, ry = 0;
        uint8_t lt = 0, rt = 0;
        uint32_t polls = 0;
      };
      static std::mutex m;
      static Seen seen[4];
      std::lock_guard<std::mutex> lock(m);
      Seen& o = seen[user_index & 3];
      ++o.polls;
      const Seen n{uint16_t(g.buttons), int16_t(g.thumb_lx), int16_t(g.thumb_ly), int16_t(g.thumb_rx),
                   int16_t(g.thumb_ry), uint8_t(g.left_trigger), uint8_t(g.right_trigger), 0};
      if (n.buttons != o.buttons || n.lx != o.lx || n.ly != o.ly || n.rx != o.rx || n.ry != o.ry ||
          n.lt != o.lt || n.rt != o.rt) {
        REXLOG_INFO("NG2 input: user {} buttons {:04X} L {} {} R {} {} T {} {} (frame {}, {} polls, gameplay {})",
                    user_index, n.buttons, n.lx, n.ly, n.rx, n.ry, n.lt, n.rt,
                    g_game_frames.load(std::memory_order_relaxed), o.polls, gameplay_now);
        o = n;
      }
    }
  });
  rex::input::SetVibrationFilter([](uint32_t, rex::input::X_INPUT_VIBRATION* v) {
    const int pct = g_vibration_pct.load();
    v->left_motor_speed = uint16_t(uint32_t(uint16_t(v->left_motor_speed)) * pct / 100);
    v->right_motor_speed = uint16_t(uint32_t(uint16_t(v->right_motor_speed)) * pct / 100);
  });
}

void RecompCountGameFrame() { g_game_frames.fetch_add(1, std::memory_order_relaxed); }

// Every connected controller: SDL's view (Xbox, PS4 / PS5 DualShock / DualSense, Switch, ...)
// plus XInput (the only source with the xinput backend; with SDL an Xbox pad shows up in both,
// which is harmless for these OR-ed reads).
// families (optional): per pad 0 = Xbox / other, 1 = PS4 (or older), 2 = PS5.
static std::vector<XINPUT_GAMEPAD> ConnectedPads(std::vector<int>* families = nullptr) {
  std::vector<XINPUT_GAMEPAD> pads;
  for (const auto& p : rex::input::ReadPhysicalPads()) {
    if (families) {
      families->push_back(p.family == rex::input::PadFamily::kPlayStation5  ? 2
                          : p.family == rex::input::PadFamily::kPlayStation ? 1
                                                                            : 0);
    }
    XINPUT_GAMEPAD g{};
    g.wButtons = uint16_t(p.gamepad.buttons);
    g.bLeftTrigger = uint8_t(p.gamepad.left_trigger);
    g.bRightTrigger = uint8_t(p.gamepad.right_trigger);
    g.sThumbLX = int16_t(p.gamepad.thumb_lx);
    g.sThumbLY = int16_t(p.gamepad.thumb_ly);
    g.sThumbRX = int16_t(p.gamepad.thumb_rx);
    g.sThumbRY = int16_t(p.gamepad.thumb_ry);
    pads.push_back(g);
  }
  for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
    XINPUT_STATE st{};
    if (XInputGetState(i, &st) == ERROR_SUCCESS) {
      pads.push_back(st.Gamepad);
      if (families) families->push_back(0);
    }
  }
  return pads;
}

void PadInput::Poll() {
  enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kAccept = 16, kBack = 32 };
  uint32_t cur = 0;
  for (const XINPUT_GAMEPAD& g : ConnectedPads()) {
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_UP) || g.sThumbLY > 16000) cur |= kUp;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_DOWN) || g.sThumbLY < -16000) cur |= kDown;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) || g.sThumbLX < -16000) cur |= kLeft;
    if ((g.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) || g.sThumbLX > 16000) cur |= kRight;
    if (g.wButtons & XINPUT_GAMEPAD_A) cur |= kAccept;
    if (g.wButtons & XINPUT_GAMEPAD_B) cur |= kBack;
  }
  if (OurWindowFocused()) {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    auto move = [&](int i) { const int vk = g_move_vk[i].load(); return vk > 0 && down(vk); };
    if (down(VK_UP) || move(0)) cur |= kUp;
    if (down(VK_DOWN) || move(1)) cur |= kDown;
    if (down(VK_LEFT) || move(2)) cur |= kLeft;
    if (down(VK_RIGHT) || move(3)) cur |= kRight;
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
  int bind_layout = 0;
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
    else if (k == "resolution_scale") resolution_scale = ParseInt(v, resolution_scale, 1, 5);
    else if (k == "swap_post_effect") anti_aliasing = AaFromCvar(v);
    else if (k == "present_effect") upscaler = UpscalerFromCvar(v);
    else if (k == "ng2_sharpness") sharpness = ParseInt(v, sharpness, 0, 10);
    else if (k == "ng2_aspect_mode") aspect_mode = ParseInt(v, aspect_mode, 0, 3);
    else if (k == "ng2_fov_percent") fov_percent = ParseInt(v, fov_percent, 70, 150);
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
    else if (k == "ng2_button_prompts") button_prompts = ParseInt(v, button_prompts, 0, 3);
    else if (k == "ng2_check_updates") check_updates = ParseBool(v, check_updates);
    else if (k == "ng2_pause_in_background")
      pause_in_background = ParseBool(v, pause_in_background);
    else if (k == "ng2_window_width") window_width = ParseInt(v, window_width, 0, 16384);
    else if (k == "ng2_window_height") window_height = ParseInt(v, window_height, 0, 16384);
    else if (k == "ng2_window_x") window_x = ParseInt(v, window_x, -65536, 65536);
    else if (k == "ng2_window_y") window_y = ParseInt(v, window_y, -65536, 65536);
    else if (k == "ng2_cheat_difficulties") cheat_difficulties = ParseBool(v, cheat_difficulties);
    else if (k == "ng2_cheat_costumes") cheat_costumes = ParseBool(v, cheat_costumes);
    else if (k == "ng2_skip_intro") skip_intro = ParseBool(v, skip_intro);
    else if (k == "ng2_skip_any_cutscene")
      skip_any_cutscene = ParseBool(v, skip_any_cutscene);
    else if (k == "ng2_fast_loading") fast_loading = ParseBool(v, fast_loading);
    else if (k == "mnk_mode") mnk_enabled = ParseBool(v, mnk_enabled);
    else if (k == "mnk_mouse") mouse_camera = ParseBool(v, mouse_camera);
    else if (k == "ng2_mouse_sens") mouse_sens = ParseInt(v, mouse_sens, 1, 50);
    else if (k == "ng2_mouse_sens_y") mouse_sens_y = ParseInt(v, mouse_sens_y, 1, 50);
    else if (k == "ng2_aim_sens") aim_sens = ParseInt(v, aim_sens, 1, 50);
    else if (k == "mnk_invert_y") invert_mouse_y = ParseBool(v, invert_mouse_y);
    else if (k == "ng2_mouse_auto_center") mouse_auto_center = ParseBool(v, mouse_auto_center);
    else if (k == "ng2_bind_layout") bind_layout = ParseInt(v, 0, 0, 1000);
    else if (k.rfind("keybind_", 0) == 0 || k.rfind("ng2_bind_", 0) == 0) {
      std::string val = v;
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
        val = val.substr(1, val.size() - 2);
      }
      for (int i = 0; i < kBindCount; ++i) {
        if (k == kBindDefs[i].cvar) binds[i] = val;
      }
    }
  }
  // Bindings saved under an older default layout are replaced by the current defaults.
  if (bind_layout < kBindLayoutVersion) binds = DefaultBinds();
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
      << "present_effect = \"" << UpscalerCvarValue(upscaler) << "\"\n"
      << "ng2_sharpness = " << sharpness << "\n"
      << "present_fsr_sharpness_reduction = " << FsrSharpnessReduction(sharpness) << "\n"
      << "present_cas_additional_sharpness = " << CasAdditionalSharpness(sharpness) << "\n"
      << "ng2_aspect_mode = " << aspect_mode << "\n"
      << "ng2_fov_percent = " << fov_percent << "\n"
      << "ng2_framerate_cap = " << framerate_cap << "\n"
      << "frame_generation = " << b(framerate_cap == 120) << "\n"
      << "present_half_rate = " << b(framerate_cap == 30) << "\n"
      << "ng2_master_volume = " << master_volume << "\n"
      << "ng2_mute_in_background = " << b(mute_in_background) << "\n"
      << "ng2_invert_camera_x = " << b(invert_camera_x) << "\n"
      << "ng2_invert_camera_y = " << b(invert_camera_y) << "\n"
      << "ng2_stick_deadzone = " << stick_deadzone << "\n"
      << "ng2_vibration = " << vibration << "\n"
      << "ng2_button_prompts = " << button_prompts << "\n"
      << "ng2_pause_in_background = " << b(pause_in_background) << "\n"
      << "ng2_check_updates = " << b(check_updates) << "\n"
      << "ng2_window_width = " << window_width << "\n"
      << "ng2_window_height = " << window_height << "\n";
  if (window_x != INT_MIN && window_y != INT_MIN) {
    out << "ng2_window_x = " << window_x << "\n"
        << "ng2_window_y = " << window_y << "\n";
  }
  out
      << "ng2_cheat_difficulties = " << b(cheat_difficulties) << "\n"
      << "ng2_cheat_costumes = " << b(cheat_costumes) << "\n"
      << "ng2_skip_intro = " << b(skip_intro) << "\n"
      << "ng2_skip_any_cutscene = " << b(skip_any_cutscene) << "\n"
      << "ng2_fast_loading = " << b(fast_loading) << "\n"
      << "mnk_mode = " << b(mnk_enabled) << "\n"
      << "mnk_mouse = " << b(mouse_camera) << "\n"
      << "ng2_mouse_sens = " << mouse_sens << "\n"
      << "ng2_mouse_sens_y = " << mouse_sens_y << "\n"
      << "ng2_aim_sens = " << aim_sens << "\n"
      << "mnk_sensitivity = " << mouse_sens / 10.0 << "\n"
      << "mnk_sensitivity_y = " << mouse_sens_y / 10.0 << "\n"
      << "mnk_invert_y = " << b(invert_mouse_y) << "\n"
      << "ng2_mouse_auto_center = " << b(mouse_auto_center) << "\n";
  out << "ng2_bind_layout = " << kBindLayoutVersion << "\n";
  for (int i = 0; i < kBindCount; ++i) {
    out << kBindDefs[i].cvar << " = \"" << binds[i] << "\"\n";
  }
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
  PublishMnk(s);  // also when the file predates these keys (PC defaults, not the SDK's)
  rex::cvar::SetFlagByName("ng2_fov_percent", std::to_string(s.fov_percent));
  REXLOG_INFO("RECOMP SETTINGS loaded from {} (present_effect {})", path.string(),
              rex::cvar::GetFlagByName("present_effect"));
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

const char* kCategories[] = {"Display",          "Graphics", "Audio", "Controls",
                             "Keyboard & Mouse", "System",   "Cheats"};
constexpr int kCategoryCount = 7;
// Rows on screen including the pinned Category row; longer lists scroll.
constexpr int kVisibleRows = 9;

enum class Kind { kStepper, kPair, kAction, kBind };
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
  int bind = -1;          // kBind: index into kBindDefs
};

}  // namespace

// ---- update check (System > Update) ---------------------------------------------------

struct UpdateCheckState {
  std::mutex mutex;
  int phase = 0;  // 0 not started, 1 checking, 2 done
  bool logged = false;
  openng2::ReleaseInfo info;
};

void RecompOptionsPane::StartUpdateCheck() {
  auto st = update_;
  {
    std::lock_guard<std::mutex> lock(st->mutex);
    if (st->phase == 1) return;
    st->phase = 1;
    st->logged = false;
  }
  // Detached: the request can take seconds (timeouts) and must never hold up quitting.
  std::thread([st] {
    openng2::ReleaseInfo info =
        openng2::FetchLatestRelease((std::string("openNG2/") + openng2::kVersion).c_str());
    std::lock_guard<std::mutex> lock(st->mutex);
    st->info = std::move(info);
    st->phase = 2;
  }).detach();
}

// The launcher that built this copy (openNG2-install.ini next to ng2.exe) does the update:
// it replaces itself with the new release and rebuilds the game; the player keeps playing
// until it asks them to close the game. Without a launcher, open the release page.
void RecompOptionsPane::OpenUpdater() {
  wchar_t exe[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  const std::filesystem::path dir = std::filesystem::path(exe).parent_path();
  std::ifstream f(dir / openng2::kInstallInfoFile);
  std::string line, launcher_utf8;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.rfind("launcher=", 0) == 0) launcher_utf8 = line.substr(9);
  }
  std::wstring launcher;
  if (!launcher_utf8.empty()) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, launcher_utf8.data(), int(launcher_utf8.size()), nullptr, 0);
    launcher.resize(size_t(n));
    MultiByteToWideChar(CP_UTF8, 0, launcher_utf8.data(), int(launcher_utf8.size()), launcher.data(), n);
  }
  std::error_code ec;
  const int64_t now = NowMs();
  if (!launcher.empty() && std::filesystem::exists(launcher, ec)) {
    const std::wstring work = std::filesystem::path(launcher).parent_path().wstring();
    if (reinterpret_cast<intptr_t>(ShellExecuteW(nullptr, L"open", launcher.c_str(), L"--update", work.c_str(),
                                                 SW_SHOWNORMAL)) > 32) {
      REXLOG_INFO("UPDATE: opened the launcher to update");
      status_ = "The launcher is updating openNG2. Keep playing; close the game when it asks.";
      status_until_ms_ = now + 10000;
      return;
    }
  }
  REXLOG_INFO("UPDATE: no launcher registered, opening the release page");
  ShellExecuteW(nullptr, L"open", L"https://github.com/KamiTK2/openNG2/releases/latest", nullptr, nullptr,
                SW_SHOWNORMAL);
  status_ = "Launcher not found: opened the openNG2 release page in your browser.";
  status_until_ms_ = now + 10000;
}

// Short notice outside gameplay once a newer release is known.
void RecompOptionsPane::DrawUpdateNotice(ImGuiIO& io, int64_t now) {
  std::string version;
  {
    std::lock_guard<std::mutex> lock(update_->mutex);
    if (update_->phase != 2) return;
    if (!update_->logged) {
      update_->logged = true;
      if (update_->info.ok) {
        REXLOG_INFO("UPDATE: latest release {} (this build {})", update_->info.version, openng2::kVersion);
      } else {
        REXLOG_INFO("UPDATE: check failed: {}", update_->info.error);
      }
    }
    if (!update_->info.ok || !openng2::IsNewer(update_->info.version, openng2::kVersion)) return;
    version = update_->info.version;
  }
  // Shown on the title screen and menus until the player first reaches gameplay, with a
  // clickable UPDATE NOW button (opens the launcher, like System > Update).
  if (Ng2InGameplay()) update_notice_until_ms_ = 0;
  if (update_notice_until_ms_ == 0 || Ng2InGameplay() || !settings_.check_updates) return;
  // After a click the notice shows the launcher message for a while, then goes away.
  const bool clicked = update_notice_until_ms_ > 0;
  if (clicked && now >= update_notice_until_ms_) {
    update_notice_until_ms_ = 0;
    return;
  }
  const std::string text = clicked ? status_ : "Update available (openNG2 " + version + ") - Open the launcher";
  const char* kUpdate = "UPDATE NOW";
  const char* kClose = "CLOSE";
  ImFont* f = g_serif_font ? g_serif_font : ImGui::GetFont();
  const float size = io.DisplaySize.y * 0.022f;  // small, top centre (any aspect)
  const float pad = size * 0.6f, vpad = size * 0.35f;
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  auto button_width = [&](const char* label) {
    return f->CalcTextSizeA(size, FLT_MAX, 0.0f, label).x + pad * 2.0f;
  };
  // Buttons (mouse only; keyboard and controller use System > Update).
  const float update_w = clicked ? 0.0f : button_width(kUpdate);
  const float close_w = clicked ? 0.0f : button_width(kClose);
  const float buttons_w = clicked ? 0.0f : pad + update_w + vpad + close_w;
  const ImVec2 at(std::floor((io.DisplaySize.x - ts.x - buttons_w) * 0.5f), std::floor(size * 0.6f + vpad));
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 box0(at.x - pad, at.y - vpad);
  const ImVec2 box1(at.x + ts.x + buttons_w + (clicked ? pad : vpad), at.y + ts.y + vpad);
  dl->AddRectFilled(box0, box1, IM_COL32(0, 0, 0, 185), size * 0.3f);
  dl->AddRectFilled(box0, ImVec2(at.x - pad + size * 0.15f, box1.y), kRedMarker);
  dl->AddText(f, size, at, kWhite, text.c_str());
  if (clicked) return;
  auto button = [&](float x, float w, const char* label, ImU32 normal, ImU32 hot) {
    const ImVec2 b0(x, at.y - vpad * 0.6f), b1(x + w, at.y + ts.y + vpad * 0.6f);
    const ImVec2 m = io.MousePos;
    const bool hover = m.x >= b0.x && m.x < b1.x && m.y >= b0.y && m.y < b1.y;
    dl->AddRectFilled(b0, b1, hover ? hot : normal, size * 0.2f);
    dl->AddText(f, size, ImVec2(x + pad, at.y), kWhite, label);
    return hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  };
  const float update_x = at.x + ts.x + pad;
  if (button(update_x, update_w, kUpdate, IM_COL32(170, 20, 14, 255), IM_COL32(230, 40, 30, 255))) {
    OpenUpdater();
    update_notice_until_ms_ = now + 10000;
    return;
  }
  if (button(update_x + update_w + vpad, close_w, kClose, IM_COL32(60, 58, 56, 255), IM_COL32(95, 92, 88, 255))) {
    update_notice_until_ms_ = 0;  // hidden for this session (System > Update still works)
  }
}

// ---- update ready (launcher waiting for the game to close; GitHub issue #2) -------------

namespace {
std::filesystem::path ExeDir() {
  wchar_t exe[MAX_PATH * 2];
  GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
  return std::filesystem::path(exe).parent_path();
}
bool ProcessAlive(DWORD pid) {
  if (!pid) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!h) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return alive;
}
}  // namespace

// Newest write time of any profile's story saves (<user data>/<xuid>/544307D5/.../ng2stryd*):
// statue saves and chapter autosaves count; system data (ng2sysd.dat, written when settings
// change) does not (user, 2026-11-02).
int64_t RecompOptionsPane::LatestSaveWriteStamp() const {
  std::error_code ec;
  int64_t newest = 0;
  for (const auto& profile : std::filesystem::directory_iterator(user_data_root_, ec)) {
    const std::string name = profile.path().filename().string();
    if (!profile.is_directory(ec) || name.size() != 16 || name == "0000000000000000") continue;
    const auto title = profile.path() / "544307D5";
    if (!std::filesystem::is_directory(title, ec)) continue;
    for (auto it = std::filesystem::recursive_directory_iterator(title, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      if (it->path().filename().string().rfind("ng2stryd", 0) != 0) continue;
      const auto t = it->last_write_time(ec);
      if (!ec) newest = std::max<int64_t>(newest, t.time_since_epoch().count());
    }
  }
  return newest;
}

void RecompOptionsPane::PollUpdateReady(int64_t now) {
  if (now - ready_poll_ms_ < 1000) return;
  ready_poll_ms_ = now;
  std::string version;
  DWORD pid = 0;
  {
    std::ifstream f(ExeDir() / openng2::kUpdateReadyFile);
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("version=", 0) == 0) version = line.substr(8);
      if (line.rfind("pid=", 0) == 0) pid = DWORD(std::strtoul(line.c_str() + 4, nullptr, 10));
    }
  }
  // Only while the launcher that wrote it is still waiting (a closed launcher installs nothing).
  if (!version.empty() && !ProcessAlive(pid)) version.clear();
  if (version != ready_version_) {
    if (!version.empty()) REXLOG_INFO("UPDATE: openNG2 {} is built and ready (launcher pid {})", version, pid);
    ready_version_ = version;
    if (version.empty()) restart_after_save_ = false;
  }
  if (ready_version_.empty() || !restart_after_save_ || restarting_) return;
  const int64_t stamp = LatestSaveWriteStamp();
  if (stamp != save_seen_) {
    save_seen_ = stamp;
    save_changed_at_ms_ = now;
  }
  // A save was written after the choice: restart once its files have been quiet for 3 s.
  if (save_seen_ > save_baseline_ && now - save_changed_at_ms_ >= 3000) {
    REXLOG_INFO("UPDATE: save written, restarting for the update");
    RestartForUpdate();
  }
}

void RecompOptionsPane::RestartAfterNextSave() {
  restart_after_save_ = true;
  save_baseline_ = save_seen_ = LatestSaveWriteStamp();
  save_changed_at_ms_ = NowMs();
  REXLOG_INFO("UPDATE: will restart after the next save");
}

// Quits like the window's close button; the waiting launcher installs the update and starts
// the new version because of the restart request file.
void RecompOptionsPane::RestartForUpdate() {
  if (restarting_) return;
  restarting_ = true;
  {
    std::ofstream f(ExeDir() / openng2::kUpdateRestartFile, std::ios::trunc);
    f << "version=" << ready_version_ << "\n";
  }
  REXLOG_INFO("UPDATE: closing the game to install openNG2 {}", ready_version_);
  HWND h = window_ ? static_cast<HWND>(window_->GetNativeWindowHandle()) : nullptr;
  if (h) PostMessageW(h, WM_CLOSE, 0, 0);
}

// Update ready notice: in menus with RESTART NOW / AFTER NEXT SAVE / LATER (mouse; keyboard and
// controller use System > Update), and once as a short text-only toast in gameplay.
bool RecompOptionsPane::DrawUpdateReadyNotice(ImGuiIO& io, int64_t now) {
  if (ready_version_.empty()) return false;
  const bool gameplay = Ng2InGameplay();
  std::string text;
  bool buttons = false;
  if (restarting_) {
    text = "Closing to install openNG2 " + ready_version_ + " - it restarts by itself";
  } else if (gameplay) {
    if (ready_toast_until_ms_ < 0) ready_toast_until_ms_ = now + 8000;
    if (restart_after_save_ || now >= ready_toast_until_ms_) return true;
    text = "openNG2 " + ready_version_ + " is ready - pause the game to restart and update";
  } else if (restart_after_save_) {
    text = "openNG2 " + ready_version_ + " installs after your next save";
    buttons = true;  // RESTART NOW only
  } else {
    if (ready_notice_hidden_) return true;
    text = "openNG2 " + ready_version_ + " is ready to install - restart the game?";
    buttons = true;
  }
  struct Btn {
    const char* label;
    int id;
  };
  std::vector<Btn> btns;
  if (buttons) {
    btns.push_back({"RESTART NOW", 0});
    if (!restart_after_save_) {
      btns.push_back({"AFTER NEXT SAVE", 1});
      btns.push_back({"LATER", 2});
    }
  }
  ImFont* f = g_serif_font ? g_serif_font : ImGui::GetFont();
  const float size = io.DisplaySize.y * 0.022f;  // small, top centre (any aspect)
  const float pad = size * 0.6f, vpad = size * 0.35f;
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  std::vector<float> widths;
  float buttons_w = 0.0f;
  for (const auto& b : btns) {
    widths.push_back(f->CalcTextSizeA(size, FLT_MAX, 0.0f, b.label).x + pad * 2.0f);
    buttons_w += (buttons_w == 0.0f ? pad : vpad) + widths.back();
  }
  const ImVec2 at(std::floor((io.DisplaySize.x - ts.x - buttons_w) * 0.5f), std::floor(size * 0.6f + vpad));
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 box0(at.x - pad, at.y - vpad);
  const ImVec2 box1(at.x + ts.x + buttons_w + (btns.empty() ? pad : vpad), at.y + ts.y + vpad);
  dl->AddRectFilled(box0, box1, IM_COL32(0, 0, 0, 185), size * 0.3f);
  dl->AddRectFilled(box0, ImVec2(at.x - pad + size * 0.15f, box1.y), kRedMarker);
  dl->AddText(f, size, at, kWhite, text.c_str());
  float x = at.x + ts.x;
  for (size_t i = 0; i < btns.size(); ++i) {
    x += i == 0 ? pad : vpad;
    const float w = widths[i];
    const ImVec2 b0(x, at.y - vpad * 0.6f), b1(x + w, at.y + ts.y + vpad * 0.6f);
    const ImVec2 m = io.MousePos;
    const bool hover = m.x >= b0.x && m.x < b1.x && m.y >= b0.y && m.y < b1.y;
    const bool primary = btns[i].id == 0;
    dl->AddRectFilled(b0, b1,
                      primary ? (hover ? IM_COL32(230, 40, 30, 255) : IM_COL32(170, 20, 14, 255))
                              : (hover ? IM_COL32(95, 92, 88, 255) : IM_COL32(60, 58, 56, 255)),
                      size * 0.2f);
    dl->AddText(f, size, ImVec2(x + pad, at.y), kWhite, btns[i].label);
    if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (btns[i].id == 0) RestartForUpdate();
      if (btns[i].id == 1) RestartAfterNextSave();
      if (btns[i].id == 2) ready_notice_hidden_ = true;
      break;
    }
    x += w;
  }
  return true;
}

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
      ingame_row_selected_(std::move(ingame_row_selected)),
      update_(std::make_shared<UpdateCheckState>()) {
  settings_.Load(settings_path_);
  // Show what this session actually runs with (may come from the command line).
  settings_.resolution_scale =
      std::clamp(std::atoi(rex::cvar::GetFlagByName("resolution_scale").c_str()), 1, 5);
  settings_.monitor = std::clamp(std::atoi(rex::cvar::GetFlagByName("monitor").c_str()), 0, 16);
  settings_.anti_aliasing = AaFromCvar(rex::cvar::GetFlagByName("swap_post_effect"));
  // Upscaling is live, but show what is running (it may come from the command line).
  settings_.upscaler = UpscalerFromCvar(rex::cvar::GetFlagByName("present_effect"));
  {
    const std::string r = rex::cvar::GetFlagByName("present_fsr_sharpness_reduction");
    if (!r.empty()) {
      settings_.sharpness =
          std::clamp(int(std::lround(10.0 - std::atof(r.c_str()) / 0.2)), 0, 10);
    }
  }
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
  rex::cvar::SetFlagByName("ng2_fov_percent", std::to_string(settings_.fov_percent));
  rex::cvar::SetFlagByName("present_effect", UpscalerCvarValue(settings_.upscaler));
  rex::cvar::SetFlagByName("present_fsr_sharpness_reduction",
                           std::to_string(FsrSharpnessReduction(settings_.sharpness)));
  rex::cvar::SetFlagByName("present_cas_additional_sharpness",
                           std::to_string(CasAdditionalSharpness(settings_.sharpness)));
  ApplyUpscalerLive(imgui_drawer(), settings_);
  rex::cvar::SetFlagByName("frame_generation", settings_.framerate_cap == 120 ? "true" : "false");
  rex::cvar::SetFlagByName("present_half_rate", settings_.framerate_cap == 30 ? "true" : "false");
  if (!muted_for_background_) rex::audio::SetOutputGain(settings_.master_volume / 100.0f);
  PublishInputSettings(settings_);
  PublishCheats(settings_);
  PublishMnk(settings_);
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
    // Every 5 s; every 1 s with the load diagnostics on (diag_io_stats).
    const int64_t log_every = rex::cvar::GetFlagByName("diag_io_stats") == "true" ? 1000 : 5000;
    if (now - last_log_ms >= log_every) {
      last_log_ms = now;
      REXLOG_INFO("NG2 game fps {:.1f} (frames {}), host ui/present fps {:.1f}", fps, frames,
                  host_fps);
    }
  }
}

bool RecompOptionsPane::WantsContinuousPaint() const {
  int stage;
  uint32_t done, total;
  return ShaderProgress(&stage, &done, &total);
}

// "Preparing shaders" screen while the SDK builds the shader cache before the game starts.
static void DrawShaderProgress(ImGuiIO& io) {
  int stage;
  uint32_t done, total;
  if (!ShaderProgress(&stage, &done, &total)) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 ds = io.DisplaySize;
  dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, 255));
  ImFont* f = g_serif_font ? g_serif_font : ImGui::GetFont();
  const float size = ds.y * 0.035f;
  char text[96];
  if (total == 0) {
    std::snprintf(text, sizeof(text), "Preparing shaders...");
  } else {
    std::snprintf(text, sizeof(text), "%s %u / %u", stage == 0 ? "Compiling shaders" : "Building pipelines",
                  done, total);
  }
  const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float cy = ds.y * 0.5f;
  dl->AddText(f, size, ImVec2((ds.x - ts.x) * 0.5f, cy - ts.y - size * 0.4f), IM_COL32(230, 220, 200, 255), text);
  const float w = ds.x * 0.36f, h = std::max(4.0f, ds.y * 0.008f);
  const ImVec2 a((ds.x - w) * 0.5f, cy), b(a.x + w, cy + h);
  dl->AddRectFilled(a, b, IM_COL32(60, 55, 50, 255), h * 0.5f);
  // Two halves: shader translation, then pipeline creation.
  float frac = total ? float(done) / float(total) : 0.0f;
  frac = std::clamp((stage == 0 ? 0.0f : 0.5f) + frac * 0.5f, 0.0f, 1.0f);
  if (frac > 0.0f) dl->AddRectFilled(a, ImVec2(a.x + w * frac, b.y), IM_COL32(180, 30, 30, 255), h * 0.5f);
  const char* note = "Later launches are faster";
  const float ns = size * 0.6f;
  const ImVec2 nts = f->CalcTextSizeA(ns, FLT_MAX, 0.0f, note);
  dl->AddText(f, ns, ImVec2((ds.x - nts.x) * 0.5f, b.y + ns), IM_COL32(150, 140, 130, 255), note);
}

// The B / Back button in the current prompt style (Esc cap, Xbox B, PS4 / PS5 Circle) centred
// at c with radius r; returns its right edge.
float DrawBackButton(ImDrawList* dl, ImFont* font, ImVec2 c, float r) {
  if (PromptsUseKeyboard()) {
    const char* key = "Esc";
    const float ks = r * 1.2f;
    const ImVec2 t = font->CalcTextSizeA(ks, FLT_MAX, 0.0f, key);
    const float w = t.x + r * 0.9f, h = r * 1.8f;
    dl->AddRectFilled(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f),
                      IM_COL32(20, 14, 14, 200), h * 0.25f);
    dl->AddRect(ImVec2(c.x - w * 0.5f, c.y - h * 0.5f), ImVec2(c.x + w * 0.5f, c.y + h * 0.5f),
                IM_COL32(220, 210, 200, 230), h * 0.25f, 0, std::max(1.0f, h * 0.06f));
    dl->AddText(font, ks, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(245, 240, 232, 255), key);
    return c.x + w * 0.5f;
  }
  const int style = CurrentPromptStyle();
  if (style == 2) {  // PS5: light button, grey circle
    dl->AddCircleFilled(c, r, IM_COL32(205, 205, 205, 255));
    dl->AddCircle(c, r, IM_COL32(120, 120, 120, 255), 0, std::max(1.0f, r * 0.10f));
    dl->AddCircle(c, r * 0.52f, IM_COL32(95, 95, 95, 255), 0, std::max(1.5f, r * 0.15f));
  } else if (style == 1) {  // PS4: dark button, red circle
    dl->AddCircleFilled(c, r, IM_COL32(32, 32, 34, 255));
    dl->AddCircle(c, r, IM_COL32(90, 90, 95, 255), 0, std::max(1.0f, r * 0.10f));
    dl->AddCircle(c, r * 0.52f, IM_COL32(232, 60, 72, 255), 0, std::max(1.5f, r * 0.15f));
  } else {  // Xbox 360 B
    dl->AddCircleFilled(c, r, IM_COL32(176, 22, 18, 255));
    const ImVec2 t = font->CalcTextSizeA(r * 1.4f, FLT_MAX, 0.0f, "B");
    dl->AddText(font, r * 1.4f, ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), IM_COL32(255, 255, 255, 255), "B");
  }
  return c.x + r;
}

// Main menu [B] Exit to Desktop (GitHub #7): a prompt bottom right while the main menu is up;
// the first B asks, a second within 3 s closes the game like its close button.
void RecompOptionsPane::UpdateQuitToDesktop(const ImGuiIO& io) {
  static uint32_t seen = g_quit_presses.load();
  const uint32_t presses = g_quit_presses.load();
  const int64_t now = NowMs();
  const bool menu = Ng2MainMenuUp() && !g_main_menu_message.load() && !overlay_open_ && !pane_visible_();
  if (!menu) {
    quit_armed_until_ms_ = 0;
    seen = presses;
    return;
  }
  if (presses != seen) {
    seen = presses;
    if (now < quit_armed_until_ms_) {
      REXLOG_INFO("RECOMP SETTINGS: exit to desktop (main menu)");
      HWND h = window_ ? static_cast<HWND>(window_->GetNativeWindowHandle()) : nullptr;
      if (h) PostMessageW(h, WM_CLOSE, 0, 0);
      quit_armed_until_ms_ = 0;
    } else {
      quit_armed_until_ms_ = now + 3000;
      Ng2QueueSystemSound(kSoundCursor);
    }
  }
  ImFont* font = g_serif_font ? g_serif_font : ImGui::GetFont();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const Viewport vp = GameViewport(io);
  // Prompt, bottom right of the 16:9 frame.
  {
    const float size = vp.h * 0.030f, r = size * 0.45f;
    const char* label = "Exit to Desktop";
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
    const float right = vp.x + vp.w * 0.95f, cy = vp.y + vp.h * 0.93f;
    const float text_x = right - ts.x;
    const float bx = text_x - size * 0.35f - (PromptsUseKeyboard() ? size * 0.9f : r);
    DrawBackButton(dl, font, ImVec2(bx, cy), r);
    dl->AddText(font, size, ImVec2(text_x + size * 0.04f, cy - ts.y * 0.5f + size * 0.04f),
                IM_COL32(0, 0, 0, 200), label);
    dl->AddText(font, size, ImVec2(text_x, cy - ts.y * 0.5f), IM_COL32(240, 232, 220, 255), label);
  }
  if (now >= quit_armed_until_ms_) return;
  // Confirmation, centred.
  const int style = PromptsUseKeyboard() ? -1 : CurrentPromptStyle();
  const char* button = style < 0 ? "Esc" : style == 0 ? "B" : "Circle";
  char text[96];
  std::snprintf(text, sizeof(text), "Press %s again to exit to the desktop", button);
  const float size = io.DisplaySize.y * 0.032f;
  const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 c(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - size, c.y - ts.y * 0.5f - size * 0.6f),
                    ImVec2(c.x + ts.x * 0.5f + size, c.y + ts.y * 0.5f + size * 0.6f),
                    IM_COL32(10, 5, 12, 230), size * 0.3f);
  dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(240, 232, 220, 255), text);
}

void RecompOptionsPane::OnDraw(ImGuiIO& io) {
  pad_.Poll();
  DrawShaderProgress(io);
  UpdatePromptDevice(io, window_);
  DrawPromptGlyphs(io);
  UpdateQuitToDesktop(io);
  if (const char* aspect_test = Ng2AspectTestModeName()) {  // NG2 DIAG (ng2_aspect_cycle)
    ImFont* f = g_serif_font ? g_serif_font : ImGui::GetFont();
    const float size = io.DisplaySize.y * 0.03f;
    const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0.0f, aspect_test);
    const ImVec2 at((io.DisplaySize.x - ts.x) * 0.5f, io.DisplaySize.y * 0.02f);
    ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(at.x - 8, at.y - 4),
                                                  ImVec2(at.x + ts.x + 8, at.y + ts.y + 4),
                                                  IM_COL32(0, 0, 0, 170), 6.0f);
    ImGui::GetForegroundDrawList()->AddText(f, size, at, IM_COL32(255, 230, 120, 255), aspect_test);
  }
  // Mouse look (cursor capture, mouse buttons as game inputs) only while playing.
  rex::input::mnk::SetMouseLookActive(Ng2InGameplay() && !focused_ && !overlay_open_);
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
  if (settings_.check_updates) {
    bool start;
    {
      std::lock_guard<std::mutex> lock(update_->mutex);
      start = update_->phase == 0;
    }
    if (start) StartUpdateCheck();
  }
  PollUpdateReady(NowMs());
  if (!DrawUpdateReadyNotice(io, NowMs())) DrawUpdateNotice(io, NowMs());

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
  // The in-game overlay belongs to the in-game menu: if that menu is gone for a while (game
  // resumed, quit to title) close it, or a stale overlay_open_ showed the pane at the main
  // menu with no cursor and no way in (user report).
  if (overlay_open_ && Ng2IngameMenuHiddenMs() > 400) {
    REXLOG_INFO("RECOMP SETTINGS: in-game menu gone, closing the overlay");
    overlay_open_ = false;
  }
  // Main-menu pane: visible while the game draws the highlighted item. A frame-time spike or
  // a menu animation can skip that for a moment; only treat a gap over 400 ms as leaving, so
  // focus and the 250 ms entry guard aren't reset by hiccups (pane showed, A did nothing).
  const bool visible_now = overlay_open_ || pane_visible_();
  if (visible_now) pane_last_visible_ms_ = NowMs();
  if (!visible_now) {
    if (NowMs() - pane_last_visible_ms_ > 400) {
      pane_since_ms_ = 0;
      if (focused_) {
        REXLOG_INFO("RECOMP SETTINGS: pane closed (item no longer shown), focus released");
        focused_ = false;
        g_pane_focused = false;
      }
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
                    scroll_ = 0;
                    capture_bind_ = -1;
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
      if (s.fov_percent == 100) std::snprintf(buf, sizeof(buf), "Default");
      else std::snprintf(buf, sizeof(buf), "%d%%", s.fov_percent);
      step_row("Field of View", buf, s.fov_percent, 70, 150, 5,
               [&s](int v) { s.fov_percent = v; });
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
      {
        // Presets set the image rows below together (not the Framerate Cap); "Custom" when
        // they match none of them.
        struct Preset { const char* name; int res, aa, upscaler, sharpness; };
        static const Preset kPresets[] = {
            {"Low", 1, 0, 1, 8},
            {"Medium", 2, 1, 1, 8},
            {"High", 3, 1, 1, 8},
            {"Ultra", 4, 2, 1, 8},
        };
        int match = -1;
        for (int i = 0; i < 4; ++i) {
          const Preset& p = kPresets[i];
          if (s.resolution_scale == p.res && s.anti_aliasing == p.aa && s.upscaler == p.upscaler &&
              s.sharpness == p.sharpness) {
            match = i;
          }
        }
        // From Custom, Left / Right start at the preset nearest the current resolution.
        const int cur = match >= 0 ? match : std::clamp(s.resolution_scale, 1, 4) - 1;
        rows.push_back({"Preset", Kind::kStepper, match >= 0 ? kPresets[match].name : "Custom",
                        match < 0 || cur > 0, match < 0 || cur < 3, nullptr, nullptr, false,
                        [&s, cur, match](int dir, bool) {
                          if (!dir) return false;
                          const int v = match < 0 ? cur : std::clamp(cur + dir, 0, 3);
                          if (v == match) return false;
                          const Preset& p = kPresets[v];
                          s.resolution_scale = p.res;
                          s.anti_aliasing = p.aa;
                          s.upscaler = p.upscaler;
                          s.sharpness = p.sharpness;
                          return true;
                        }});
      }
      std::snprintf(buf, sizeof(buf), "%dx", s.resolution_scale);
      step_row("Internal Resolution", buf, s.resolution_scale, 1, 5, 1,
               [&s](int v) { s.resolution_scale = v; });
      const char* aa[] = {"Off", "FXAA", "FXAA Extreme"};
      step_row("Anti-Aliasing", aa[s.anti_aliasing], s.anti_aliasing, 0, 2, 1,
               [&s](int v) { s.anti_aliasing = v; });
      const char* upscalers[] = {"Bilinear", "FSR 1.0", "CAS"};
      step_row("Upscaling", upscalers[s.upscaler], s.upscaler, 0, 2, 1,
               [&s](int v) { s.upscaler = v; });
      std::snprintf(buf, sizeof(buf), "%d", s.sharpness);
      step_row("Sharpness", buf, s.sharpness, 0, 10, 1, [&s](int v) { s.sharpness = v; });
      if (s.upscaler == 0) {  // Bilinear has no sharpening pass
        rows.back().disabled = true;
        rows.back().can_dec = rows.back().can_inc = false;
        rows.back().change = nullptr;
      }
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
      {
        const char* prompts[] = {"Auto", "Xbox 360", "PS4", "PS5"};
        step_row("Button Prompts", prompts[std::clamp(s.button_prompts, 0, 3)], s.button_prompts, 0, 3,
                 1, [&s](int v) { s.button_prompts = v; });
      }
      break;
    }
    case 4: {  // Keyboard & Mouse
      pair_row("Keyboard & Mouse", "On", "Off", s.mnk_enabled,
               [&s](bool a) { s.mnk_enabled = a; });
      pair_row("Mouse Camera", "On", "Off", s.mouse_camera, [&s](bool a) { s.mouse_camera = a; });
      step_row("Mouse Sensitivity", FormatSens(s.mouse_sens), s.mouse_sens, 1, 50, 1,
               [&s](int v) { s.mouse_sens = v; });
      step_row("Vertical Sensitivity", FormatSens(s.mouse_sens_y), s.mouse_sens_y, 1, 50, 1,
               [&s](int v) { s.mouse_sens_y = v; });
      step_row("Aim Sensitivity", FormatSens(s.aim_sens), s.aim_sens, 1, 50, 1,
               [&s](int v) { s.aim_sens = v; });
      pair_row("Invert Mouse Y", "On", "Off", s.invert_mouse_y,
               [&s](bool a) { s.invert_mouse_y = a; });
      pair_row("Camera Auto-Center", "On", "Off", s.mouse_auto_center,
               [&s](bool a) { s.mouse_auto_center = a; });
      for (int i = 0; i < kBindCount; ++i) {
        RowDef r{kBindDefs[i].label, Kind::kBind, PrettyBind(s.binds[i])};
        r.bind = i;
        r.change = [this, i, now](int dir, bool accept) {
          if (!accept || dir) return false;
          capture_bind_ = i;  // next key / mouse button / wheel becomes the binding
          capture_armed_ = false;
          capture_until_ms_ = now + 8000;
          return false;
        };
        rows.push_back(std::move(r));
      }
      action_row("Reset Bindings", [&s] {
        s.binds = DefaultBinds();
        return true;
      });
      break;
    }
    case 6: {  // Cheats
      pair_row("Unlock Difficulties", "On", "Off", s.cheat_difficulties,
               [&s](bool a) { s.cheat_difficulties = a; });
      pair_row("Unlock Costumes", "On", "Off", s.cheat_costumes,
               [&s](bool a) { s.cheat_costumes = a; });
      break;
    }
    case 5: {  // System
      pair_row("Skip Intro", "On", "Off", s.skip_intro, [&s](bool a) { s.skip_intro = a; });
      pair_row("Skip Any Cutscene", "On", "Off", s.skip_any_cutscene,
               [&s](bool a) { s.skip_any_cutscene = a; });
      pair_row("Fast Loading", "On", "Off", s.fast_loading, [&s](bool a) { s.fast_loading = a; });
      pair_row("Pause in Background", "On", "Off", s.pause_in_background,
               [&s](bool a) { s.pause_in_background = a; });
      pair_row("Check for Updates", "On", "Off", s.check_updates,
               [&s](bool a) { s.check_updates = a; });
      {
        // Update: opens the launcher when a newer release exists; otherwise shows the state.
        int phase;
        openng2::ReleaseInfo info;
        {
          std::lock_guard<std::mutex> lock(update_->mutex);
          phase = update_->phase;
          info = update_->info;
        }
        const bool newer = phase == 2 && info.ok && openng2::IsNewer(info.version, openng2::kVersion);
        if (!ready_version_.empty()) {
          // The launcher has built the update and waits for the game to close.
          update_label_ = "Restart Now to Install openNG2 " + ready_version_;
          action_row(update_label_.c_str(), [this] {
            RestartForUpdate();
            return false;
          });
          action_row(restart_after_save_ ? "Restarting After the Next Save" : "Restart After the Next Save",
                     [this] {
                       RestartAfterNextSave();
                       return false;
                     });
          if (restart_after_save_) {
            rows.back().disabled = true;
            rows.back().change = nullptr;
          }
        } else if (newer) {
          update_label_ = "Update to openNG2 " + info.version;
          action_row(update_label_.c_str(), [this] {
            OpenUpdater();
            return false;
          });
        } else if (phase == 1) {
          update_label_ = "Checking for Updates...";
          action_row(update_label_.c_str(), [] { return false; });
          rows.back().disabled = true;
          rows.back().change = nullptr;
        } else if (phase == 2 && info.ok) {
          update_label_ = std::string("openNG2 ") + openng2::kVersion + " Is Up to Date";
          action_row(update_label_.c_str(), [] { return false; });
          rows.back().disabled = true;
          rows.back().change = nullptr;
        } else {
          update_label_ = phase == 2 ? "Update Check Failed - Retry" : "Check for Updates Now";
          const std::string error = info.error;
          action_row(update_label_.c_str(), [this, error, phase] {
            if (phase == 2 && !error.empty()) {
              status_ = error;
              status_until_ms_ = NowMs() + 6000;
            }
            StartUpdateCheck();
            return false;
          });
        }
      }
      action_row("Open Save Folder", [this] {
        ShellExecuteW(nullptr, L"open", user_data_root_.wstring().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);
        return false;
      });
      action_row("Open Log Folder", [] {  // for bug reports: logs\ng2_NNN.log, newest highest
        const std::wstring logs = (ExeDir() / "logs").wstring();
        ShellExecuteW(nullptr, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
      REXLOG_INFO("RECOMP SETTINGS: pane focused (main menu)");
    } else if (pad_.accept) {
      REXLOG_INFO("RECOMP SETTINGS: accept ignored, pane shown {} ms ago", now - pane_since_ms_);
    }
  } else if (capture_bind_ >= 0) {
    // Key capture for a binding row: wait until everything is released, then take the next
    // key, mouse button or wheel tick. Esc / controller B cancel, Delete clears.
    bool done = false;
    if (!OurWindowFocused() || now > capture_until_ms_ || capture_bind_ >= kBindCount) {
      done = true;
    } else {
      int pressed_vk = 0;
      bool any = false;
      for (int vk = 1; vk < 255; ++vk) {
        if (vk >= 0xA0 && vk <= 0xA5) continue;  // L/R variants; the generic code is reported
        if (GetAsyncKeyState(vk) & 0x8000) {
          any = true;
          if (!pressed_vk && !NameForVk(vk).empty()) pressed_vk = vk;
        }
      }
      if (!capture_armed_) {
        if (!any && !pad_.any_held && io.MouseWheel == 0.0f) capture_armed_ = true;
      } else if (io.MouseWheel != 0.0f) {
        s.binds[capture_bind_] = io.MouseWheel > 0.0f ? "WheelUp" : "WheelDown";
        changed = done = true;
      } else if (pressed_vk == VK_ESCAPE) {
        done = true;
      } else if (pressed_vk == VK_DELETE) {
        s.binds[capture_bind_].clear();
        changed = done = true;
      } else if (pressed_vk) {
        s.binds[capture_bind_] = NameForVk(pressed_vk);
        changed = done = true;
      } else if (pad_.back) {
        done = true;
      }
    }
    if (done) {
      capture_bind_ = -1;
      Ng2QueueSystemSound(kSoundCursor);
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
  // Keep the cursor row on screen (Category stays pinned at the top).
  const int row_count = int(rows.size());
  if (row_count > kVisibleRows) {
    if (row_ >= 1 && row_ < scroll_ + 1) scroll_ = row_ - 1;
    if (row_ > scroll_ + kVisibleRows - 1) scroll_ = row_ - (kVisibleRows - 1);
    scroll_ = std::clamp(scroll_, 0, row_count - kVisibleRows);
  } else {
    scroll_ = 0;
  }
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
    // "Back" hint in the current prompt style (keys, Xbox B, PS4 / PS5 Circle).
    const float hx = X(0.865f), hy = vp.y + vp.h * 0.853f, r = px * 0.45f;
    float label_x = hx + px * 0.75f;
    if (PromptsUseKeyboard()) {
      const char* key = "Esc";
      const ImVec2 ks = font->CalcTextSizeA(px * 0.55f, FLT_MAX, 0.0f, key);
      const float w = ks.x + px * 0.4f, h = px * 0.8f;
      dl->AddRectFilled(ImVec2(hx - w * 0.5f, hy - h * 0.5f), ImVec2(hx + w * 0.5f, hy + h * 0.5f),
                        IM_COL32(20, 14, 14, 200), h * 0.25f);
      dl->AddRect(ImVec2(hx - w * 0.5f, hy - h * 0.5f), ImVec2(hx + w * 0.5f, hy + h * 0.5f),
                  IM_COL32(220, 210, 200, 230), h * 0.25f, 0, std::max(1.0f, h * 0.06f));
      text_at(hx, hy, kWhite, key, true, px * 0.55f);
      label_x = hx + w * 0.5f + px * 0.3f;
    } else if (const int style = CurrentPromptStyle(); style == 2) {  // PS5: light, grey symbol
      dl->AddCircleFilled(ImVec2(hx, hy), r, IM_COL32(205, 205, 205, 255));
      dl->AddCircle(ImVec2(hx, hy), r, IM_COL32(120, 120, 120, 255), 0, std::max(1.0f, r * 0.10f));
      dl->AddCircle(ImVec2(hx, hy), r * 0.52f, IM_COL32(95, 95, 95, 255), 0, std::max(1.5f, r * 0.15f));
    } else if (style == 1) {  // PS4: dark, red symbol
      dl->AddCircleFilled(ImVec2(hx, hy), r, IM_COL32(32, 32, 34, 255));
      dl->AddCircle(ImVec2(hx, hy), r, IM_COL32(90, 90, 95, 255), 0, std::max(1.0f, r * 0.10f));
      dl->AddCircle(ImVec2(hx, hy), r * 0.52f, IM_COL32(232, 60, 72, 255), 0, std::max(1.5f, r * 0.15f));
    } else {  // Xbox 360 B
      dl->AddCircleFilled(ImVec2(hx, hy), r, IM_COL32(176, 22, 18, 255));
      text_at(hx, hy, kWhite, "B", true, px * 0.62f);
    }
    text_at(label_x, hy, kWhite, "Back", false, px);
  }

  // The row list can change after input (category switch), so rebuild-free drawing uses
  // the category that was current when the rows were built; a switch shows next frame.
  const int slots = std::min(row_count, kVisibleRows);
  for (int slot = 0; slot < slots; ++slot) {
    const int i = slot == 0 ? 0 : scroll_ + slot;
    if (i >= row_count) break;
    const RowDef& r = rows[i];
    const float y = Y(slot);
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
    } else if (r.kind == Kind::kBind) {
      const bool capturing = capture_bind_ == r.bind;
      const char* v = capturing ? "Press a key  (Esc cancel, Del clear)" : r.value.c_str();
      const float room = X(L.arrow_right_x) - X(L.arrow_left_x) + px * 2.0f;
      const float w = font->CalcTextSizeA(px, FLT_MAX, 0.0f, v).x;
      const float vpx = w > room ? std::max(px * room / w, px * 0.45f) : px;
      text_at(X(L.value_x), y, capturing ? kRedMarker : kWhite, v, true, vpx);
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
  // Scroll hints: more rows above / below.
  if (row_count > kVisibleRows) {
    const float t = px * 0.22f, ax = X(L.label_x) - px * 1.1f;
    if (scroll_ > 0) {
      const float ay = Y(1) - px * 0.15f;
      dl->AddTriangleFilled(ImVec2(ax, ay - t), ImVec2(ax - t, ay + t * 0.6f),
                            ImVec2(ax + t, ay + t * 0.6f), kGrey);
    }
    if (scroll_ + kVisibleRows < row_count) {
      const float ay = Y(slots - 1) + px * 0.15f;
      dl->AddTriangleFilled(ImVec2(ax, ay + t), ImVec2(ax - t, ay - t * 0.6f),
                            ImVec2(ax + t, ay - t * 0.6f), kGrey);
    }
  }
  float fy = Y(slots) + px * 0.2f;
  // Internal Resolution and Anti-Aliasing apply live (SDK cvar callbacks; a resolution
  // change rebuilds the GPU context with a brief hitch).
  const bool needs_restart = s.cheat_difficulties != startup_.cheat_difficulties ||
                             s.cheat_costumes != startup_.cheat_costumes ||
                             s.skip_intro != startup_.skip_intro;
  if (needs_restart) {
    text_at(X(L.label_x), fy, kGrey, "Some changes apply the next time the game starts.", false,
            note_px);
    fy += note_px * 1.5f;
  }
  // Hint for the row under the cursor.
  if (focused_ && row_ >= 0 && row_ < int(rows.size()) &&
      std::strcmp(rows[row_].label, "Preset") == 0) {
    text_at(X(L.label_x), fy, kGrey,
            "Low suits weaker PCs. Ultra renders at 4x and needs a strong GPU.", false, note_px);
    fy += note_px * 1.5f;
  }
  if (focused_ && row_ >= 0 && row_ < int(rows.size()) &&
      std::strcmp(rows[row_].label, "Internal Resolution") == 0) {
    text_at(X(L.label_x), fy, kGrey,
            "Render sizes above 1x may slow the game depending on your hardware.", false, note_px);
    fy += note_px * 1.5f;
  }
  if (focused_ && row_ >= 0 && row_ < int(rows.size()) &&
      std::strcmp(rows[row_].label, "Button Prompts") == 0) {
    text_at(X(L.label_x), fy, kGrey,
            "Auto matches the controller you use. Keyboard & mouse input still shows keys.", false,
            note_px);
    fy += note_px * 1.5f;
  }
  if (focused_ && row_ >= 0 && row_ < int(rows.size()) &&
      std::strncmp(rows[row_].label, "Update to", 9) == 0) {
    text_at(X(L.label_x), fy, kGrey,
            "Opens the openNG2 launcher, which updates and rebuilds the game. Keep playing until it asks.",
            false, note_px);
    fy += note_px * 1.5f;
  }
  if (now < status_until_ms_) text_at(X(L.label_x), fy, kWhite, status_.c_str(), false, note_px);
}

MouseCameraParams GetMouseCameraParams() {
  return {g_mnk_enabled.load() && g_mouse_camera.load(), g_mouse_sens_x.load(),
          g_mouse_sens_y.load(), g_mouse_invert_y.load(), g_mouse_auto_center.load()};
}

// ---- keyboard & mouse prompts -----------------------------------------------------------
// The game writes button prompts as @[pad:NAME] tags in its text (findings #54); with
// keyboard prompts on, ng2_hooks.cpp replaces them with these labels.
bool PromptsUseKeyboard() { return g_mnk_enabled.load() && g_prompts_keyboard.load(); }
uint32_t PromptLabelsVersion() { return g_prompt_labels_version.load(); }

std::string PromptLabel(const std::string& tag, bool gameplay) {
  std::array<std::string, kBindCount> b;
  {
    std::lock_guard<std::mutex> lock(g_prompt_binds_mutex);
    b = g_prompt_binds;
  }
  // First key of binding i (index into kBindDefs), pretty-printed.
  auto key = [&](int i) {
    std::string v = b[i];
    const size_t comma = v.find(',');
    if (comma != std::string::npos) v.resize(comma);
    return v.empty() ? std::string() : PrettyBind(v);
  };
  auto both = [&](int i, int j) { return key(i) + "/" + key(j); };
  enum { kFwd, kBack, kLeft, kRight, kJump, kQuick, kStrong, kProj, kNinpo, kBlock, kCenter,
         kInteract, kLook, kQuickMenu, kOptions, kPause };
  if (tag == "ActAttack") return key(kQuick);
  if (tag == "ActStrong" || tag == "ActNinpo0") return key(kStrong);
  if (tag == "ActProjectile" || tag == "ActNinpo1") return key(kProj);
  if (tag == "ActJump") return key(kJump);
  if (tag == "ActGuard") return key(kBlock);
  if (tag == "ActCenterCamera") return key(kCenter);
  if (tag == "ActLook") return key(kLook);
  if (tag == "Interact") return key(kInteract);
  if (tag == "Decide" || tag == "Proceed" || tag == "Skip") return "Enter";
  if (tag == "Cancel") return "Esc";
  if (tag == "Rd") return gameplay ? key(kJump) : "Enter";
  if (tag == "Rr") return gameplay ? key(kProj) : "Esc";
  if (tag == "Rl") return gameplay ? key(kQuick) : "F";
  if (tag == "Ru") return gameplay ? key(kStrong) : "R";
  if (tag == "L1") return key(kLook);
  if (tag == "R1") return key(kInteract);
  if (tag == "L2") return key(kBlock);
  if (tag == "R2") return key(kCenter);
  if (tag == "Start") return gameplay ? key(kPause) : "Enter";
  if (tag == "Select") return key(kOptions);
  if (tag == "Lu") return gameplay ? key(kQuickMenu) : "Up";
  if (tag == "Ld") return "Down";
  if (tag == "Ll") return "Left";
  if (tag == "Lr") return "Right";
  if (tag == "Lud") return "Up/Down";
  if (tag == "Llr") return "Left/Right";
  if (tag == "LS" || tag == "Stick") return key(kFwd) + key(kLeft) + key(kBack) + key(kRight);
  if (tag == "LSu") return key(kFwd);
  if (tag == "LSd") return key(kBack);
  if (tag == "LSud") return both(kFwd, kBack);
  if (tag == "LSlr") return both(kLeft, kRight);
  if (tag == "RS" || tag == "RSud" || tag == "RSlr") return "Mouse";
  return {};  // symbols (ninpo kanji, shuriken, plus, ...) keep their icon
}

void UpdatePlayStationPrompts(int want);  // ps_prompts.cpp

// Last input device: called once per frame from the pane (UI thread).
void UpdatePromptDevice(const ImGuiIO& io, rex::ui::Window* window) {
  bool pad = false;
  std::vector<int> families;
  const std::vector<XINPUT_GAMEPAD> pads = ConnectedPads(&families);
  // PlayStation prompts follow the pad used last (at start: the first one connected).
  static int pad_family = -1;
  if (pad_family < 0 && !families.empty()) pad_family = families[0];
  for (size_t i = 0; i < pads.size() && !pad; ++i) {
    const XINPUT_GAMEPAD& g = pads[i];
    pad = g.wButtons != 0 || g.bLeftTrigger > 40 || g.bRightTrigger > 40 ||
          std::abs(int(g.sThumbLX)) > 12000 || std::abs(int(g.sThumbLY)) > 12000 ||
          std::abs(int(g.sThumbRX)) > 12000 || std::abs(int(g.sThumbRY)) > 12000;
    if (pad) pad_family = families[i];
  }
  // Button Prompts setting: Auto follows the pad, otherwise fixed (0 Xbox, 1 PS4, 2 PS5).
  // Keyboard & mouse input still switches to key prompts either way (g_prompts_keyboard).
  const int forced = g_button_prompts.load();
  UpdatePlayStationPrompts(forced == 0 ? std::max(pad_family, 0) : forced - 1);
  // Keyboard & mouse count only on a fresh key press or real mouse travel: a key that reads as
  // held all the time (stuck VK, a key-holding tool, or a held modifier) or sub-pixel / warp
  // mouse jitter must not keep flipping prompts back to keyboard while the pad is idle.
  static bool key_was_down[256] = {};
  static float mouse_travel = 0.0f;
  bool kbm = false;
  int cause_vk = 0;
  static bool keys_seeded = false;  // keys already down at the first focused frame don't count
  const bool focused = OurWindowFocused();
  for (int vk = 1; vk < 255; ++vk) {
    if (vk >= 0x07 && vk <= 0x0F && vk != 0x08 && vk != 0x09 && vk != 0x0D) continue;
    if (vk >= 0xC3 && vk <= 0xDA) continue;  // VK_GAMEPAD_*: the controller, not the keyboard
    const bool down = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (!keys_seeded) {
      if (down && REXCVAR_GET(ng2_prompt_device_diag))
        REXLOG_INFO("NG2 prompt device: vk {:02X} already down at start", vk);
    } else if (down && !key_was_down[vk] && !kbm) {
      kbm = true;
      cause_vk = vk;
    }
    key_was_down[vk] = down;
  }
  if (focused) keys_seeded = true;
  bool cause_mouse = false;
  if (focused && !kbm) {
    const float d = std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y);
    mouse_travel = d > 0.0f ? mouse_travel + d : 0.0f;  // consecutive frames of motion
    if (mouse_travel > 24.0f || io.MouseWheel != 0.0f) {
      kbm = cause_mouse = true;
      mouse_travel = 0.0f;
    }
  }
  static bool first = true;
  if (first) {  // start with keyboard prompts when no controller is connected
    first = false;
    g_prompts_keyboard = pads.empty();
  }
  if (REXCVAR_GET(ng2_prompt_device_diag) && ((pad && g_prompts_keyboard) || (!pad && kbm && !g_prompts_keyboard))) {
    if (pad) REXLOG_INFO("NG2 prompt device: controller");
    else if (cause_mouse)
      REXLOG_INFO("NG2 prompt device: keyboard (mouse d={:.1f},{:.1f} wheel={:.1f})", io.MouseDelta.x,
                  io.MouseDelta.y, io.MouseWheel);
    else REXLOG_INFO("NG2 prompt device: keyboard (vk {:02X})", cause_vk);
  }
  if (pad) g_prompts_keyboard = false;
  else if (kbm) g_prompts_keyboard = true;
  // Mouse pointer: hidden while the controller is in use, back on mouse / key input
  // (GitHub #7: the pointer stayed on screen when playing with a pad).
  if (window) {
    using Vis = rex::ui::Window::CursorVisibility;
    static bool hidden_by_pad = false;
    static Vis saved = Vis::kAutoHidden;
    if (pad && !hidden_by_pad && window->GetCursorVisibility() != Vis::kHidden) {
      saved = window->GetCursorVisibility();
      window->SetCursorVisibility(Vis::kHidden);
      hidden_by_pad = true;
    } else if (!pad && kbm && hidden_by_pad) {
      if (window->GetCursorVisibility() == Vis::kHidden) window->SetCursorVisibility(saved);
      hidden_by_pad = false;
    }
  }
}

// Button icons the game draws as sprites (pause / inventory menu) are replaced by key caps
// drawn here (game thread pushes, UI thread draws; positions in the 1280x720 2D space).
namespace {
struct PromptGlyph {
  float x0, y0, x1, y1;
  std::string label;
  int64_t at_ms;
};
std::mutex g_prompt_glyphs_mutex;
std::vector<PromptGlyph> g_prompt_glyphs;
}  // namespace

void PushPromptGlyph(float x0, float y0, float x1, float y1, const std::string& label) {
  std::lock_guard<std::mutex> lock(g_prompt_glyphs_mutex);
  const int64_t now = NowMs();
  for (auto& g : g_prompt_glyphs) {
    if (std::abs(g.x0 - x0) < 1.0f && std::abs(g.y0 - y0) < 1.0f) {
      g = {x0, y0, x1, y1, label, now};
      return;
    }
  }
  g_prompt_glyphs.push_back({x0, y0, x1, y1, label, now});
}

void DrawPromptGlyphs(const ImGuiIO& io) {
  std::vector<PromptGlyph> glyphs;
  {
    std::lock_guard<std::mutex> lock(g_prompt_glyphs_mutex);
    const int64_t now = NowMs();
    g_prompt_glyphs.erase(std::remove_if(g_prompt_glyphs.begin(), g_prompt_glyphs.end(),
                                         [now](const PromptGlyph& g) { return now - g.at_ms > 120; }),
                          g_prompt_glyphs.end());
    glyphs = g_prompt_glyphs;
  }
  if (glyphs.empty()) return;
  const Viewport vp = GameViewport(io);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImFont* font = g_serif_font ? g_serif_font : ImGui::GetFont();
  for (const auto& g : glyphs) {
    const float sx = vp.w / 1280.0f, sy = vp.h / 720.0f;
    // The game draws its labels over the lower half of the button icon; keep the cap small
    // and in the icon's upper half so the label stays readable.
    const float icon_h = (g.y1 - g.y0) * sy;
    const float cx = vp.x + (g.x0 + g.x1) * 0.5f * sx, cy = vp.y + g.y0 * sy + icon_h * 0.30f;
    const float h = icon_h * 0.42f;
    const float px = h * 0.70f;
    const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, g.label.c_str());
    const float w = std::max(h, ts.x + px * 0.7f);
    const ImVec2 a(cx - w * 0.5f, cy - h * 0.5f), b(cx + w * 0.5f, cy + h * 0.5f);
    dl->AddRectFilled(a, b, IM_COL32(20, 14, 14, 175), h * 0.25f);
    dl->AddRect(a, b, IM_COL32(220, 210, 200, 220), h * 0.25f, 0, std::max(1.0f, h * 0.06f));
    dl->AddText(font, px, ImVec2(cx - ts.x * 0.5f, cy - ts.y * 0.5f), IM_COL32(245, 240, 232, 255),
                g.label.c_str());
  }
}

}  // namespace ng2

