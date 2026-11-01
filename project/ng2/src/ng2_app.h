// ng2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <memory>

#include <rex/input/input_system.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "crash_handler.h"
#ifdef NG2_TRACE_ALL
#include "ng2_trace_all.h"
#endif
#include "recomp_settings.h"

void Ng2InstallDataPatches();  // ng2_hooks.cpp
bool Ng2RecompPaneVisible();   // ng2_hooks.cpp

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

  void OnConfigureFonts(ImFontAtlas* atlas) override { ng2::LoadRecompSettingsFonts(atlas); }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Options > Recomp Settings: settings rows drawn inline in the game's pane.
    recomp_pane_ = std::make_unique<ng2::RecompOptionsPane>(
        drawer, window(), RecompSettingsPath(), user_data_root(),
        [] { return Ng2RecompPaneVisible(); });
  }

  void OnShutdown() override { recomp_pane_.reset(); }

 private:
  std::filesystem::path RecompSettingsPath() const {
    return user_data_root() / "recomp_settings.toml";
  }

  std::unique_ptr<ng2::RecompOptionsPane> recomp_pane_;
};

