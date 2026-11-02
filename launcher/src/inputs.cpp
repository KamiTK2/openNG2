// openNG2 launcher: identifying the files the player gives us.
#include "inputs.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "util.h"
#include "xbox.h"

namespace launcher {
namespace fs = std::filesystem;

namespace {

std::string Hex8(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}

const char* kWrongXex =
    "This is not the supported game version. openNG2 needs the original Ninja Gaiden II disc "
    "release (media ID 69F555CB), as a Games on Demand package, disc image or extracted folder.";

Identified CheckXex(Identified r, const std::vector<uint8_t>& xex) {
  if (xex.size() < 4 || std::memcmp(xex.data(), "XEX2", 4) != 0) {
    r.message = "default.xex is damaged (not an Xbox 360 executable).";
    return r;
  }
  if (Sha256(xex.data(), xex.size()) != kXexSha256) {
    r.message = kWrongXex;
    return r;
  }
  r.ok = true;
  return r;
}

Identified CheckImage(Identified r, ImageSource& src, const char* what) {
  std::vector<XdvdfsEntry> entries;
  std::string err;
  if (!ListXdvdfs(src, &entries, &err)) {
    r.message = err;
    return r;
  }
  for (const auto& e : entries) {
    if (!e.directory && e.path == "default.xex") {
      std::vector<uint8_t> xex;
      if (!ReadXdvdfsFile(src, e, &xex)) {
        r.message = "Reading default.xex failed: the game data is damaged or incomplete.";
        return r;
      }
      r = CheckXex(r, xex);
      if (r.ok) r.message = std::string("Ninja Gaiden II (") + what + ") - verified.";
      return r;
    }
  }
  r.message = "This image contains no default.xex - it is not an Xbox 360 game.";
  return r;
}

Identified FromPackage(const fs::path& path, const PackageInfo& info) {
  Identified r;
  r.path = path;
  if (info.content_type == 0x7000) {  // Games on Demand header
    r.kind = Identified::Kind::kGame;
    r.game_kind = GameSourceKind::kGod;
    if (info.title_id != kTitleId) {
      r.message = "This Games on Demand package is a different game (title ID " + Hex8(info.title_id) +
                  "). Use Ninja Gaiden II's package " + kGodHeaderName + ".";
      return r;
    }
    std::string err;
    auto src = OpenGod(path, &err);
    if (!src) {
      r.message = err;
      return r;
    }
    return CheckImage(r, *src, "Games on Demand");
  }
  if (info.content_type == 0xB0000) {  // title update
    r.kind = Identified::Kind::kUpdate;
    if (info.title_id != kTitleId) {
      r.message = "This title update is for a different game (title ID " + Hex8(info.title_id) +
                  "). You need " + kTu2FileName + ".";
      return r;
    }
    if (info.media_id == kTu3MediaId) {
      r.message = std::string("This is Title Update #3, which is for a different release of the game. "
                              "openNG2 needs Title Update #2: ") + kTu2FileName + ".";
      return r;
    }
    if (info.media_id != kDiscMediaId) {
      r.message = "This title update is for another version of the game (media ID " +
                  Hex8(info.media_id) + "). You need Title Update #2: " + kTu2FileName + ".";
      return r;
    }
    r.ok = true;
    r.message = info.display_name.empty() ? "Title Update #2 - verified." : info.display_name + " - verified.";
    return r;
  }
  if (info.content_type == 0x2) {  // marketplace content (DLC)
    r.kind = Identified::Kind::kDlc;
    if (info.title_id != kTitleId) {
      r.message = "This downloadable content is for a different game (title ID " + Hex8(info.title_id) + ").";
      return r;
    }
    const std::string name = path.filename().string();
    for (size_t i = 0; i < kDlc.size(); ++i) {
      if (_stricmp(name.c_str(), kDlc[i].file_name) == 0) r.dlc_index = int(i);
    }
    if (r.dlc_index < 0) {
      // Renamed file: match by its display name.
      for (size_t i = 0; i < kDlc.size(); ++i) {
        if (std::string(kDlc[i].title).find(info.display_name) == 0 && !info.display_name.empty()) {
          r.dlc_index = int(i);
        }
      }
    }
    if (r.dlc_index < 0) {
      r.message = "Unknown Ninja Gaiden II content \"" + info.display_name + "\" - not used.";
      return r;
    }
    r.ok = true;
    r.message = std::string(kDlc[size_t(r.dlc_index)].title) + " - verified.";
    return r;
  }
  r.message = "This Xbox 360 package (type " + Hex8(info.content_type) +
              ") is not the game, Title Update #2 or Ninja Gaiden II DLC.";
  return r;
}

}  // namespace

Identified Identify(const fs::path& in) {
  Identified r;
  std::error_code ec;
  fs::path path = in;
  if (!fs::exists(path, ec)) {
    r.message = "Not found: " + PathText(path);
    return r;
  }
  if (fs::is_directory(path, ec)) {
    // "<header>.data" folder picked instead of the header file next to it.
    if (path.extension() == ".data") {
      fs::path header = path;
      header.replace_extension();
      if (fs::is_regular_file(header, ec)) return Identify(header);
    }
    // Extracted game folder.
    if (fs::is_regular_file(path / "default.xex", ec)) {
      r.kind = Identified::Kind::kGame;
      r.game_kind = GameSourceKind::kFolder;
      r.path = path;
      std::ifstream f(path / "default.xex", std::ios::binary);
      std::vector<uint8_t> xex((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      r = CheckXex(r, xex);
      if (r.ok) r.message = "Ninja Gaiden II (extracted folder) - verified.";
      return r;
    }
    // A folder above the GOD package (e.g. "544307D5" or "00007000"): look for the header.
    for (auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (it.depth() > 3) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file(ec) && it->file_size(ec) >= 0x1000 && it->file_size(ec) < (1 << 20)) {
        const PackageInfo info = ReadPackageInfo(it->path());
        if (info.valid && info.content_type == 0x7000 && info.title_id == kTitleId) {
          return Identify(it->path());
        }
      }
    }
    r.message = "This folder holds no Ninja Gaiden II game files (no default.xex and no Games on "
                "Demand package " + std::string(kGodHeaderName) + ").";
    return r;
  }
  // A file.
  const std::string name = path.filename().string();
  if (_stricmp(name.c_str(), "default.xex") == 0) return Identify(path.parent_path());
  const PackageInfo info = ReadPackageInfo(path);
  if (info.valid) return FromPackage(path, info);
  std::string err;
  if (auto iso = OpenIso(path, &err)) {
    r.kind = Identified::Kind::kGame;
    r.game_kind = GameSourceKind::kIso;
    r.path = path;
    return CheckImage(r, *iso, "disc image");
  }
  if (_stricmp(name.c_str(), "default.xexp") == 0) {
    r.kind = Identified::Kind::kUpdate;
    r.message = std::string("Please add the title update package itself (") + kTu2FileName +
                "), not a file extracted from it.";
    return r;
  }
  r.message = "\"" + name + "\" is not a Ninja Gaiden II file. Expected the game (" + kGodHeaderName +
              " package, .iso or game folder), " + kTu2FileName + " or a DLC package.";
  return r;
}

DlcSet ScanDlcFolder(const fs::path& folder) {
  DlcSet set;
  set.folder = folder;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it.depth() > 2) {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec) || it->file_size(ec) < 0x1000) continue;
    const PackageInfo info = ReadPackageInfo(it->path());
    if (!info.valid || info.content_type != 0x2) continue;  // not DLC: ignore quietly
    Identified id = Identify(it->path());
    if (id.ok && id.dlc_index >= 0) {
      set.found[size_t(id.dlc_index)] = id;
    } else {
      set.problems.push_back(PathText(it->path().filename()) + ": " + id.message);
    }
  }
  return set;
}

}  // namespace launcher
