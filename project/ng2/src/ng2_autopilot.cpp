// NG2 DIAG: unattended test autopilot (cvar ng2_autopilot, default off).
// Presses the pad in-process (through the input state filter, so no window focus or key
// timing is involved) and decides from what the game is showing, not from fixed waits:
//   "menu"     stop at the main menu
//   "options"  open Options from the main menu
//   "gameplay" load the save: main menu -> ng2_autopilot_menu_item -> ... -> HUD up
//   "cutscene" NEW GAME, confirming with A only, until a movie or real-time cutscene runs
// Every state change is logged as "NG2 AUTOPILOT" with milliseconds since launch, and after
// the goal it logs the presented frame rate; tools/native_test.ps1 reads these lines.
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_STRING(ng2_autopilot, "", "NG2",
                      "NG2 DIAG: drive the game unattended to menu / options / gameplay");
REXCVAR_DEFINE_INT32(ng2_autopilot_pulse_ms, 50, "NG2",
                     "NG2 DIAG: autopilot press and release length");
REXCVAR_DEFINE_INT32(ng2_autopilot_menu_item, 1, "NG2",
                     "NG2 DIAG: main menu row the gameplay route selects (1 = LOAD GAME)");

REXCVAR_DEFINE_INT32(ng2_autopilot_list_down, 0, "NG2",
                     "NG2 DIAG: Down presses on the first list after the main menu (mission or save)");
REXCVAR_DEFINE_BOOL(ng2_autopilot_turbo, true, "NG2",
                    "NG2 DIAG: guest vblank unthrottled until the autopilot reaches its goal");

REXCVAR_DEFINE_INT32(ng2_autopilot_scale_change, 0, "NG2",
                     "NG2 DIAG: 4 s after the goal, set resolution_scale live to this (0 = off)");

REXCVAR_DEFINE_STRING(ng2_autopilot_script, "", "NG2",
                      "NG2 DIAG: steps on the lists after the main menu, e.g. wwadddda");
REXCVAR_DEFINE_BOOL(ng2_autopilot_open_pane, false, "NG2",
                    "NG2 DIAG: after the options goal, open the Recomp Settings pane");

REXCVAR_DEFINE_INT32(ng2_autopilot_stage, -1, "NG2",
                     "NG2 DIAG: load this stage id in place of the requested one (-1 = off)");
REXCVAR_DEFINE_INT32(ng2_autopilot_freeze_frame, 0, "NG2",
                     "NG2 DIAG: freeze the guest on this swap after the goal (0 = off)");
REXCVAR_DEFINE_BOOL(ng2_autopilot_story_mode, false, "NG2",
                    "NG2 DIAG: forced stage from Mission Mode runs in story mode");
REXCVAR_DEFINE_INT32(ng2_autopilot_entry, 0xFFFF, "NG2", "NG2 DIAG: entry for ng2_autopilot_stage");

bool Ng2InGameplay();          // ng2_hooks.cpp
int Ng2MainMenuCursor();       // ng2_hooks.cpp
bool Ng2LoadingScreenUp();     // ng2_hooks.cpp
bool Ng2ChapterScreenUp();     // ng2_hooks.cpp
bool Ng2CutsceneUp();          // ng2_hooks.cpp
uint16_t Ng2GuestU16(uint32_t address);  // ng2_hooks.cpp

namespace {

constexpr uint16_t kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kStart = 0x0010, kA = 0x1000;
constexpr int64_t kGoalHoldMs = 500;  // the goal state must stay up this long
constexpr int64_t kFpsWindowMs = 2000;

int64_t NowMs() { return int64_t(GetTickCount64()); }

struct Autopilot {
  int64_t start_ms = 0;
  std::string last_state;
  bool done = false;
  int64_t goal_since_ms = 0;
  uint16_t pressing = 0;
  int64_t press_end_ms = 0;
  int64_t next_press_ms = 0;
  uint32_t presses = 0;
  int left_menu = 0;
  bool confirm_left = false;
  int list_down_done = 0;
  int64_t left_menu_ms = 0;
  int64_t done_ms = 0;
  size_t script_pos = 0;
  bool menu_seen = false;
} g_ap;


void Note(const char* state) {
  if (g_ap.last_state == state) return;
  g_ap.last_state = state;
  REXLOG_INFO("NG2 AUTOPILOT t={} state={} presses={}", NowMs() - g_ap.start_ms, state,
              g_ap.presses);
}

void Pulse(int64_t now, uint16_t buttons) {
  if (g_ap.pressing || now < g_ap.next_press_ms) return;
  const int64_t pulse = REXCVAR_GET(ng2_autopilot_pulse_ms);
  g_ap.pressing = buttons;
  g_ap.press_end_ms = now + pulse;
  g_ap.next_press_ms = now + 2 * pulse;
  ++g_ap.presses;
}

void Finish(const char* goal) {
  g_ap.done = true;
  g_ap.done_ms = NowMs();
  REXLOG_INFO("NG2 AUTOPILOT t={} GOAL {} presses={}", NowMs() - g_ap.start_ms, goal,
              g_ap.presses);
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_82444FB0);

// Stage load: sub_82444FB0 starts a loaded stage from the u16 at 0x842B033E. Rewriting the first
// story load sends the game's own load path (assets, chapter data) to another stage.
extern "C" REX_FUNC(sub_82444FB0) {
  static bool rewritten = false;
  uint8_t* const id = base + 0x842B033E;
  const uint32_t from = (uint32_t(id[0]) << 8) | id[1];
  const int stage = REXCVAR_GET(ng2_autopilot_stage);
  REXLOG_INFO("NG2 AUTOPILOT stage start {:#x} entry {:#x}", from, (uint32_t(id[-2]) << 8) | id[-1]);
  if (stage >= 0 && !rewritten && from < 0x200) {
    rewritten = true;
    id[0] = uint8_t(stage >> 8);
    id[1] = uint8_t(stage);
    const int entry = REXCVAR_GET(ng2_autopilot_entry);
    id[-2] = uint8_t(entry >> 8);
    id[-1] = uint8_t(entry);
    if (REXCVAR_GET(ng2_autopilot_story_mode)) std::memset(base + 0x84C3F064, 0, 4);
  }
  __imp__sub_82444FB0(ctx, base);
}

void Ng2AutopilotPad(uint32_t user_index, rex::input::X_INPUT_STATE* st) {
  const std::string route = REXCVAR_GET(ng2_autopilot);
  if (route.empty() || user_index != 0) return;
  const int64_t now = NowMs();
  if (g_ap.done) {
    static bool scale_changed = false;
    if (!scale_changed && REXCVAR_GET(ng2_autopilot_scale_change) > 0 && g_ap.done_ms &&
        now - g_ap.done_ms >= 4000) {
      scale_changed = true;
      rex::cvar::SetFlagByName("resolution_scale",
                               std::to_string(REXCVAR_GET(ng2_autopilot_scale_change)));
    }
    // Options route: open the Recomp Settings pane (an ImGui overlay) for a screenshot.
    static int pane_step = 0;
    if (route == "options" && REXCVAR_GET(ng2_autopilot_open_pane) && pane_step < 6 &&
        !g_ap.pressing && now >= g_ap.next_press_ms && now - g_ap.done_ms >= 800) {
      Pulse(now, pane_step < 5 ? kDpadDown : kA);
      g_ap.next_press_ms = now + 250;
      ++pane_step;
    }
    if (g_ap.pressing && now >= g_ap.press_end_ms) g_ap.pressing = 0;
    if (g_ap.pressing) st->gamepad.buttons = uint16_t(uint16_t(st->gamepad.buttons) | g_ap.pressing);
    return;
  }
  if (!g_ap.start_ms) g_ap.start_ms = now;
  // This route confirms a save overwrite, so it refuses anything but the test sandbox.
  if (route == "cutscene" && !std::strstr(GetCommandLineA(), "openNG2-test")) {
    REXLOG_ERROR("NG2 AUTOPILOT: cutscene route needs the sandbox user data, stopping");
    g_ap.done = true;
    return;
  }

  const int cursor = Ng2MainMenuCursor();
  const bool gameplay = Ng2InGameplay();
  const bool loading = Ng2LoadingScreenUp();
  const bool cutscene_route = route == "cutscene";
  const bool goal_up = route == "menu"      ? cursor >= 0
                       : route == "options" ? g_ap.left_menu == 2
                       : cutscene_route     ? g_ap.left_menu && Ng2CutsceneUp()
                                            : gameplay;
  // A stage forced through the Mission Mode path shows its chapter card before the scene, and the
  // gameplay flag from the HUD can still be set under it.
  if (REXCVAR_GET(ng2_autopilot_stage) >= 0 && (Ng2ChapterScreenUp() || loading)) {
    g_ap.goal_since_ms = 0;
    if (Ng2ChapterScreenUp()) Pulse(now, kA | kStart);
    Note("stage_card");
    if (g_ap.pressing && now >= g_ap.press_end_ms) g_ap.pressing = 0;
    if (g_ap.pressing) st->gamepad.buttons = uint16_t(uint16_t(st->gamepad.buttons) | g_ap.pressing);
    return;
  }
  if (goal_up) {
    if (!g_ap.goal_since_ms) g_ap.goal_since_ms = now;
    if (now - g_ap.goal_since_ms >= kGoalHoldMs) {
      Finish(route.c_str());
      return;
    }
  } else {
    g_ap.goal_since_ms = 0;
  }

  if (loading) {
    Note("loading");
  } else if (cursor >= 0) {
    Note("main_menu");
    g_ap.menu_seen = true;
    g_ap.left_menu_ms = 0;
    const int target = route == "options"    ? 5
                       : cutscene_route      ? 0
                                             : REXCVAR_GET(ng2_autopilot_menu_item);
    if ((route == "options" || cutscene_route) && cursor == target) g_ap.left_menu = 1;
    if (route != "menu") {
      if (cursor < target) Pulse(now, kDpadDown);
      else if (cursor > target) Pulse(now, kDpadUp);
      else Pulse(now, kA);
    }
  } else if (gameplay) {
    Note("gameplay");
  } else if (route == "options" && g_ap.left_menu) {
    g_ap.left_menu = 2;
    Note("options");
  } else {
    // Title, save list, costume selection, chapter card, cutscene: A confirms, Start skips.
    Note(Ng2ChapterScreenUp() ? "chapter_or_slots" : "other");
    if (!g_ap.left_menu_ms) g_ap.left_menu_ms = now;
    const bool in_list_wait = now - g_ap.left_menu_ms < 800;
    // Scripted lists after the main menu: 'd' Down, 'a' A, 'l' Left, 'r' Right, 'w' 300 ms wait.
    const std::string script = REXCVAR_GET(ng2_autopilot_script);
    if (g_ap.menu_seen && g_ap.script_pos < script.size()) {
      if (!in_list_wait && !g_ap.pressing && now >= g_ap.next_press_ms) {
        const char step = script[g_ap.script_pos++];
        if (step == 'd') Pulse(now, kDpadDown);
        else if (step == 'a') Pulse(now, kA);
        else if (step == 'l') Pulse(now, kDpadLeft);
        else if (step == 'r') Pulse(now, 0x0008);
        g_ap.next_press_ms = now + (step == 'w' ? 300 : 200);
      }
    } else if (g_ap.list_down_done < REXCVAR_GET(ng2_autopilot_list_down)) {
      if (!in_list_wait && !g_ap.pressing && now >= g_ap.next_press_ms) {
        Pulse(now, kDpadDown);
        g_ap.next_press_ms = now + 150;
        ++g_ap.list_down_done;
      }
    } else if (!goal_up && !cutscene_route) {
      Pulse(now, kA | kStart);
    } else if (!goal_up && !g_ap.pressing && now >= g_ap.next_press_ms) {
      // New Game asks to overwrite a story save for that difficulty: Left picks Yes, then A. Left
      // does nothing on the screens without that prompt, so this copes with both. Only ever run
      // on the sandbox user data (tools/native_test.ps1).
      Pulse(now, g_ap.confirm_left ? kDpadLeft : kA);
      g_ap.confirm_left = !g_ap.confirm_left;
      g_ap.next_press_ms = now + 400;
    }
  }

  if (g_ap.pressing && now >= g_ap.press_end_ms) g_ap.pressing = 0;
  if (g_ap.pressing) st->gamepad.buttons = uint16_t(uint16_t(st->gamepad.buttons) | g_ap.pressing);
}

void Ng2AutopilotOnPresent() {
  if (REXCVAR_GET(ng2_autopilot).empty()) return;
  {
    static uint32_t stage_seen = 0x10000;
    const uint32_t stage = uint32_t(Ng2GuestU16(0x84C3F08C));
    if (stage != stage_seen) {
      stage_seen = stage;
      REXLOG_INFO("NG2 AUTOPILOT stage {:#x} at t={}", stage, NowMs() - g_ap.start_ms);
    }
  }
  {
    static int cutscene = -1;
    const int up = Ng2CutsceneUp() ? 1 : 0;
    if (up != cutscene) {
      cutscene = up;
      REXLOG_INFO("NG2 AUTOPILOT cutscene {} at t={}", up ? "start" : "end", NowMs() - g_ap.start_ms);
    }
  }
  // Frame-paced screens (fades, chapter card) run unthrottled until the goal, as Fast Loading
  // does for the loading screen; measurements after the goal run at the normal 60 Hz.
  static bool normal_vsync_restored = false;
  if (REXCVAR_GET(ng2_autopilot_turbo) && !g_ap.done) {
    if (rex::cvar::GetFlagByName("vsync") != "false") rex::cvar::SetFlagByName("vsync", "false");
  } else if (REXCVAR_GET(ng2_autopilot_turbo) && !normal_vsync_restored) {
    normal_vsync_restored = true;
    rex::cvar::SetFlagByName("vsync", "true");
  }
  // Same-frame comparison: stop the guest on the Nth swap of the scene, counted in swaps from the
  // first swap showing it, so that frame stays on screen on every backend. Needs N above the goal
  // hold and turbo off. The script's timeout ends the run.
  const int freeze = REXCVAR_GET(ng2_autopilot_freeze_frame);
  static int scene_swaps = 0;
  const bool scene = REXCVAR_GET(ng2_autopilot) == "cutscene" ? Ng2CutsceneUp() : Ng2InGameplay();
  if (freeze > 0 && scene_swaps == freeze - 2) rex::cvar::SetFlagByName("ng2_native_dump_frame", "-1");
  if (freeze > 0 && (scene_swaps > 0 || scene) && ++scene_swaps == freeze) {
    REXLOG_INFO("NG2 AUTOPILOT frozen at frame {}", freeze);
    for (;;) Sleep(1000);
  }
  if (!g_ap.done) return;
  static int64_t window_ms = 0;
  static uint32_t frames = 0;
  const int64_t now = NowMs();
  if (!window_ms) window_ms = now;
  ++frames;
  if (now - window_ms >= kFpsWindowMs) {
    REXLOG_INFO("NG2 AUTOPILOT fps {:.1f}", frames * 1000.0 / double(now - window_ms));
    window_ms = now;
    frames = 0;
  }
}
