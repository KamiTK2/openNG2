// ng2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <fstream>
#include <memory>

#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>

#include "crash_handler.h"
#ifdef NG2_TRACE_ALL
#include "ng2_trace_all.h"
#endif
#include "recomp_settings.h"
#include "shader_precompile.h"

void Ng2InstallDataPatches();  // ng2_hooks.cpp
bool Ng2RecompPaneVisible();   // ng2_hooks.cpp
bool Ng2IngameRecompRowSelected();  // ng2_hooks.cpp

class Ng2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Ng2App>(new Ng2App(ctx, "ng2",
        PPCImageConfig));
  }

  // Runs right after ng2.toml is loaded and before the runtime / GPU plugin start, so
  // the user's RECOMP SETTINGS override the shipped defaults.
  void OnPostInitLogging() override {
    InstallCrashHandler();
#ifdef NG2_TRACE_ALL
    ng2trace::StartTriggerThread();  // NG2 DIAG
#endif
    ng2::RecompSettings::ApplyStartup(RecompSettingsPath());
  }

  // Launcher installs put the game data in a "game" folder next to ng2.exe; use it when no
  // --game_data_root is given, so the exe starts with a plain double-click.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) {
      const auto local = rex::filesystem::GetExecutableFolder() / "game";
      std::error_code ec;
      if (std::filesystem::is_directory(local, ec)) paths.game_data_root = local;
    }
  }

  // Default to the Xenos GPU plugin (staged next to ng2.exe by CMake);
  // --gpu_plugin on the command line or in ng2.toml still overrides it.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  }

  void OnPostSetup() override {
    // Patch game data as it's read (Options "Copyright Information" -> "Recomp Settings").
    Ng2InstallDataPatches();
    // The game sees no input while the Recomp Settings pane has focus (or while the
    // button that left it is still held).
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      input->SetActiveCallback([] { return !ng2::RecompSettingsBlocksGameInput(); });
    }
    // Controller deadzone / camera inversion / vibration strength from RECOMP SETTINGS.
    ng2::InstallRecompInputFilters();
  }

  // DLC: install every package in <game data>\dlc that isn't installed yet (extracted into the
  // user data folder like a console's marketplace content), before the game starts and
  // enumerates content. The launcher only has to put the player's DLC files there.
  // Shaders: add every shader/pipeline from the shipped manifest that the cache lacks, so the
  // SDK builds them all before the game starts (findings #58).
  void OnPreLaunchModule() override {
    InstallDlc();
    if (auto* ks = REX_KERNEL_STATE()) {
      ng2::PrepareShaderCache(rex::filesystem::GetExecutableFolder() / "shaders", cache_root(),
                              ks->title_id());
    }
  }

  // Shader storage progress (drawn by the settings overlay until the game starts).
  void OnShaderStorageProgress(int stage, uint32_t done, uint32_t total) override {
    ng2::SetShaderProgress(stage, done, total);
    if (stage == 0 && total == 0 && done == 0) {  // first call, on the UI thread
      ng2::SetShaderProgressActive(true);
      if (auto* d = imgui_drawer(); d && d->presenter()) d->presenter()->RequestUIPaintFromUIThread();
    }
  }
  void OnPostLaunchModule(rex::system::XThread*) override { ng2::SetShaderProgressActive(false); }

  void OnConfigureFonts(ImFontAtlas* atlas) override { ng2::LoadRecompSettingsFonts(atlas); }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Options > Recomp Settings: settings rows drawn inline in the game's pane; from the
    // in-game menu's Recomp Settings row they open as a full-screen panel.
    recomp_pane_ = std::make_unique<ng2::RecompOptionsPane>(
        drawer, window(), RecompSettingsPath(), user_data_root(),
        [] { return Ng2RecompPaneVisible(); }, [] { return Ng2IngameRecompRowSelected(); });
  }

  void OnShutdown() override { recomp_pane_.reset(); }

 private:
  std::filesystem::path RecompSettingsPath() const {
    return user_data_root() / "recomp_settings.toml";
  }

  void InstallDlc() {
    auto* ks = REX_KERNEL_STATE();
    auto* content = ks ? ks->content_manager() : nullptr;
    const auto dlc_dir = game_data_root() / "dlc";
    std::error_code ec;
    if (!content || !std::filesystem::is_directory(dlc_dir, ec)) return;
    const auto markers = user_data_root() / "dlc_installed";
    std::filesystem::create_directories(markers, ec);
    for (const auto& e : std::filesystem::directory_iterator(dlc_dir, ec)) {
      if (!e.is_regular_file()) continue;
      const auto marker = markers / e.path().filename();
      if (!std::filesystem::exists(marker, ec)) {
        REXLOG_INFO("NG2 DLC: installing {}", e.path().filename().string());
        const auto result = content->InstallContent(e.path());
        if (result != 0) {
          REXLOG_WARN("NG2 DLC: install of {} failed ({:#x})", e.path().filename().string(),
                      uint32_t(result));
          continue;
        }
        std::ofstream(marker) << "installed\n";
        REXLOG_INFO("NG2 DLC: installed {}", e.path().filename().string());
      }
      FixDlcLicense(e.path(), ks->title_id());
    }
  }

  // The runtime's installer only counts STFS license entries whose flags are non-zero, so a
  // package whose only entry is {id 0xFFFFFFFFFFFFFFFF (anyone), bits 1, flags 0} - NG2's
  // Mission Mode - gets no license mask in its .header, and the game treats it as
  // unlicensed and hides it (findings #43). Append the mask from every entry with a licensee.
  void FixDlcLicense(const std::filesystem::path& package, uint32_t title_id) {
    char title[16];
    std::snprintf(title, sizeof(title), "%08X", title_id);
    const auto header = user_data_root() / "0000000000000000" / title / "Headers" / "00000002" /
                        (package.filename().string() + ".header");
    std::error_code ec;
    const auto size = std::filesystem::file_size(header, ec);
    if (ec || size != sizeof(rex::system::xam::XCONTENT_AGGREGATE_DATA)) return;  // has a mask
    std::ifstream in(package, std::ios::binary);
    uint8_t lic[16 * 16] = {};
    in.seekg(0x22C);
    if (!in.read(reinterpret_cast<char*>(lic), sizeof(lic))) return;
    auto be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    uint32_t mask = 0;
    for (int i = 0; i < 16; ++i) {
      const uint8_t* entry = lic + 16 * i;
      if (be32(entry) || be32(entry + 4)) mask |= be32(entry + 8);  // licensee id set
    }
    if (!mask) return;
    std::ofstream(header, std::ios::binary | std::ios::app)
        .write(reinterpret_cast<const char*>(&mask), sizeof(mask));
    REXLOG_INFO("NG2 DLC: {} license mask {:#x} added", package.filename().string(), mask);
  }

  std::unique_ptr<ng2::RecompOptionsPane> recomp_pane_;
};

