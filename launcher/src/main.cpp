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
#include <cstdarg>
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
#include "update_check.h"
#include "updater.h"
#include "util.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;
using namespace launcher;

using openng2::kVersion;  // root VERSION file (also read by tools/make_release.py)

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
bool g_check_updates = true;
Pipeline g_pipeline;
SelfUpdater g_updater;
// Command line: --update (opened by the game's Update row: install a newer launcher, then
// rebuild), --updated (just replaced by a newer launcher: rebuild the installed game).
bool g_arg_update = false, g_arg_updated = false;
bool g_auto_done = false;  // the automatic step for those has run
std::string g_installed;   // installed openNG2 version ("" = none)
bool g_can_rebuild = false;
HWND g_hwnd = nullptr;
std::mutex g_drop_mutex;
std::vector<fs::path> g_dropped;
bool g_shortcut_done = false;  // "Desktop shortcut" pressed for the current build
float g_pending_dpi = 0.0f;     // new DPI scale from WM_DPICHANGED, applied in the main loop

fs::path SettingsFile() { return Pipeline::AppDataDir() / "launcher.ini"; }
void LoadSettings() {
  std::ifstream f(SettingsFile());
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("install=", 0) == 0) g_install_dir = fs::u8path(line.substr(8));
    if (line == "license=1") g_license = true;
    if (line == "check_updates=0") g_check_updates = false;
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
  f << "install=" << PathText(g_install_dir) << "\n" << "license=" << (g_license ? 1 : 0) << "\n"
    << "check_updates=" << (g_check_updates ? 1 : 0) << "\n";
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
// A fixed-size window (kClientW x kClientH logical px): a brand panel on the left (logo, update
// status, help links) and one screen at a time on the right - a four-step setup guide, the
// installed game (Play / Update), or the build. Nothing scrolls. Sizes are in logical pixels
// times g_scale (DPI).
constexpr float kClientW = 1040.0f, kClientH = 680.0f, kSideW = 330.0f;
ImFont* g_font = nullptr;       // Segoe UI (Variable on Windows 11)
ImFont* g_font_bold = nullptr;  // Segoe UI Semibold
ImFont* g_font_mono = nullptr;  // Cascadia Mono / Consolas
ID3D11ShaderResourceView* g_logo = nullptr;
float g_scale = 1.0f;
int g_ui_preview = -1;  // --ui-preview=N: 0-3 setup steps, 4 home, 5 building, 6 done, 7 failed (screenshots)

// Palette.
const ImVec4 kBg(0.067f, 0.067f, 0.078f, 1.0f);
const ImVec4 kPanel(0.102f, 0.102f, 0.118f, 1.0f);
const ImVec4 kPanelHi(0.137f, 0.137f, 0.157f, 1.0f);
const ImVec4 kBorder(0.200f, 0.200f, 0.230f, 1.0f);
const ImVec4 kText(0.945f, 0.945f, 0.953f, 1.0f);
const ImVec4 kDim(0.660f, 0.660f, 0.700f, 1.0f);
const ImVec4 kFaint(0.430f, 0.430f, 0.470f, 1.0f);
const ImVec4 kAccent(0.800f, 0.067f, 0.180f, 1.0f);
const ImVec4 kAccentHi(0.900f, 0.150f, 0.260f, 1.0f);
const ImVec4 kOk(0.290f, 0.780f, 0.420f, 1.0f);
const ImVec4 kWarn(0.930f, 0.690f, 0.250f, 1.0f);
const ImVec4 kBad(0.960f, 0.380f, 0.380f, 1.0f);

float S(float px) { return px * g_scale; }
ImU32 U32(const ImVec4& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * a)); }

void LoadLogo(HINSTANCE inst, int px) {
  HICON icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
  if (!icon) return;
  ICONINFO ii{};
  if (GetIconInfo(icon, &ii) && ii.hbmColor) {
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB};
    std::vector<uint32_t> bgra(size_t(px) * px);
    HDC dc = GetDC(nullptr);
    if (GetDIBits(dc, ii.hbmColor, 0, UINT(px), bgra.data(), &bi, DIB_RGB_COLORS)) {
      std::vector<uint32_t> rgba(bgra.size());
      for (size_t i = 0; i < bgra.size(); ++i) {
        const uint32_t c = bgra[i];
        rgba[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
      }
      D3D11_TEXTURE2D_DESC td{};
      td.Width = td.Height = UINT(px);
      td.MipLevels = td.ArraySize = 1;
      td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      D3D11_SUBRESOURCE_DATA sd{rgba.data(), UINT(px) * 4, 0};
      ID3D11Texture2D* tex = nullptr;
      if (SUCCEEDED(g_device->CreateTexture2D(&td, &sd, &tex))) {
        g_device->CreateShaderResourceView(tex, nullptr, &g_logo);
        tex->Release();
      }
    }
    ReleaseDC(nullptr, dc);
  }
  if (ii.hbmColor) DeleteObject(ii.hbmColor);
  if (ii.hbmMask) DeleteObject(ii.hbmMask);
  DestroyIcon(icon);
}

// ---- text and widgets ----
void Text(ImFont* f, float px, const ImVec4& c, const char* s) {
  ImGui::PushFont(f, S(px));
  ImGui::PushStyleColor(ImGuiCol_Text, c);
  ImGui::TextUnformatted(s);
  ImGui::PopStyleColor();
  ImGui::PopFont();
}
// Wraps at the caller's PushTextWrapPos (each screen sets its column), else at the window edge.
void Wrapped(ImFont* f, float px, const ImVec4& c, const std::string& s) {
  ImGui::PushFont(f, S(px));
  ImGui::PushStyleColor(ImGuiCol_Text, c);
  ImGui::TextWrapped("%s", s.c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
}
void Gap(float px) { ImGui::Dummy(ImVec2(0, S(px))); }

bool Button(const char* label, ImVec2 size, bool primary, bool enabled = true) {
  ImGui::BeginDisabled(!enabled);
  if (primary) {
    ImGui::PushStyleColor(ImGuiCol_Button, enabled ? kAccent : ImVec4(0.30f, 0.15f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHi);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.66f, 0.05f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, enabled ? ImVec4(1, 1, 1, 1) : ImVec4(1, 1, 1, 0.55f));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button, kPanelHi);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.19f, 0.19f, 0.22f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.23f, 0.23f, 0.27f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
  }
  ImGui::PushFont(g_font_bold, S(primary ? 18.0f : 16.5f));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
  const bool r = ImGui::Button(label, size);
  ImGui::PopStyleVar();
  ImGui::PopFont();
  ImGui::PopStyleColor(4);
  ImGui::EndDisabled();
  return r && enabled;
}
bool Link(const char* label, float px = 15.0f, const ImVec4& c = kDim) {
  ImGui::PushFont(g_font, S(px));
  ImGui::PushStyleColor(ImGuiCol_Text, c);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, U32(c));
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }
  return hovered && ImGui::IsItemClicked();
}

// Round marks: tick, cross, spinner, numbered or empty circle.
enum class Mark { kEmpty, kOk, kBad, kBusy, kNumber, kCurrent };
void DrawMark(ImDrawList* dl, ImVec2 c, float r, Mark m, int number = 0) {
  const float t = std::max(1.5f, r * 0.14f);
  switch (m) {
    case Mark::kOk:
      dl->AddCircleFilled(c, r, U32(kOk));
      dl->AddPolyline(std::array<ImVec2, 3>{ImVec2(c.x - r * 0.42f, c.y + r * 0.02f), ImVec2(c.x - r * 0.1f, c.y + r * 0.34f),
                                            ImVec2(c.x + r * 0.45f, c.y - r * 0.32f)}.data(),
                      3, U32(kBg), 0, t * 1.3f);
      break;
    case Mark::kBad:
      dl->AddCircleFilled(c, r, U32(kBad));
      dl->AddLine(ImVec2(c.x - r * 0.35f, c.y - r * 0.35f), ImVec2(c.x + r * 0.35f, c.y + r * 0.35f), U32(kBg), t * 1.3f);
      dl->AddLine(ImVec2(c.x - r * 0.35f, c.y + r * 0.35f), ImVec2(c.x + r * 0.35f, c.y - r * 0.35f), U32(kBg), t * 1.3f);
      break;
    case Mark::kBusy: {
      dl->AddCircle(c, r * 0.86f, U32(kBorder), 0, t);
      const float a0 = float(ImGui::GetTime()) * 5.5f;
      dl->PathArcTo(c, r * 0.86f, a0, a0 + 2.0f, 20);
      dl->PathStroke(U32(kAccentHi), 0, t);
      break;
    }
    case Mark::kNumber:
    case Mark::kCurrent: {
      if (m == Mark::kCurrent) dl->AddCircleFilled(c, r, U32(kAccent));
      else dl->AddCircle(c, r * 0.92f, U32(kFaint), 0, t);
      char n[4];
      std::snprintf(n, sizeof(n), "%d", number);
      ImFont* f = g_font_bold;
      const float fs = r * 1.15f;
      const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, n);
      dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.52f), U32(m == Mark::kCurrent ? kText : kDim), n);
      break;
    }
    case Mark::kEmpty:
      dl->AddCircle(c, r * 0.86f, U32(kFaint), 0, t);
      break;
  }
}

// Rounded panel drawn behind content placed with the cursor (fixed height).
void Panel(ImVec2 a, ImVec2 b, const ImVec4& fill, const ImVec4& border, float rounding = 12.0f) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b, U32(fill), S(rounding));
  dl->AddRect(a, b, U32(border), S(rounding), 0, std::max(1.0f, S(1.0f)));
}
constexpr float kPi = 3.14159265f;
// Dashed rounded outline (drop zone).
void DashedRect(ImVec2 a, ImVec2 b, const ImVec4& c) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float dash = S(9), gap = S(6), t = std::max(1.0f, S(1.5f)), r = S(14);
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float len = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / len, (q.y - p.y) / len);
    for (float s = 0; s < len; s += dash + gap) {
      const float e = std::min(len, s + dash);
      dl->AddLine(ImVec2(p.x + d.x * s, p.y + d.y * s), ImVec2(p.x + d.x * e, p.y + d.y * e), U32(c), t);
    }
  };
  line(ImVec2(a.x + r, a.y), ImVec2(b.x - r, a.y));
  line(ImVec2(a.x + r, b.y), ImVec2(b.x - r, b.y));
  line(ImVec2(a.x, a.y + r), ImVec2(a.x, b.y - r));
  line(ImVec2(b.x, a.y + r), ImVec2(b.x, b.y - r));
  dl->PathArcTo(ImVec2(a.x + r, a.y + r), r, kPi, kPi * 1.5f, 8);
  dl->PathStroke(U32(c), 0, t);
  dl->PathArcTo(ImVec2(b.x - r, a.y + r), r, kPi * 1.5f, kPi * 2.0f, 8);
  dl->PathStroke(U32(c), 0, t);
  dl->PathArcTo(ImVec2(b.x - r, b.y - r), r, 0, kPi * 0.5f, 8);
  dl->PathStroke(U32(c), 0, t);
  dl->PathArcTo(ImVec2(a.x + r, b.y - r), r, kPi * 0.5f, kPi, 8);
  dl->PathStroke(U32(c), 0, t);
}

// Compact checkbox (the global frame padding is sized for buttons).
bool Check(const char* id, bool* v) {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), S(4)));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(5));
  const bool r = ImGui::Checkbox(id, v);
  ImGui::PopStyleVar(2);
  return r;
}

// ---- app helpers ----
fs::path PayloadDir() { return SelfUpdater::LauncherDir() / "payload"; }
void RefreshInstalled() {
  g_installed = Pipeline::InstalledVersion(g_install_dir);
  g_can_rebuild = !g_installed.empty() && Pipeline::CanRebuild(g_install_dir);
}
void StartRebuild() {
  BuildRequest req;
  req.rebuild = true;
  req.install_dir = g_install_dir;
  req.payload_dir = PayloadDir();
  g_pipeline.Start(req);
}
bool ToolchainReady() {
  std::error_code ec;
  return fs::exists(Pipeline::ToolchainDir(g_install_dir) / "toolchain-v1.ok", ec);
}
bool InstalledIsOlder() { return !g_installed.empty() && openng2::IsNewer(kVersion, g_installed); }
bool RebuildPossible() { return g_can_rebuild && (g_license || ToolchainReady()); }
void PlayGame(const fs::path& exe) { ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(), SW_SHOW); }
void OpenFolder(const fs::path& dir) { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOW); }
void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOW); }

bool g_setup = false;  // the setup guide is open over an existing install
int g_step = 0;        // setup guide step 0..3

// ---- left brand panel ----
void DrawSide(ImVec2 origin, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 b(origin.x + size.x, origin.y + size.y);
  dl->AddRectFilledMultiColor(origin, b, U32(ImVec4(0.30f, 0.025f, 0.055f, 1)), U32(ImVec4(0.12f, 0.02f, 0.035f, 1)),
                              U32(ImVec4(0.045f, 0.035f, 0.045f, 1)), U32(ImVec4(0.09f, 0.02f, 0.035f, 1)));
  // A few faint diagonal slashes for texture.
  for (int i = 0; i < 5; ++i) {
    const float x = origin.x + S(60.0f + 70.0f * float(i));
    dl->AddLine(ImVec2(x, origin.y), ImVec2(x - S(220), b.y), U32(ImVec4(1, 1, 1, 0.025f)), S(18));
  }
  dl->AddLine(ImVec2(b.x - 1, origin.y), ImVec2(b.x - 1, b.y), U32(kBorder));
  const float pad = S(34);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, origin.y + S(44)));
  const float logo = S(84);
  if (g_logo) ImGui::Image(ImTextureRef(reinterpret_cast<ImTextureID>(g_logo)), ImVec2(logo, logo));
  else ImGui::Dummy(ImVec2(logo, logo));
  ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, ImGui::GetCursorScreenPos().y + S(14)));
  ImGui::BeginGroup();
  Text(g_font_bold, 38, kText, "openNG2");
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(6));
  Text(g_font, 19, ImVec4(0.92f, 0.80f, 0.82f, 1), "Ninja Gaiden II for Windows");
  Gap(18);
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x - pad * 2);
  Wrapped(g_font, 15.5f, ImVec4(0.85f, 0.78f, 0.80f, 0.85f),
          "Rebuilds the Xbox 360 game as a native PC game from your own copy. Your files stay on this PC - "
          "nothing is uploaded or shared.");
  ImGui::PopTextWrapPos();
  if (g_installed.empty() || g_setup) {
    Gap(26);
    Text(g_font_bold, 14, ImVec4(0.92f, 0.80f, 0.82f, 0.9f), "WHAT YOU'LL NEED");
    Gap(4);
    for (const char* item : {"Your copy of Ninja Gaiden II", "Title Update #2", "About 22 GB of free space",
                             "10-30 minutes for the first build"}) {
      const ImVec2 p = ImGui::GetCursorScreenPos();
      DrawMark(ImGui::GetWindowDrawList(), ImVec2(p.x + S(8), p.y + S(11)), S(8), Mark::kOk);
      ImGui::SetCursorScreenPos(ImVec2(p.x + S(26), p.y));
      Text(g_font, 15.5f, ImVec4(0.92f, 0.86f, 0.87f, 1.0f), item);
      Gap(3);
    }
  }
  ImGui::EndGroup();

  // Bottom: launcher update status, links, version.
  using St = SelfUpdater::State;
  const auto st = g_updater.state();
  float y = b.y - S(158);
  ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, y));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x - pad * 2);
  if (st == St::kAvailable || st == St::kFailed) {
    const openng2::ReleaseInfo rel = g_updater.release();
    Text(g_font_bold, 16, kText, ("openNG2 " + rel.version + " is available").c_str());
    if (st == St::kFailed) Wrapped(g_font, 14, kBad, g_updater.message());
    Gap(4);
    if (Button(st == St::kFailed ? "Try again" : "Update openNG2", ImVec2(S(170), S(36)), true, !g_pipeline.running())) {
      g_updater.Install();
    }
  } else if (st == St::kInstalling || st == St::kReady) {
    Text(g_font_bold, 16, kText, "Updating openNG2...");
    Wrapped(g_font, 14, kDim, g_updater.message());
  } else {
    const char* s = st == St::kChecking ? "Checking for updates..." : st == St::kUpToDate ? "openNG2 is up to date"
                    : st == St::kCheckFailed ? "Couldn't check for updates" : "";
    Text(g_font, 15, st == St::kCheckFailed ? kWarn : ImVec4(0.85f, 0.78f, 0.80f, 0.8f), s);
    if (st == St::kCheckFailed && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_updater.message().c_str());
    if ((st == St::kIdle || st == St::kUpToDate || st == St::kCheckFailed) && !g_pipeline.running()) {
      ImGui::SameLine(0, S(10));
      if (Link("Check now", 15)) g_updater.Check();
    }
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, b.y - S(62)));
  if (Link("Game logs", 15)) OpenFolder(g_install_dir / "logs");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("One log per play session. Attach the newest to a bug report.");
  ImGui::SameLine(0, S(16));
  if (Link("Help & project page", 15)) OpenUrl(L"https://github.com/KamiTK2/openNG2");
  ImGui::SameLine(0, S(16));
  if (Link("Report a problem", 15)) OpenUrl(L"https://github.com/KamiTK2/openNG2/issues");
  ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, b.y - S(34)));
  char ver[48];
  std::snprintf(ver, sizeof(ver), "Version %s", kVersion);
  Text(g_font, 13.5f, ImVec4(0.85f, 0.78f, 0.80f, 0.5f), ver);
}

// ---- setup guide ----
struct StepInfo {
  const char* short_name;
  const char* title;
};
const StepInfo kSteps[4] = {{"Your game", "Add your copy of Ninja Gaiden II"},
                            {"Title Update", "Add Title Update #2"},
                            {"DLC", "Add your downloadable content"},
                            {"Install", "Choose where to install"}};

bool StepDone(int i) {
  switch (i) {
    case 0: return g_game.result && g_game.result->ok;
    case 1: return g_update.result && g_update.result->ok;
    case 2: return true;  // optional
    default: return false;
  }
}

void DrawStepper(float x0, float y, float w) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float r = S(15), seg = w / 4.0f;
  for (int i = 0; i < 4; ++i) {
    const ImVec2 c(x0 + seg * (float(i) + 0.5f), y + r);
    if (i < 3) {
      const ImVec2 n(x0 + seg * (float(i) + 1.5f), y + r);
      dl->AddLine(ImVec2(c.x + r + S(6), c.y), ImVec2(n.x - r - S(6), n.y), U32(i < g_step ? kOk : kBorder),
                  std::max(1.0f, S(2)));
    }
    const Mark m = i == g_step ? Mark::kCurrent : (i < g_step || (i != 2 && StepDone(i))) ? Mark::kOk : Mark::kNumber;
    DrawMark(dl, c, r, m, i + 1);
    ImFont* f = i == g_step ? g_font_bold : g_font;
    const float fs = S(14.5f);
    const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, kSteps[i].short_name);
    dl->AddText(f, fs, ImVec2(c.x - ts.x * 0.5f, c.y + r + S(7)), U32(i == g_step ? kText : kDim), kSteps[i].short_name);
    // Steps already reached can be revisited.
    ImGui::SetCursorScreenPos(ImVec2(c.x - seg * 0.5f, y));
    ImGui::PushID(i);
    if (ImGui::InvisibleButton("step", ImVec2(seg, r * 2 + S(28))) && !g_pipeline.running()) {
      if (i <= g_step || (i == 1 && StepDone(0)) || (i >= 2 && StepDone(0) && StepDone(1))) g_step = i;
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopID();
  }
}

// Dot + text with a hanging indent.
void Bullet(const char* text) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  dl->AddCircleFilled(ImVec2(p.x + S(10), p.y + S(11)), S(2.6f), U32(kDim));
  ImGui::Indent(S(24));
  Wrapped(g_font, 16, kDim, text);
  ImGui::Unindent(S(24));
}

// "What is it / where is it" lines: bold label + text.
void InfoLine(const char* label, const char* text, float w) {
  ImGui::PushFont(g_font_bold, S(16.5f));
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::PopFont();
  Wrapped(g_font, 16.5f, kDim, text);
  Gap(6);
}

// The drop zone: invites a file, then shows what was found.
void DropZone(ImVec2 a, ImVec2 b, const Slot* slot, const DlcSet* dlc, bool folder_only, const char* noun) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const bool checking = Busy();
  bool ok = false, bad = false;
  std::string head, sub, path;
  if (slot && slot->result) {
    ok = slot->result->ok;
    bad = !ok;
    head = slot->result->message;
    path = PathText(slot->result->path);
  } else if (dlc && (!dlc->folder.empty() || dlc->count())) {
    const int n = dlc->count();
    ok = n > 0;
    bad = !ok;
    char t[96];
    std::snprintf(t, sizeof(t), n ? "%d of %zu found and verified" : "No Ninja Gaiden II DLC in this folder", n,
                  kDlc.size());
    head = t;
    if (!dlc->problems.empty()) sub = dlc->problems.front();
    path = PathText(dlc->folder);
  }
  const ImVec4 tint = ok ? kOk : bad ? kBad : kFaint;
  dl->AddRectFilled(a, b, U32(ok ? ImVec4(0.29f, 0.78f, 0.42f, 0.06f) : bad ? ImVec4(0.96f, 0.38f, 0.38f, 0.06f)
                                                                       : ImVec4(1, 1, 1, 0.02f)),
                    S(14));
  if (ok || bad) {
    dl->AddRect(a, b, U32(tint, 0.6f), S(14), 0, std::max(1.0f, S(1.5f)));
  } else {
    DashedRect(a, b, kFaint);
  }
  const float cx = (a.x + b.x) * 0.5f;
  if (ok || bad) {
    // Result: mark + headline + path + change buttons.
    const float r = S(20);
    DrawMark(dl, ImVec2(a.x + S(42), a.y + S(46)), r, ok ? Mark::kOk : Mark::kBad);
    ImGui::SetCursorScreenPos(ImVec2(a.x + S(78), a.y + S(30)));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + (b.x - a.x) - S(100));
    Wrapped(g_font_bold, 18, ok ? kText : kBad, head);
    if (!sub.empty()) Wrapped(g_font, 14.5f, kBad, sub);
    if (!path.empty()) Wrapped(g_font_mono, 13.5f, kDim, path);
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
  } else if (checking) {
    DrawMark(dl, ImVec2(cx, a.y + S(52)), S(20), Mark::kBusy);
    ImFont* f = g_font_bold;
    const ImVec2 ts = f->CalcTextSizeA(S(18), FLT_MAX, 0.0f, "Checking...");
    dl->AddText(f, S(18), ImVec2(cx - ts.x * 0.5f, a.y + S(84)), U32(kText), "Checking...");
  } else {
    // Empty: a big down-arrow and the invitation.
    const float ay = a.y + S(30);
    dl->AddLine(ImVec2(cx, ay), ImVec2(cx, ay + S(30)), U32(kDim), S(3));
    dl->AddTriangleFilled(ImVec2(cx - S(11), ay + S(22)), ImVec2(cx + S(11), ay + S(22)), ImVec2(cx, ay + S(36)), U32(kDim));
    dl->AddLine(ImVec2(cx - S(20), ay + S(44)), ImVec2(cx + S(20), ay + S(44)), U32(kDim), S(3));
    char t[96];
    std::snprintf(t, sizeof(t), "Drop %s here", noun);
    const ImVec2 ts = g_font_bold->CalcTextSizeA(S(19), FLT_MAX, 0.0f, t);
    dl->AddText(g_font_bold, S(19), ImVec2(cx - ts.x * 0.5f, ay + S(56)), U32(kText), t);
  }
  // Buttons along the bottom of the zone.
  const float bh = S(38), by = b.y - bh - S(18);
  const char* l1 = folder_only ? "Browse for folder..." : "Browse for file...";
  const float w1 = S(folder_only ? 190 : 170), w2 = S(190);
  const bool two = slot == &g_game;
  float total = w1 + (two ? w2 + S(10) : 0);
  float x = (ok || bad) ? a.x + S(78) : cx - total * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(x, by));
  if (!(ok || bad) || true) {
    if (Button((ok || bad) ? (folder_only ? "Choose another folder..." : "Choose another...") : l1,
               ImVec2((ok || bad) ? S(200) : w1, bh), false, !g_pipeline.running())) {
      for (const auto& p : PickPaths(folder_only, false)) Add(p, dlc != nullptr);
    }
    if (two && !(ok || bad)) {
      ImGui::SameLine(0, S(10));
      if (Button("Browse for folder...", ImVec2(w2, bh), false, !g_pipeline.running())) {
        for (const auto& p : PickPaths(true, false)) Add(p);
      }
    }
  }
}

void DrawSetup(ImVec2 o, ImVec2 size) {
  const float pad = S(48);
  const float x0 = o.x + pad, w = size.x - pad * 2;
  DrawStepper(x0, o.y + S(30), w);
  ImGui::SetCursorScreenPos(ImVec2(x0, o.y + S(108)));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(x0 + w - ImGui::GetWindowPos().x);
  Text(g_font_bold, 29, kText, kSteps[g_step].title);
  Gap(8);
  const float body_w = w;
  const float zone_top = o.y + S(408), zone_bottom = o.y + S(576);
  switch (g_step) {
    case 0:
      InfoLine("Which version?",
               "Ninja Gaiden II for Xbox 360 (2008), the World release - the one sold everywhere except Japan "
               "(media ID 69F555CB). Not the Japanese release, Ninja Gaiden Sigma 2 or the Master Collection.",
               body_w);
      InfoLine("In which form?", "Any one of these:", body_w);
      Bullet("Games on Demand or a disc installed to the console: the file C1200E9B558D04454FA6 plus the "
             "folder C1200E9B558D04454FA6.data next to it, from Content\\0000000000000000\\544307D5\\00007000\\.");
      Bullet("A disc image (.iso) made from your disc, or a folder of the disc's files (with default.xex).");
      break;
    case 1:
      InfoLine("Which file?",
               "Title Update #2 for the World release, exactly: TU_1A461UL_000000G000000.00000000000G4 "
               "(2.3 MB). On the console it's called \"Ninja Gaiden 2 Title Update #2\".",
               body_w);
      InfoLine("Not these",
               "Title Update #3 (...00000000000O4) or the Japanese \"Re-sign\" updates (...C000000...) - they "
               "contain different code and won't build.",
               body_w);
      InfoLine("Where is it?",
               "Wherever you keep your title updates: a console drive or USB stick (Content\\0000000000000000\\"
               "544307D5\\000B0000\\), or the console's update cache copied out with a tool like Horizon.",
               body_w);
      break;
    case 2:
      InfoLine("What is it?",
               "Mission Mode and three costumes for Ryu (Fiend, Shadow Walker, Biometal). You don't need them "
               "to play - skip this step if you don't have any.",
               body_w);
      InfoLine("Where is it?",
               "On your Xbox 360 drive or USB stick in Content\\<your profile>\\544307D5\\00000002\\. Choose "
               "that folder; every pack in it is added.",
               body_w);
      break;
    default: break;
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();

  const ImVec2 za(x0, zone_top), zb(x0 + w, zone_bottom);
  if (g_step == 0) DropZone(za, zb, &g_game, nullptr, false, "your game");
  if (g_step == 1) DropZone(za, zb, &g_update, nullptr, false, "the title update");
  if (g_step == 2) DropZone(za, zb, nullptr, &g_dlc, true, "your DLC folder");
  if (g_step == 3) {
    // Install: folder, space, licence, build.
    ImGui::SetCursorScreenPos(ImVec2(x0, o.y + S(160)));
    ImGui::BeginGroup();
    ImGui::PushTextWrapPos(x0 + w - ImGui::GetWindowPos().x);
    Text(g_font_bold, 16.5f, kText, "Install folder");
    Gap(2);
    const ImVec2 pa = ImGui::GetCursorScreenPos();
    Panel(pa, ImVec2(pa.x + w, pa.y + S(52)), kPanel, kBorder, 10);
    ImGui::SetCursorScreenPos(ImVec2(pa.x + S(16), pa.y + S(15)));
    ImGui::PushFont(g_font_mono, S(15));
    ImGui::PushStyleColor(ImGuiCol_Text, kText);
    ImGui::TextUnformatted(PathText(g_install_dir).c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(pa.x + w - S(118), pa.y + S(8)));
    if (Button("Change...", ImVec2(S(110), S(36)), false, !g_pipeline.running())) {
      auto p = PickPaths(true, false);
      if (!p.empty()) {
        g_install_dir = p[0];
        SaveSettings();
        RefreshInstalled();
      }
    }
    ImGui::SetCursorScreenPos(ImVec2(pa.x, pa.y + S(62)));
    const double free_gb = double(FreeDiskBytes(g_install_dir)) / 1e9;
    char space[160];
    std::snprintf(space, sizeof(space), "Needs about 22 GB of free space - this drive has %.0f GB free.", free_gb);
    Wrapped(g_font, 15.5f, free_gb < 22.0 ? kWarn : kDim, space);
    Gap(22);
    Text(g_font_bold, 16.5f, kText, "Microsoft build tools");
    Gap(2);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w);
    Wrapped(g_font, 16, kDim,
            "To build the game, openNG2 downloads Microsoft's free C++ build tools (about 1 GB, straight from "
            "Microsoft - this is not the Visual C++ Redistributable). Microsoft asks you to accept its licence.");
    ImGui::PopTextWrapPos();
    Gap(8);
    if (Check("##license", &g_license)) SaveSettings();
    ImGui::SameLine(0, S(10));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
    Text(g_font, 16.5f, kText, "I accept the Microsoft Visual Studio Build Tools licence");
    ImGui::SameLine(0, S(10));
    if (Link("Read it", 16.5f, kAccentHi)) OpenUrl(L"https://go.microsoft.com/fwlink/?LinkId=2086102");
    Gap(22);
    Wrapped(g_font, 15.5f, kDim,
            "The first build takes about 10-30 minutes depending on your PC (it downloads the tools, unpacks the "
            "game and compiles it). Later updates are much quicker.");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
  }

  // Navigation row.
  const float bh = S(46), by = o.y + size.y - bh - S(36);
  if (g_step > 0 || g_setup) {
    ImGui::SetCursorScreenPos(ImVec2(x0, by));
    if (Button(g_step > 0 ? "Back" : "Cancel", ImVec2(S(120), bh), false, !g_pipeline.running())) {
      if (g_step > 0) --g_step;
      else g_setup = false;
    }
  }
  const float nw = S(g_step == 3 ? 260 : 170);
  ImGui::SetCursorScreenPos(ImVec2(x0 + w - nw, by));
  if (g_step < 3) {
    const bool can = StepDone(g_step);
    const char* label = g_step == 2 ? (g_dlc.count() ? "Next" : "Skip") : "Next";
    if (Button(label, ImVec2(nw, bh), true, can)) ++g_step;
    if (!can) {
      const char* why = Busy() ? "Checking your file..." : g_step == 0 ? "Add your game to continue" : "Add the title update to continue";
      const ImVec2 ts = g_font->CalcTextSizeA(S(15), FLT_MAX, 0.0f, why);
      ImGui::GetWindowDrawList()->AddText(g_font, S(15), ImVec2(x0 + w - nw - ts.x - S(16), by + (bh - ts.y) * 0.5f),
                                          U32(kDim), why);
    }
  } else {
    const bool ready = StepDone(0) && StepDone(1) && g_license && !Busy();
    if (Button("Build Ninja Gaiden II", ImVec2(nw, bh), true, ready)) {
      BuildRequest req;
      req.game = *g_game.result;
      req.update = *g_update.result;
      for (const auto& d : g_dlc.found) {
        if (d.ok) req.dlc.push_back(d);
      }
      req.install_dir = g_install_dir;
      req.payload_dir = PayloadDir();
      SaveSettings();
      g_shortcut_done = false;
      g_pipeline.Start(req);
    }
    if (!ready && !g_license) {
      const char* why = "Accept the licence to build";
      const ImVec2 ts = g_font->CalcTextSizeA(S(15), FLT_MAX, 0.0f, why);
      ImGui::GetWindowDrawList()->AddText(g_font, S(15), ImVec2(x0 + w - nw - ts.x - S(16), by + (bh - ts.y) * 0.5f),
                                          U32(kDim), why);
    }
  }
}

// ---- installed game ----
void DrawHome(ImVec2 o, ImVec2 size) {
  const float pad = S(56);
  const float x0 = o.x + pad, w = size.x - pad * 2;
  const bool older = InstalledIsOlder();
  ImGui::SetCursorScreenPos(ImVec2(x0, o.y + S(70)));
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(x0 + w - ImGui::GetWindowPos().x);
  Text(g_font, 17, kDim, older ? "Update ready" : "Ready to play");
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(4));
  Text(g_font_bold, 44, kText, "Ninja Gaiden II");
  Gap(4);
  char line[160];
  if (older) {
    std::snprintf(line, sizeof(line), "Installed: openNG2 %s.  New: openNG2 %s - update to get the latest fixes.",
                  g_installed.c_str(), kVersion);
  } else {
    std::snprintf(line, sizeof(line), "openNG2 %s is installed and up to date.", g_installed.c_str());
  }
  Wrapped(g_font, 17, older ? kWarn : kDim, line);
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();

  // Big action.
  const float bh = S(64), by = o.y + S(236);
  ImGui::SetCursorScreenPos(ImVec2(x0, by));
  if (older) {
    if (Button("Update the game", ImVec2(w * 0.62f, bh), true, RebuildPossible() && !g_updater.busy())) {
      SaveSettings();
      StartRebuild();
    }
    ImGui::SameLine(0, S(12));
    if (Button("Play anyway", ImVec2(w * 0.38f - S(12), bh), false)) PlayGame(g_install_dir / "ng2.exe");
    ImGui::SetCursorScreenPos(ImVec2(x0, by + bh + S(14)));
    ImGui::PushTextWrapPos(x0 + w - ImGui::GetWindowPos().x);
    Wrapped(g_font, 15.5f, kDim,
            "Updating rebuilds the game from the files already in its folder - nothing to add again. Your saves "
            "and settings are kept.");
    ImGui::PopTextWrapPos();
  } else {
    if (Button("Play", ImVec2(w, bh), true)) PlayGame(g_install_dir / "ng2.exe");
  }
  if (!g_can_rebuild) {
    ImGui::SetCursorScreenPos(ImVec2(x0, by + bh + S(70)));
    Wrapped(g_font, 15, kWarn, "The game files in the install folder are incomplete - use \"Set up again\" to add them.");
  }

  // Install details + secondary actions.
  const float cy = o.y + size.y - S(170);
  const ImVec2 ca(x0, cy), cb(x0 + w, cy + S(96));
  Panel(ca, cb, kPanel, kBorder, 12);
  ImGui::SetCursorScreenPos(ImVec2(ca.x + S(20), ca.y + S(16)));
  ImGui::BeginGroup();
  Text(g_font_bold, 15, kDim, "INSTALLED IN");
  ImGui::PushFont(g_font_mono, S(15));
  ImGui::PushStyleColor(ImGuiCol_Text, kText);
  ImGui::TextUnformatted(PathText(g_install_dir).c_str());
  ImGui::PopStyleColor();
  ImGui::PopFont();
  Gap(6);
  if (Link("Open folder", 15.5f, kText)) OpenFolder(g_install_dir);
  ImGui::SameLine(0, S(22));
  ImGui::BeginDisabled(!RebuildPossible() || g_updater.busy());
  if (Link("Rebuild", 15.5f, RebuildPossible() ? kText : kFaint)) {
    SaveSettings();
    StartRebuild();
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build the installed version again (e.g. after changing PCs)");
  ImGui::SameLine(0, S(22));
  if (Link("Set up again with different files", 15.5f, kText)) {
    g_setup = true;
    g_step = 0;
  }
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(ImVec2(x0, cb.y + S(14)));
  if (Check("##upd", &g_check_updates)) SaveSettings();
  ImGui::SameLine(0, S(8));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
  Text(g_font, 15, kDim, "Check for openNG2 updates when the launcher starts");
}

// ---- build ----
struct BuildView {
  std::vector<Step> steps;
  bool running = false, finished = false, succeeded = false;
  std::string error;
};
float Overall(const std::vector<Step>& steps) {
  float o = 0.0f;
  for (const auto& s : steps) {
    o += (s.state == StepState::kDone || s.state == StepState::kSkipped) ? 1.0f
         : (s.state == StepState::kRunning && s.progress > 0) ? s.progress : 0.0f;
  }
  return steps.empty() ? 0.0f : o / float(steps.size());
}
bool g_show_log = false;

void DrawBuild(ImVec2 o, ImVec2 size, const BuildView& v) {
  const float pad = S(56);
  const float x0 = o.x + pad, w = size.x - pad * 2;
  const float overall = Overall(v.steps);
  const Step* cur = nullptr;
  for (const auto& s : v.steps)
    if (s.state == StepState::kRunning || s.state == StepState::kFailed) cur = &s;
  ImGui::SetCursorScreenPos(ImVec2(x0, o.y + S(56)));
  ImGui::BeginGroup();
  Text(g_font, 17, kDim, v.finished ? (v.succeeded ? "All done" : "Something went wrong") : "Please wait");
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(4));
  Text(g_font_bold, 38, kText, v.finished ? (v.succeeded ? "Ninja Gaiden II is ready" : "The build stopped")
                                        : "Building Ninja Gaiden II");
  ImGui::EndGroup();

  // Progress bar + percent + current step.
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float py = o.y + S(170), ph = S(12);
  dl->AddRectFilled(ImVec2(x0, py), ImVec2(x0 + w, py + ph), U32(kPanelHi), ph * 0.5f);
  const ImVec4 fill = v.finished ? (v.succeeded ? kOk : kBad) : kAccent;
  const float f = v.finished && v.succeeded ? 1.0f : overall;
  if (f > 0) dl->AddRectFilled(ImVec2(x0, py), ImVec2(x0 + w * f, py + ph), U32(fill), ph * 0.5f);
  ImGui::SetCursorScreenPos(ImVec2(x0, py + ph + S(14)));
  // One group at the column: new lines inside it start at x0, not at the window edge (the
  // second failure line was drawn over the side panel).
  ImGui::BeginGroup();
  ImGui::PushTextWrapPos(x0 + w - ImGui::GetWindowPos().x);
  if (!v.finished) {
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%.0f%%", overall * 100.0f);
    Text(g_font_bold, 22, kText, pct);
    if (cur) {
      ImGui::SameLine(0, S(14));
      ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(4));
      std::string t = cur->title;
      if (!cur->detail.empty()) t += " - " + cur->detail;
      Wrapped(g_font, 16, kDim, t);
    }
  } else if (v.succeeded) {
    Wrapped(g_font, 17, kDim, "Rebuilt from your files and installed. Have fun!");
  } else {
    Wrapped(g_font, 16.5f, kBad, v.error);
    Gap(2);
    Wrapped(g_font, 15, kDim, "Nothing is lost - finished steps are kept. Fix the problem and build again.");
  }
  ImGui::PopTextWrapPos();
  ImGui::EndGroup();

  // Steps (or the log).
  const float ly = o.y + S(286), lh = size.y - S(286) - S(118);
  const ImVec2 la(x0, ly), lb(x0 + w, ly + lh);
  Panel(la, lb, kPanel, kBorder, 12);
  if (g_show_log) {
    ImGui::SetCursorScreenPos(ImVec2(la.x + S(1), la.y + S(1)));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(12)));
    ImGui::BeginChild("log", ImVec2(w - S(2), lh - S(2)), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(g_font_mono, S(13));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.74f, 0.74f, 0.78f, 1.0f));
    for (const auto& l : g_pipeline.log_tail(400)) ImGui::TextUnformatted(l.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
  } else {
    const float row = lh / float(std::max<size_t>(1, v.steps.size()));
    for (size_t i = 0; i < v.steps.size(); ++i) {
      const Step& s = v.steps[i];
      const float yc = ly + row * (float(i) + 0.5f);
      const Mark m = s.state == StepState::kDone || s.state == StepState::kSkipped ? Mark::kOk
                     : s.state == StepState::kFailed ? Mark::kBad
                     : s.state == StepState::kRunning ? Mark::kBusy : Mark::kEmpty;
      DrawMark(dl, ImVec2(la.x + S(30), yc), S(11), m);
      ImFont* fnt = s.state == StepState::kRunning ? g_font_bold : g_font;
      const ImVec4 col = s.state == StepState::kPending ? kFaint : kText;
      const ImVec2 ts = fnt->CalcTextSizeA(S(16.5f), FLT_MAX, 0.0f, s.title.c_str());
      dl->AddText(fnt, S(16.5f), ImVec2(la.x + S(54), yc - ts.y * 0.5f), U32(col), s.title.c_str());
      const char* right = s.state == StepState::kSkipped ? "Already done" : s.state == StepState::kDone ? "Done" : "";
      if (*right) {
        const ImVec2 rs = g_font->CalcTextSizeA(S(14.5f), FLT_MAX, 0.0f, right);
        dl->AddText(g_font, S(14.5f), ImVec2(lb.x - rs.x - S(20), yc - rs.y * 0.5f), U32(kFaint), right);
      }
    }
  }

  // Buttons.
  const float bh = S(48), by = o.y + size.y - bh - S(40);
  ImGui::SetCursorScreenPos(ImVec2(x0, by));
  if (v.running) {
    // Cancel stops every build process (they all run in one job) before the screen changes.
    const bool stopping = g_pipeline.cancelling();
    if (Button(stopping ? "Cancelling..." : "Cancel", ImVec2(S(150), bh), false, !stopping)) g_pipeline.Cancel();
  } else if (v.succeeded) {
    if (Button("Play", ImVec2(S(180), bh), true)) PlayGame(g_pipeline.exe_path());
    ImGui::SameLine(0, S(10));
    if (Button(g_shortcut_done ? "Shortcut added" : "Add desktop shortcut", ImVec2(S(200), bh), false)) {
      CreateShortcut(g_pipeline.exe_path());
      g_shortcut_done = true;
    }
    ImGui::SameLine(0, S(10));
    if (Button("Done", ImVec2(S(100), bh), false)) {
      g_pipeline.Reset();
      RefreshInstalled();
      g_setup = false;
    }
  } else {
    if (Button("Back", ImVec2(S(130), bh), false)) {
      g_pipeline.Reset();
      RefreshInstalled();
    }
    ImGui::SameLine(0, S(10));
    if (Button("Open the build log", ImVec2(S(210), bh), false)) {
      ShellExecuteW(nullptr, L"open", g_pipeline.log_path().c_str(), nullptr, nullptr, SW_SHOW);
    }
  }
  const char* tl = g_show_log ? "Show steps" : "Show details";
  const ImVec2 ts = g_font->CalcTextSizeA(S(15.5f), FLT_MAX, 0.0f, tl);
  ImGui::SetCursorScreenPos(ImVec2(x0 + w - ts.x, by + (bh - ts.y) * 0.5f));
  if (Link(tl, 15.5f)) g_show_log = !g_show_log;
}

// Mock data for --ui-preview (screenshots of every screen without a real build).
BuildView PreviewBuild(int mode) {
  BuildView v;
  v.steps = g_pipeline.steps();
  for (size_t i = 0; i < v.steps.size(); ++i) {
    Step& s = v.steps[i];
    s.state = i < 4 ? (i == 1 ? StepState::kSkipped : StepState::kDone) : StepState::kPending;
    if (mode == 5 && i == 4) {
      s.state = StepState::kRunning;
      s.progress = 0.42f;
      s.detail = "Compiling: step 241 of 563";
    }
    if (mode == 6) s.state = StepState::kDone;
    if (mode == 7 && i == 4) {
      s.state = StepState::kFailed;
      s.detail = "Building openNG2 failed (see the log).";
    }
  }
  v.running = mode == 5;
  v.finished = mode != 5;
  v.succeeded = mode == 6;
  v.error = "Building openNG2 failed (see the log).";
  return v;
}

void DrawUi() {
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("openNG2", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                   ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  const ImVec2 o = vp->WorkPos, sz = vp->WorkSize;
  const float side = S(kSideW);
  DrawSide(o, ImVec2(side, sz.y));
  const ImVec2 ro(o.x + side, o.y), rs(sz.x - side, sz.y);
  if (g_ui_preview >= 5) {
    DrawBuild(ro, rs, PreviewBuild(g_ui_preview));
  } else if (g_ui_preview >= 0) {
    if (g_ui_preview == 4) DrawHome(ro, rs);
    else {
      g_step = g_ui_preview;
      g_setup = true;
      DrawSetup(ro, rs);
    }
  } else if (g_pipeline.running() || g_pipeline.finished()) {
    BuildView v;
    v.steps = g_pipeline.steps();
    v.running = g_pipeline.running();
    v.finished = g_pipeline.finished();
    v.succeeded = g_pipeline.succeeded();
    v.error = g_pipeline.error();
    DrawBuild(ro, rs, v);
  } else if (!g_installed.empty() && !g_setup) {
    DrawHome(ro, rs);
  } else {
    DrawSetup(ro, rs);
  }
  ImGui::End();
}

void ApplyStyle(float scale) {
  ImGuiStyle& s = ImGui::GetStyle();
  s = ImGuiStyle();
  ImGui::StyleColorsDark(&s);
  s.WindowPadding = ImVec2(0, 0);
  s.FramePadding = ImVec2(14, 8);
  s.ItemSpacing = ImVec2(10, 6);
  s.ItemInnerSpacing = ImVec2(8, 6);
  s.FrameRounding = 8;
  s.GrabRounding = 6;
  s.ChildRounding = 10;
  s.PopupRounding = 8;
  s.ScrollbarSize = 12;
  s.ScrollbarRounding = 6;
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  s.FrameBorderSize = 0;
  ImVec4* c = s.Colors;
  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kFaint;
  c[ImGuiCol_WindowBg] = kBg;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = kPanelHi;
  c[ImGuiCol_Border] = kBorder;
  c[ImGuiCol_FrameBg] = kPanelHi;
  c[ImGuiCol_FrameBgHovered] = ImVec4(0.19f, 0.19f, 0.22f, 1.0f);
  c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.23f, 0.27f, 1.0f);
  c[ImGuiCol_CheckMark] = kAccentHi;
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.24f, 0.28f, 1.0f);
  s.ScaleAllSizes(scale);
  s.FontScaleDpi = 1.0f;
}

void ApplyScale(float scale) {
  g_scale = scale;
  ApplyStyle(scale);
  ImGui::GetStyle().FontSizeBase = 16.0f * scale;
}

// Window size for a DPI scale: the fixed client area plus the frame.
RECT WindowRectFor(float scale, int x, int y) {
  RECT r{0, 0, LONG(kClientW * scale), LONG(kClientH * scale)};
  AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_ACCEPTFILES);
  return RECT{x, y, x + (r.right - r.left), y + (r.bottom - r.top)};
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
      mm->ptMinTrackSize = POINT{LONG(720 * g_scale), LONG(600 * g_scale)};
      return 0;
    }
    case WM_DPICHANGED: {
      // Moved to a monitor with another scale: resize as Windows suggests, rescale the UI.
      g_pending_dpi = float(HIWORD(wparam)) / 96.0f;
      const RECT* r = reinterpret_cast<const RECT*>(lparam);
      const RECT w = WindowRectFor(g_pending_dpi, r->left, r->top);  // fixed size at the new scale
      SetWindowPos(hwnd, nullptr, w.left, w.top, w.right - w.left, w.bottom - w.top, SWP_NOZORDER | SWP_NOACTIVATE);
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
  {
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
      for (int i = 1; i < argc; ++i) {
        if (std::wstring(argv[i]) == L"--update") g_arg_update = true;
        if (std::wstring(argv[i]) == L"--updated") g_arg_updated = true;
        if (std::wstring(argv[i]).rfind(L"--ui-preview=", 0) == 0) g_ui_preview = _wtoi(argv[i] + 13);
      }
      LocalFree(argv);
    }
  }
  SelfUpdater::CleanupOldFiles();
  RefreshInstalled();
  if (g_check_updates || g_arg_update) g_updater.Check();

  WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(1)),
                 LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"openNG2Launcher", nullptr};
  RegisterClassExW(&wc);
  const float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
  const std::wstring title = L"openNG2 " + Widen(kVersion) + L" - Ninja Gaiden II";
  // Fixed size: everything fits without scrolling. Centred on the primary monitor's work area.
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  RECT wr = WindowRectFor(dpi, 0, 0);
  const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
  g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, title.c_str(),
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           work.left + std::max(0, int(work.right - work.left - ww) / 2),
                           work.top + std::max(0, int(work.bottom - work.top - wh) / 2), ww, wh, nullptr, nullptr, inst,
                           nullptr);
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
  ApplyScale(scale);
  // Fonts are sized per use (ImGui 1.92 dynamic fonts); the load size is only the default.
  ImFontConfig cfg;
  cfg.RasterizerMultiply = 1.08f;  // slightly heavier strokes on the dark background
  auto load = [&](std::initializer_list<const char*> files) -> ImFont* {
    for (const char* file : files) {
      std::error_code ec;
      if (fs::exists(file, ec)) {
        if (ImFont* f = io.Fonts->AddFontFromFileTTF(file, 16.0f * scale, &cfg)) return f;
      }
    }
    return nullptr;
  };
  g_font = load({"C:\\Windows\\Fonts\\SegUIVar.ttf", "C:\\Windows\\Fonts\\segoeui.ttf"});
  g_font_bold = load({"C:\\Windows\\Fonts\\seguisb.ttf", "C:\\Windows\\Fonts\\segoeuib.ttf"});
  g_font_mono = load({"C:\\Windows\\Fonts\\CascadiaMono.ttf", "C:\\Windows\\Fonts\\consola.ttf"});
  if (!g_font) g_font = io.Fonts->AddFontDefault();
  if (!g_font_bold) g_font_bold = g_font;
  if (!g_font_mono) g_font_mono = g_font;
  io.FontDefault = g_font;
  LoadLogo(inst, int(64 * scale));
  ImGui_ImplWin32_Init(g_hwnd);
  ImGui_ImplDX11_Init(g_device, g_context);

  bool done = false;
  while (!done) {
    MSG msg;
    // Sleep while idle; wake for input, or every 100 ms while work is in progress.
    const bool active = g_pipeline.running() || Busy() || g_updater.busy();
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
    {
      // A finished self-update restarts into the new launcher, which then rebuilds the game.
      using S = SelfUpdater::State;
      const auto st = g_updater.state();
      if (st == S::kReady && !done) {
        if (!SelfUpdater::Restart(L"--updated")) {
          MessageBoxW(g_hwnd, L"The update is installed. Start openNG2.exe again to continue.", L"openNG2",
                      MB_ICONINFORMATION);
        }
        done = true;
        break;
      }
      // --update: install a newer launcher if there is one, else rebuild an older game.
      // --updated: rebuild the installed game with this (new) launcher.
      if (!g_auto_done && (g_arg_update || g_arg_updated) && !g_pipeline.running()) {
        if (g_arg_update && st == S::kAvailable) {
          g_auto_done = true;
          g_updater.Install();
        } else if (g_arg_updated || st == S::kUpToDate || st == S::kCheckFailed) {
          g_auto_done = true;
          if (InstalledIsOlder() && RebuildPossible()) StartRebuild();
        }
      }
    }
    if (g_pending_dpi > 0.0f) {
      ApplyScale(g_pending_dpi);
      g_pending_dpi = 0.0f;
    }
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
  if (g_logo) g_logo->Release();
  if (g_rtv) g_rtv->Release();
  g_swap->Release();
  g_context->Release();
  g_device->Release();
  DestroyWindow(g_hwnd);
  CoUninitialize();
  return 0;
}
