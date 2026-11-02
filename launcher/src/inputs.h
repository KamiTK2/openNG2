// openNG2 launcher: what the player has to provide, and identifying what they gave us.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {

constexpr uint32_t kTitleId = 0x544307D5;      // Ninja Gaiden II
constexpr uint32_t kDiscMediaId = 0x69F555CB;  // the disc version the port is built for
constexpr uint32_t kTu3MediaId = 0x0E6FB1CE;   // "Re-sign" release (Title Update #3)
constexpr char kGodHeaderName[] = "C1200E9B558D04454FA6";
constexpr char kTu2FileName[] = "TU_1A461UL_000000G000000.00000000000G4";
constexpr char kXexSha256[] = "4E94213A502250F2B9186AB53867FE41AA5F5BA51BD30FC81EC35A77702FAE30";

struct DlcInfo {
  const char* file_name;
  const char* title;
};
constexpr std::array<DlcInfo, 4> kDlc = {{
    {"F591AD78AA8F87452542887FF97DCF985459A18054", "Mission Mode"},
    {"863C5F74B44C969C92A38F45830D9CBDF76DBD8154", "Fiend Hayabusa costume"},
    {"EC14BBD269A74DA378F953B579B25795D29CBD4E54", "Shadow Walker Hayabusa costume"},
    {"F80413BA6E9D7DCCDF54D5C901DD915ABA73BCE554", "Biometal Hayabusa costume"},
}};

enum class GameSourceKind { kNone, kGod, kIso, kFolder };

// Result of looking at one file or folder the player added.
struct Identified {
  enum class Kind { kUnknown, kGame, kUpdate, kDlc } kind = Kind::kUnknown;
  bool ok = false;
  std::string message;      // shown to the player (what it is, or what is wrong and what to use)
  std::filesystem::path path;  // normalised: GOD header file, .iso, game folder, package file
  GameSourceKind game_kind = GameSourceKind::kNone;
  int dlc_index = -1;
};

// DLC: the player points at the folder holding their DLC packages; each of the four known
// packages found there (or added as a single file) is checked.
struct DlcSet {
  std::filesystem::path folder;
  std::array<Identified, kDlc.size()> found;  // .ok when present and verified
  std::vector<std::string> problems;          // packages that are not usable, and why
  int count() const {
    int n = 0;
    for (const auto& f : found) n += f.ok ? 1 : 0;
    return n;
  }
};
// Scans a folder (and its subfolders, two levels) for Ninja Gaiden II DLC packages.
DlcSet ScanDlcFolder(const std::filesystem::path& folder);

// Identifies a dropped / chosen path. Game sources are fully checked (default.xex SHA-256), which
// reads ~30 MB: call it off the UI thread.
Identified Identify(const std::filesystem::path& path);

}  // namespace launcher
