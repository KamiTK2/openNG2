// openNG2 launcher: Xbox 360 container readers (Games on Demand, disc images, STFS packages).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace launcher {

// Header fields shared by STFS-style packages (LIVE / CON / PIRS): title updates, DLC and the
// Games on Demand header file.
struct PackageInfo {
  bool valid = false;
  std::string magic;         // "LIVE", "CON ", "PIRS"
  uint32_t content_type = 0;  // 0x2 marketplace content, 0x7000 GOD, 0xB0000 title update
  uint32_t title_id = 0;
  uint32_t media_id = 0;
  uint32_t version = 0;
  std::string display_name;  // UTF-8
};
PackageInfo ReadPackageInfo(const std::filesystem::path& file);

// Random-access byte source for an XDVDFS game image.
class ImageSource {
 public:
  virtual ~ImageSource() = default;
  // Reads `size` bytes at image offset `offset`; false on a short read.
  virtual bool Read(uint64_t offset, void* out, size_t size) = 0;
};
// Games on Demand: header file + "<header>.data\DataNNNN" part files.
std::unique_ptr<ImageSource> OpenGod(const std::filesystem::path& header, std::string* error);
// Disc image (.iso): plain XDVDFS image or a full dump with the game partition at a known offset.
std::unique_ptr<ImageSource> OpenIso(const std::filesystem::path& iso, std::string* error);

struct XdvdfsEntry {
  std::string path;  // relative, '/' separated
  bool directory = false;
  uint64_t offset = 0;  // in the image
  uint64_t size = 0;
};
// Lists the file tree (the volume descriptor is found automatically).
bool ListXdvdfs(ImageSource& source, std::vector<XdvdfsEntry>* entries, std::string* error);
// Reads one file of the tree into memory (small files only, e.g. default.xex).
bool ReadXdvdfsFile(ImageSource& source, const XdvdfsEntry& entry, std::vector<uint8_t>* out);
// Extracts the whole tree to `out_dir`; progress(bytes_done, bytes_total); returns false on
// error or when `cancel` becomes true.
bool ExtractXdvdfs(ImageSource& source, const std::filesystem::path& out_dir,
                   const std::function<void(uint64_t, uint64_t)>& progress,
                   const std::function<bool()>& cancel, std::string* error);

// Extracts one file (by its path inside the package, e.g. "default.xexp") from an STFS package.
bool ExtractStfsFile(const std::filesystem::path& package, const std::string& inner_path,
                     const std::filesystem::path& out_file, std::string* error);

}  // namespace launcher
