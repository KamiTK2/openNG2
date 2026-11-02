// openNG2 launcher: players add their own Ninja Gaiden II files, the launcher checks them, then
// downloads a compiler, recompiles the game and installs a ready-to-play ng2.exe.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "inputs.h"
#include "pipeline.h"
#include "util.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace launcher;

constexpr char kVersion[] = "1.0";  // keep in sync with tools/make_release.py VERSION

namespace {

// ---- D3D11 -------------------------------------------------------------------------------------
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;

void CreateRtv() {
  ID3D11Texture2D* back = nullptr;
  g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
  g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
  back->Release();
}
bool CreateDevice(HWND hwnd) {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL got;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                             D3D11_SDK_VERSION, &sd, &g_swap, &g_device, &got, &g_context);
  if (hr == DXGI_ERROR_UNSUPPORTED) {
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
                                       &g_swap, &g_device, &got, &g_context);
  }
  if (FAILED(hr)) return false;
  CreateRtv();
  return true;
}

// ---- app state -----------------------------------------------------------------------------------
struct Slot {
  std::optional<Identified> result;
  std::future<Identified> pending;
  fs::path pending_path;
  bool checking() const { return pending.valid(); }
};
Slot g_game, g_update;
DlcSet g_dlc;  // DLC folder scan (plus single DLC files dropped on the window)
std::string g_notice;  // last message about a dropped file that fit no slot
fs::path g_install_dir;
bool g_license = false;
Pipeline g_pipeline;
HWND g_hwnd = nullptr;
std::mutex g_drop_mutex;
std::vector<fs::path> g_dropped;

fs::path SettingsFile() { return Pipeline::AppDataDir() / "launcher.ini"; }
void LoadSettings() {
  std::ifstream f(SettingsFile());
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("install=", 0) == 0) g_install_dir = fs::u8path(line.substr(8));
    if (line == "license=1") g_license = true;
  }
  if (g_install_dir.empty()) {
    wchar_t* docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &docs))) {
      g_install_dir = fs::path(docs) / "Games" / "openNG2";
    }
    CoTaskMemFree(docs);
  }
}
void SaveSettings() {
  std::error_code ec;
  fs::create_directories(SettingsFile().parent_path(), ec);
  std::ofstream f(SettingsFile());
  f << "install=" << PathText(g_install_dir) << "\n" << "license=" << (g_license ? 1 : 0) << "\n";
}

// Identify a path in the background; route it to a slot by what it turns out to be.
// A folder is the game (extracted folder / GOD package location) or, failing that, a DLC folder.
struct Routed {
  Identified id;
  bool dlc_folder = false;
  DlcSet dlc;
};
struct Routing {
  fs::path path;
  std::future<Routed> fut;
};
std::vector<Routing> g_routing;
void Add(const fs::path& p, bool as_dlc_folder = false) {
  Routing r;
  r.path = p;
  r.fut = std::async(std::launch::async, [p, as_dlc_folder] {
    Routed out;
    std::error_code ec;
    const bool dir = fs::is_directory(p, ec);
    if (!as_dlc_folder) {
      out.id = Identify(p);
      if (!dir || out.id.kind == Identified::Kind::kGame) return out;
    }
    out.dlc = ScanDlcFolder(p);
    if (as_dlc_folder || out.dlc.count() || !out.dlc.problems.empty()) out.dlc_folder = true;
    return out;
  });
  g_routing.push_back(std::move(r));
}
void Assign(Slot& s, const Identified& id) {
  s.result = id;
  s.pending = {};
}
void PumpRouting() {
  for (size_t i = 0; i < g_routing.size();) {
    auto& r = g_routing[i];
    if (r.fut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
      ++i;
      continue;
    }
    Routed routed = r.fut.get();
    g_notice.clear();
    if (routed.dlc_folder) {
      g_dlc = routed.dlc;
      g_routing.erase(g_routing.begin() + std::ptrdiff_t(i));
      continue;
    }
    const Identified& id = routed.id;
    switch (id.kind) {
      case Identified::Kind::kGame: Assign(g_game, id); break;
      case Identified::Kind::kUpdate: Assign(g_update, id); break;
      case Identified::Kind::kDlc:
        if (id.ok && id.dlc_index >= 0) {
          g_dlc.found[size_t(id.dlc_index)] = id;
        } else {
          g_notice = id.message;
        }
        break;
      default: g_notice = id.message; break;
    }
    g_routing.erase(g_routing.begin() + std::ptrdiff_t(i));
  }
}
bool Busy() { return !g_routing.empty(); }

// ---- dialogs --------------------------------------------------------------------------------------
std::vector<fs::path> PickPaths(bool folder, bool multi) {
  std::vector<fs::path> out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  opts |= FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0) | (multi ? FOS_ALLOWMULTISELECT : 0);
  dlg->SetOptions(opts);
  if (SUCCEEDED(dlg->Show(g_hwnd))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items))) {
      DWORD n = 0;
      items->GetCount(&n);
      for (DWORD i = 0; i < n; ++i) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &item))) {
          PWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            out.emplace_back(p);
            CoTaskMemFree(p);
          }
          item->Release();
        }
      }
      items->Release();
    }
  }
  dlg->Release();
  return out;
}

void CreateShortcut(const fs::path& exe) {
  IShellLinkW* link = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return;
  link->SetPath(exe.c_str());
  link->SetWorkingDirectory(exe.parent_path().c_str());
  link->SetIconLocation(exe.c_str(), 0);  // the oNG2 icon embedded in ng2.exe
  link->SetDescription(L"Ninja Gaiden II (openNG2)");
  IPersistFile* file = nullptr;
  wchar_t* desktop = nullptr;
  if (SUCCEEDED(link->QueryInterface(IID_PPV_ARGS(&file))) &&
      SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop))) {
    file->Save((fs::path(desktop) / L"Ninja Gaiden II (openNG2).lnk").c_str(), TRUE);
  }
  CoTaskMemFree(desktop);
  if (file) file->Release();
  link->Release();
}

// ---- UI ----------------------------------------------------------------------------------------------
ImFont* g_font = nullptr;
ImFont* g_font_bold = nullptr;
const ImVec4 kAccent(0.80f, 0.13f, 0.13f, 1.0f);
const ImVec4 kOk(0.35f, 0.80f, 0.40f, 1.0f);
const ImVec4 kBad(0.95f, 0.38f, 0.32f, 1.0f);
const ImVec4 kDim(0.76f, 0.76f, 0.80f, 1.0f);

void StatusDot(const ImVec4& color) {
  const float r = ImGui::GetFontSize() * 0.32f;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetFontSize() * 0.55f), r,
                                              ImGui::GetColorU32(color));
  ImGui::Dummy(ImVec2(r * 2.6f, ImGui::GetFontSize()));
  ImGui::SameLine();
}

void Heading(const char* text) {
  ImGui::Spacing();
  ImGui::PushFont(g_font_bold, ImGui::GetStyle().FontSizeBase * 1.15f);
  ImGui::TextUnformatted(text);
  ImGui::PopFont();
  ImGui::Separator();
}

// One input card: title, exact file names, status line, buttons.
void InputCard(const char* id, Slot& slot, const char* title, const char* names, bool required, bool allow_folder,
               bool game) {
  ImGui::PushID(id);
  ImGui::BeginGroup();
  const bool ok = slot.result && slot.result->ok;
  const bool checking = slot.checking() || (Busy() && !slot.result);
  StatusDot(ok ? kOk : (slot.result ? kBad : (required ? kAccent : kDim)));
  ImGui::PushFont(g_font_bold, 0.0f);
  ImGui::TextUnformatted(title);
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(required ? kAccent : kDim, required ? "required" : "optional");
  ImGui::Indent(ImGui::GetFontSize() * 1.2f);
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("%s", names);
  ImGui::PopStyleColor();
  if (slot.result) {
    ImGui::TextWrapped("%s", PathText(slot.result->path).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, slot.result->ok ? kOk : kBad);
    ImGui::TextWrapped("%s", slot.result->message.c_str());
    ImGui::PopStyleColor();
  } else if (checking) {
    ImGui::TextColored(kDim, "Checking...");
  }
  const bool locked = g_pipeline.running();
  ImGui::BeginDisabled(locked);
  if (ImGui::Button(game ? "Choose file..." : "Choose file...")) {
    for (const auto& p : PickPaths(false, false)) Add(p);
  }
  if (allow_folder) {
    ImGui::SameLine();
    if (ImGui::Button("Choose folder...")) {
      for (const auto& p : PickPaths(true, false)) Add(p);
    }
  }
  if (slot.result) {
    ImGui::SameLine();
    if (ImGui::Button("Remove")) slot.result.reset();
  }
  ImGui::EndDisabled();
  ImGui::Unindent(ImGui::GetFontSize() * 1.2f);
  ImGui::EndGroup();
  ImGui::Spacing();
  ImGui::PopID();
}

// DLC: one folder; the four known packages are listed with what was found.
void DrawDlcCard() {
  ImGui::PushID("dlc");
  ImGui::BeginGroup();
  const int n = g_dlc.count();
  StatusDot(n ? kOk : (g_dlc.problems.empty() ? kDim : kBad));
  ImGui::PushFont(g_font_bold, 0.0f);
  ImGui::TextUnformatted("Downloadable content");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::TextColored(kDim, "optional");
  const float indent = ImGui::GetFontSize() * 1.2f;
  ImGui::Indent(indent);
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("Choose the folder that holds your DLC packages. Any of these that are there will be added "
                     "(Mission Mode adds the mission mode to the main menu):");
  ImGui::PopStyleColor();
  for (size_t i = 0; i < kDlc.size(); ++i) {
    const bool have = g_dlc.found[i].ok;
    ImGui::Indent(indent * 0.6f);
    StatusDot(have ? kOk : ImVec4(0.35f, 0.35f, 0.38f, 1.0f));
    ImGui::TextColored(have ? ImVec4(1, 1, 1, 1) : kDim, "%s", kDlc[i].title);
    ImGui::SameLine();
    ImGui::TextColored(kDim, "  %s", kDlc[i].file_name);
    ImGui::Unindent(indent * 0.6f);
  }
  if (!g_dlc.folder.empty()) {
    ImGui::TextWrapped("%s", PathText(g_dlc.folder).c_str());
    if (n) {
      ImGui::TextColored(kOk, "%d of %zu found - verified.", n, kDlc.size());
    } else if (g_dlc.problems.empty()) {
      ImGui::TextColored(kBad, "No Ninja Gaiden II DLC packages in this folder.");
    }
  }
  for (const auto& p : g_dlc.problems) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", p.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::BeginDisabled(g_pipeline.running());
  if (ImGui::Button("Choose folder...")) {
    for (const auto& p : PickPaths(true, false)) Add(p, true);
  }
  if (n || !g_dlc.problems.empty() || !g_dlc.folder.empty()) {
    ImGui::SameLine();
    if (ImGui::Button("Remove")) g_dlc = DlcSet{};
  }
  ImGui::EndDisabled();
  ImGui::Unindent(indent);
  ImGui::EndGroup();
  ImGui::Spacing();
  ImGui::PopID();
}

void DrawFilesPage() {
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped(
      "openNG2 rebuilds Ninja Gaiden II (Xbox 360) as a native Windows game from your own copy. Add the files "
      "below - drag and drop them anywhere on this window, or use the buttons. Each file is checked as soon as "
      "you add it. Nothing is uploaded anywhere; everything happens on this PC.");
  ImGui::PopStyleColor();

  Heading("Required");
  InputCard(
      "game", g_game, "Ninja Gaiden II (Xbox 360, original disc release)",
      "Any one of:\n"
      "  - Games on Demand package: the file C1200E9B558D04454FA6 together with the folder "
      "C1200E9B558D04454FA6.data next to it (found in 544307D5\\00007000\\)\n"
      "  - a disc image (.iso) of the game disc\n"
      "  - a folder with the extracted disc contents (containing default.xex)",
      true, true, true);
  InputCard("update", g_update, "Title Update #2",
            "TU_1A461UL_000000G000000.00000000000G4  (\"Ninja Gaiden 2 Title Update #2\")\n"
            "Title Update #3 does not work - it is for a different release of the game.",
            true, false, false);

  Heading("Optional");
  DrawDlcCard();
  if (!g_notice.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", g_notice.c_str());
    ImGui::PopStyleColor();
  }

  Heading("Install");
  ImGui::TextWrapped("Install folder: %s", PathText(g_install_dir).c_str());
  ImGui::SameLine();
  if (ImGui::Button("Change...")) {
    auto p = PickPaths(true, false);
    if (!p.empty()) {
      g_install_dir = p[0];
      SaveSettings();
    }
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kDim);
  ImGui::TextWrapped("Needs about 22 GB of free space on that drive: the game (14 GB), the build tools "
                     "(about 6 GB, downloaded once into its build-tools folder) and temporary build files. "
                     "Free there now: %.0f GB.", double(FreeDiskBytes(g_install_dir)) / 1e9);
  ImGui::PopStyleColor();
  if (ImGui::Checkbox("##license", &g_license)) SaveSettings();
  ImGui::SameLine();
  ImGui::TextWrapped(
      "I accept the Microsoft Visual Studio Build Tools licence terms. Building needs Microsoft's C++ libraries, "
      "which are downloaded from Microsoft for you.");
  if (ImGui::SmallButton("Read the licence terms")) {
    ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/?LinkId=2086102", nullptr, nullptr, SW_SHOW);
  }

  ImGui::Spacing();
  const bool ready = g_game.result && g_game.result->ok && g_update.result && g_update.result->ok && g_license && !Busy();
  std::string why;
  if (!(g_game.result && g_game.result->ok)) why = "Add the game to continue.";
  else if (!(g_update.result && g_update.result->ok)) why = "Add Title Update #2 to continue.";
  else if (!g_license) why = "Accept the Microsoft licence terms to continue.";
  ImGui::BeginDisabled(!ready);
  ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.92f, 0.20f, 0.20f, 1.0f));
  if (ImGui::Button("Build openNG2", ImVec2(ImGui::GetFontSize() * 12, ImGui::GetFontSize() * 2.2f))) {
    BuildRequest req;
    req.game = *g_game.result;
    req.update = *g_update.result;
    for (const auto& d : g_dlc.found) {
      if (d.ok) req.dlc.push_back(d);
    }
    req.install_dir = g_install_dir;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    req.payload_dir = fs::path(exe).parent_path() / "payload";
    SaveSettings();
    g_pipeline.Start(req);
  }
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  if (!why.empty()) {
    ImGui::SameLine();
    ImGui::TextColored(kDim, "%s", why.c_str());
  }
}

void DrawBuildPage() {
  const auto steps = g_pipeline.steps();
  float overall = 0.0f;
  for (const auto& s : steps) {
    overall += (s.state == StepState::kDone || s.state == StepState::kSkipped) ? 1.0f
               : (s.state == StepState::kRunning && s.progress > 0) ? s.progress
                                                                     : 0.0f;
  }
  overall /= float(steps.size());
  Heading(g_pipeline.finished() ? (g_pipeline.succeeded() ? "openNG2 is ready" : "The build stopped")
                                : "Building openNG2");
  if (!g_pipeline.finished()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("The first build takes a while (downloads, extracting about 14 GB, then compiling). You can "
                       "leave this window open in the background.");
    ImGui::PopStyleColor();
  }
  ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
  ImGui::ProgressBar(overall, ImVec2(-1, 0));
  ImGui::PopStyleColor();
  ImGui::Spacing();
  for (const auto& s : steps) {
    const ImVec4 c = s.state == StepState::kDone ? kOk
                     : s.state == StepState::kSkipped ? kOk
                     : s.state == StepState::kFailed ? kBad
                     : s.state == StepState::kRunning ? kAccent
                                                      : kDim;
    StatusDot(c);
    ImGui::TextColored(s.state == StepState::kPending ? kDim : ImVec4(1, 1, 1, 1), "%s", s.title.c_str());
    if (!s.detail.empty() || s.state == StepState::kRunning) {
      ImGui::Indent(ImGui::GetFontSize() * 1.2f);
      if (s.state == StepState::kRunning) {
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
        if (s.progress >= 0) {
          ImGui::ProgressBar(s.progress, ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 0.6f), "");
        } else {
          ImGui::ProgressBar(-1.0f * float(ImGui::GetTime()), ImVec2(ImGui::GetFontSize() * 22, ImGui::GetFontSize() * 0.6f), "");
        }
        ImGui::PopStyleColor();
      }
      if (!s.detail.empty()) ImGui::TextColored(s.state == StepState::kFailed ? kBad : kDim, "%s", s.detail.c_str());
      ImGui::Unindent(ImGui::GetFontSize() * 1.2f);
    }
  }
  ImGui::Spacing();
  if (g_pipeline.running()) {
    if (ImGui::Button("Cancel")) g_pipeline.Cancel();
  } else if (g_pipeline.succeeded()) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    if (ImGui::Button("Play", ImVec2(ImGui::GetFontSize() * 8, ImGui::GetFontSize() * 2.0f))) {
      const fs::path exe = g_pipeline.exe_path();
      ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(), SW_SHOW);
    }
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
      ShellExecuteW(nullptr, L"open", g_pipeline.exe_path().parent_path().c_str(), nullptr, nullptr, SW_SHOW);
    }
    ImGui::SameLine();
    static bool shortcut_done = false;
    if (ImGui::Button(shortcut_done ? "Shortcut created" : "Create desktop shortcut")) {
      CreateShortcut(g_pipeline.exe_path());
      shortcut_done = true;
    }
    ImGui::TextColored(kDim, "Game: %s", PathText(g_pipeline.exe_path()).c_str());
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kBad);
    ImGui::TextWrapped("%s", g_pipeline.error().c_str());
    ImGui::PopStyleColor();
    if (ImGui::Button("Back")) g_pipeline.Reset();
    ImGui::SameLine();
    if (ImGui::Button("Open the build log")) {
      ShellExecuteW(nullptr, L"open", g_pipeline.log_path().c_str(), nullptr, nullptr, SW_SHOW);
    }
  }
  ImGui::Spacing();
  if (ImGui::CollapsingHeader("Details", g_pipeline.finished() && !g_pipeline.succeeded() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
    ImGui::BeginChild("log", ImVec2(-1, ImGui::GetFontSize() * 14), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& l : g_pipeline.log_tail(400)) ImGui::TextUnformatted(l.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
  }
}

void DrawUi() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  // A centred column keeps lines a comfortable length in wide windows.
  const float column = std::min(vp->WorkSize.x, ImGui::GetStyle().FontSizeBase * 46.0f);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - column) * 0.5f, vp->WorkPos.y));
  ImGui::SetNextWindowSize(ImVec2(column, vp->WorkSize.y));
  ImGui::Begin("openNG2", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PushFont(g_font_bold, ImGui::GetStyle().FontSizeBase * 1.9f);
  ImGui::TextColored(kAccent, "openNG2");
  ImGui::PopFont();
  ImGui::SameLine();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFontSize() * 0.55f);
  ImGui::TextColored(kDim, "Ninja Gaiden II for Windows - builder %s", kVersion);
  const bool build_view = g_pipeline.running() || g_pipeline.finished();
  if (build_view) {
    DrawBuildPage();
  } else {
    DrawFilesPage();
  }
  ImGui::End();
}

void ApplyStyle(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  ImGui::StyleColorsDark();
  s.WindowPadding = ImVec2(22, 18);
  s.FramePadding = ImVec2(12, 6);
  s.ItemSpacing = ImVec2(10, 9);
  s.ItemInnerSpacing = ImVec2(8, 6);
  s.FrameRounding = 5;
  s.GrabRounding = 5;
  s.ChildRounding = 5;
  s.ScrollbarSize = 14;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.075f, 0.085f, 1.0f);
  c[ImGuiCol_ChildBg] = ImVec4(0.05f, 0.05f, 0.06f, 1.0f);
  c[ImGuiCol_Button] = ImVec4(0.20f, 0.20f, 0.23f, 1.0f);
  c[ImGuiCol_ButtonHovered] = ImVec4(0.30f, 0.30f, 0.34f, 1.0f);
  c[ImGuiCol_ButtonActive] = ImVec4(0.55f, 0.10f, 0.10f, 1.0f);
  c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.16f, 0.19f, 1.0f);
  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_Separator] = ImVec4(0.25f, 0.25f, 0.28f, 1.0f);
  c[ImGuiCol_Header] = ImVec4(0.18f, 0.18f, 0.21f, 1.0f);
  c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.26f, 0.30f, 1.0f);
  s.ScaleAllSizes(scale);
  s.FontScaleDpi = 1.0f;
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam)) return TRUE;
  switch (msg) {
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        g_resize_w = LOWORD(lparam);
        g_resize_h = HIWORD(lparam);
      }
      return 0;
    case WM_DROPFILES: {
      HDROP drop = reinterpret_cast<HDROP>(wparam);
      const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      std::lock_guard<std::mutex> lock(g_drop_mutex);
      for (UINT i = 0; i < n; ++i) {
        wchar_t p[MAX_PATH * 4];
        if (DragQueryFileW(drop, i, p, UINT(std::size(p)))) g_dropped.emplace_back(p);
      }
      DragFinish(drop);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lparam);
      mm->ptMinTrackSize = POINT{760, 560};
      return 0;
    }
    case WM_CLOSE:
      if (g_pipeline.running() &&
          MessageBoxW(hwnd, L"A build is running. Stop it and close? It continues where it stopped next time.",
                      L"openNG2", MB_YESNO | MB_ICONQUESTION) != IDYES) {
        return 0;
      }
      break;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ImGui_ImplWin32_EnableDpiAwareness();
  LoadSettings();

  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)),
                 LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"openNG2Launcher", nullptr};
  RegisterClassExW(&wc);
  const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"openNG2 1.0 - Ninja Gaiden II builder",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, int(940 * dpi), int(900 * dpi), nullptr,
                           nullptr, inst, nullptr);
  const BOOL dark = TRUE;
  DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
  if (!CreateDevice(g_hwnd)) {
    MessageBoxW(nullptr, L"Direct3D 11 is not available on this PC.", L"openNG2", MB_ICONERROR);
    return 1;
  }
  ShowWindow(g_hwnd, SW_SHOWDEFAULT);
  UpdateWindow(g_hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
  ApplyStyle(scale);
  ImGui::GetStyle().FontSizeBase = 19.0f * scale;
  ImFontConfig cfg;
  cfg.OversampleH = 3;
  cfg.OversampleV = 2;
  cfg.RasterizerMultiply = 1.1f;  // slightly heavier strokes on the dark background
  g_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 19.0f * scale, &cfg);
  g_font_bold = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisb.ttf", 19.0f * scale, &cfg);
  if (!g_font) g_font = io.Fonts->AddFontDefault();
  if (!g_font_bold) g_font_bold = g_font;
  io.FontDefault = g_font;
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  bool done = false;
  while (!done) {
    MSG msg;
    // Sleep while idle; wake for input, or every 100 ms while work is in progress.
    const bool active = g_pipeline.running() || Busy();
    if (!active) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) done = true;
    }
    if (done) break;
    {
      std::lock_guard<std::mutex> lock(g_drop_mutex);
      if (!g_pipeline.running()) {
        for (const auto& p : g_dropped) Add(p);
      }
      g_dropped.clear();
    }
    PumpRouting();
    if (g_resize_w) {
      if (g_rtv) g_rtv->Release();
      g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
      g_resize_w = g_resize_h = 0;
      CreateRtv();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawUi();
    ImGui::Render();
    const float clear[4] = {0.075f, 0.075f, 0.085f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
    if (active) Sleep(30);
  }
  g_pipeline.Cancel();
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  if (g_rtv) g_rtv->Release();
  g_swap->Release();
  g_context->Release();
  g_device->Release();
  DestroyWindow(g_hwnd);
  CoUninitialize();
  return 0;
}
