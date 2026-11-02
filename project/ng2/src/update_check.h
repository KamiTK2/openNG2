#pragma once
// openNG2 update check, shared by the game (ng2.exe) and the launcher (openNG2.exe).
//
// Asks GitHub for the latest published release of KamiTK2/openNG2 (drafts and pre-releases
// are skipped by the API). Release tags are "v<version>" and carry the release zip
// "openNG2-<version>.zip" (tools/make_release.py). Nothing about the player is sent: one
// anonymous HTTPS GET to api.github.com.

#include <cstdint>
#include <string>
#include <string_view>

namespace openng2 {

inline constexpr char kRepo[] = "KamiTK2/openNG2";
inline constexpr char kReleasesPage[] = "https://github.com/KamiTK2/openNG2/releases/latest";

// The build's own version (root VERSION file; "dev" when unknown).
#ifdef OPENNG2_VERSION
inline constexpr char kVersion[] = OPENNG2_VERSION;
#else
inline constexpr char kVersion[] = "dev";
#endif

// Install marker written by the launcher next to ng2.exe: which version is installed and
// which launcher built it (the game opens that launcher to update).
inline constexpr char kInstallInfoFile[] = "openNG2-install.ini";

// Update hand-over between the launcher and a running game (both next to ng2.exe):
// - kUpdateReadyFile: written by the launcher while a rebuilt update waits for the game to
//   close ("version=<v>" and "pid=<launcher process id>" lines); removed when it stops waiting.
//   The game shows "update ready" with Restart now / Restart after next save.
// - kUpdateRestartFile: written by the game just before it quits for the update; the launcher
//   starts the new ng2.exe once it is installed, then removes the file.
inline constexpr char kUpdateReadyFile[] = "openNG2-update-ready";
inline constexpr char kUpdateRestartFile[] = "openNG2-update-restart";

struct ReleaseInfo {
  bool ok = false;
  std::string error;      // when !ok
  std::string version;    // "1.1" (tag without the leading v)
  std::string page_url;   // release page
  std::string notes;      // release body (Markdown), may be empty
  std::string zip_url;    // openNG2-<version>.zip, empty if the release has none
  std::string zip_sha256; // upper-case hex from GitHub's asset digest, empty if not given
  uint64_t zip_size = 0;
};

// Blocking (a few hundred ms); call it off the UI / game thread.
ReleaseInfo FetchLatestRelease(const char* user_agent);

// <0, 0, >0 like strcmp, comparing dotted numbers ("1.10" > "1.9", "v1.0" == "1.0", "1.0" == "1").
// A version without digits ("dev") is older than everything.
int CompareVersions(std::string_view a, std::string_view b);
inline bool IsNewer(std::string_view candidate, std::string_view current) {
  return CompareVersions(candidate, current) > 0;
}

}  // namespace openng2
