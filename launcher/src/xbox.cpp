// openNG2 launcher: Xbox 360 container readers. Ported from tools/god_extract.py and
// tools/stfs_extract.py.
#include "xbox.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace launcher {
namespace fs = std::filesystem;

namespace {

uint32_t BE32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint16_t LE16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t LE32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

std::string Utf16BeToUtf8(const uint8_t* p, size_t max_chars) {
  std::string out;
  for (size_t i = 0; i < max_chars; ++i) {
    const uint32_t c = uint32_t(p[i * 2]) << 8 | p[i * 2 + 1];
    if (!c) break;
    if (c < 0x80) {
      out.push_back(char(c));
    } else if (c < 0x800) {
      out.push_back(char(0xC0 | (c >> 6)));
      out.push_back(char(0x80 | (c & 0x3F)));
    } else {
      out.push_back(char(0xE0 | (c >> 12)));
      out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
      out.push_back(char(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

constexpr uint64_t kSector = 0x800;
constexpr char kXdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";

// ---- Games on Demand ---------------------------------------------------------------------
// Per Data part: 1 master hash block, then up to 0xCB subparts of 1 hash block + 0xCC data blocks.
class GodSource final : public ImageSource {
 public:
  explicit GodSource(std::vector<fs::path> parts) {
    for (auto& p : parts) files_.emplace_back(new std::ifstream(p, std::ios::binary));
  }
  bool Read(uint64_t offset, void* out, size_t size) override {
    constexpr uint64_t kBlock = 0x1000, kPerSub = 0xCC, kSubs = 0xCB, kPerPart = kPerSub * kSubs;
    auto* dst = static_cast<uint8_t*>(out);
    while (size) {
      const uint64_t blk = offset / kBlock, within = offset % kBlock;
      const uint64_t part = blk / kPerPart, rem = blk % kPerPart;
      const uint64_t sub = rem / kPerSub, b = rem % kPerSub;
      const uint64_t phys = kBlock + sub * (kBlock * (kPerSub + 1)) + kBlock + b * kBlock + within;
      const size_t n = size_t(std::min<uint64_t>(size, kBlock - within));
      if (part >= files_.size()) return false;
      std::ifstream& f = *files_[part];
      f.clear();
      f.seekg(std::streamoff(phys));
      if (!f.read(reinterpret_cast<char*>(dst), std::streamsize(n))) return false;
      dst += n;
      offset += n;
      size -= n;
    }
    return true;
  }

 private:
  std::vector<std::unique_ptr<std::ifstream>> files_;
};

// ---- Disc image --------------------------------------------------------------------------
class IsoSource final : public ImageSource {
 public:
  IsoSource(const fs::path& path, uint64_t base) : file_(path, std::ios::binary), base_(base) {}
  bool Read(uint64_t offset, void* out, size_t size) override {
    file_.clear();
    file_.seekg(std::streamoff(base_ + offset));
    return bool(file_.read(static_cast<char*>(out), std::streamsize(size)));
  }

 private:
  std::ifstream file_;
  uint64_t base_;
};

bool ReadVolume(ImageSource& src, uint32_t* root_sector, uint32_t* root_size) {
  uint8_t vd[0x20];
  if (!src.Read(0x10000, vd, sizeof(vd))) return false;
  if (std::memcmp(vd, kXdvdfsMagic, 20) != 0) return false;
  *root_sector = LE32(vd + 0x14);
  *root_size = LE32(vd + 0x18);
  return true;
}

bool Walk(ImageSource& src, uint32_t sector, uint32_t size, const std::string& rel,
          std::vector<XdvdfsEntry>* out, int depth) {
  if (depth > 32 || !size) return true;
  std::vector<uint8_t> table(size);
  if (!src.Read(sector * kSector, table.data(), size)) return false;
  std::vector<uint32_t> stack{0};
  std::vector<bool> seen(size / 4 + 1);
  while (!stack.empty()) {
    const uint32_t off = stack.back();
    stack.pop_back();
    if (off + 14 > size || seen[off / 4]) continue;
    seen[off / 4] = true;
    const uint16_t left = LE16(&table[off]), right = LE16(&table[off + 2]);
    if (left == 0xFFFF && right == 0xFFFF) continue;
    const uint32_t s = LE32(&table[off + 4]), len = LE32(&table[off + 8]);
    const uint8_t attr = table[off + 12], nlen = table[off + 13];
    if (off + 14 + nlen > size) continue;
    const std::string name(reinterpret_cast<const char*>(&table[off + 14]), nlen);
    if (left) stack.push_back(uint32_t(left) * 4);
    if (right) stack.push_back(uint32_t(right) * 4);
    if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos) {
      continue;  // never write outside the target folder
    }
    XdvdfsEntry e;
    e.path = rel.empty() ? name : rel + "/" + name;
    e.directory = (attr & 0x10) != 0;
    e.offset = uint64_t(s) * kSector;
    e.size = len;
    out->push_back(e);
    if (e.directory && !Walk(src, s, len, e.path, out, depth + 1)) return false;
  }
  return true;
}

}  // namespace

PackageInfo ReadPackageInfo(const fs::path& file) {
  PackageInfo info;
  std::ifstream f(file, std::ios::binary);
  uint8_t h[0x500];
  if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) return info;
  info.magic.assign(reinterpret_cast<char*>(h), 4);
  if (info.magic != "LIVE" && info.magic != "CON " && info.magic != "PIRS") return info;
  info.valid = true;
  info.content_type = BE32(h + 0x344);
  info.media_id = BE32(h + 0x354);
  info.version = BE32(h + 0x358);
  info.title_id = BE32(h + 0x360);
  info.display_name = Utf16BeToUtf8(h + 0x411, 0x40);
  return info;
}

std::unique_ptr<ImageSource> OpenGod(const fs::path& header, std::string* error) {
  fs::path data_dir = header;
  data_dir += ".data";
  std::error_code ec;
  if (!fs::is_directory(data_dir, ec)) {
    *error = "The folder \"" + data_dir.filename().string() +
             "\" must be next to the package file (it holds the game data).";
    return nullptr;
  }
  std::vector<fs::path> parts;
  for (const auto& e : fs::directory_iterator(data_dir, ec)) {
    const std::string n = e.path().filename().string();
    if (e.is_regular_file() && n.rfind("Data", 0) == 0) parts.push_back(e.path());
  }
  std::sort(parts.begin(), parts.end());
  if (parts.empty()) {
    *error = "\"" + data_dir.filename().string() + "\" contains no Data0000... files.";
    return nullptr;
  }
  auto src = std::make_unique<GodSource>(parts);
  uint32_t a, b;
  if (!ReadVolume(*src, &a, &b)) {
    *error = "The Games on Demand data is incomplete or damaged.";
    return nullptr;
  }
  return src;
}

std::unique_ptr<ImageSource> OpenIso(const fs::path& iso, std::string* error) {
  // Plain XDVDFS image, or a full disc dump with the game partition at XGD2 / XGD3 / XGD1.
  for (uint64_t base : {0x0ull, 0xFD90000ull, 0x2080000ull, 0x18300000ull}) {
    auto src = std::make_unique<IsoSource>(iso, base);
    uint32_t a, b;
    if (ReadVolume(*src, &a, &b)) return src;
  }
  *error = "This file is not an Xbox 360 disc image.";
  return nullptr;
}

bool ListXdvdfs(ImageSource& source, std::vector<XdvdfsEntry>* entries, std::string* error) {
  uint32_t root_sector, root_size;
  if (!ReadVolume(source, &root_sector, &root_size)) {
    *error = "Game file system not found.";
    return false;
  }
  entries->clear();
  if (!Walk(source, root_sector, root_size, "", entries, 0)) {
    *error = "Reading the game file system failed (damaged or incomplete files).";
    return false;
  }
  return true;
}

bool ReadXdvdfsFile(ImageSource& source, const XdvdfsEntry& entry, std::vector<uint8_t>* out) {
  out->resize(size_t(entry.size));
  return source.Read(entry.offset, out->data(), out->size());
}

bool ExtractXdvdfs(ImageSource& source, const fs::path& out_dir,
                   const std::function<void(uint64_t, uint64_t)>& progress,
                   const std::function<bool()>& cancel, std::string* error) {
  std::vector<XdvdfsEntry> entries;
  if (!ListXdvdfs(source, &entries, error)) return false;
  uint64_t total = 0, done = 0;
  for (const auto& e : entries) total += e.directory ? 0 : e.size;
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  std::vector<uint8_t> buf(8u << 20);
  for (const auto& e : entries) {
    const fs::path dst = out_dir / fs::u8path(e.path);
    if (e.directory) {
      fs::create_directories(dst, ec);
      continue;
    }
    fs::create_directories(dst.parent_path(), ec);
    // Already extracted (a previous attempt): skip if the size matches.
    if (fs::is_regular_file(dst, ec) && fs::file_size(dst, ec) == e.size) {
      done += e.size;
      progress(done, total);
      continue;
    }
    std::ofstream o(dst, std::ios::binary | std::ios::trunc);
    if (!o) {
      *error = "Cannot write " + dst.string() + " (disk full or no permission?)";
      return false;
    }
    uint64_t pos = 0;
    while (pos < e.size) {
      if (cancel()) {
        o.close();
        fs::remove(dst, ec);
        *error = "Cancelled.";
        return false;
      }
      const size_t n = size_t(std::min<uint64_t>(buf.size(), e.size - pos));
      if (!source.Read(e.offset + pos, buf.data(), n)) {
        *error = "Read error in " + e.path + " (damaged or incomplete game files).";
        return false;
      }
      if (!o.write(reinterpret_cast<char*>(buf.data()), std::streamsize(n))) {
        *error = "Write error (disk full?) at " + dst.string();
        return false;
      }
      pos += n;
      done += n;
      progress(done, total);
    }
  }
  return true;
}

bool ExtractStfsFile(const fs::path& package, const std::string& inner_path,
                     const fs::path& out_file, std::string* error) {
  std::ifstream f(package, std::ios::binary);
  std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (d.size() < 0x1000) {
    *error = "Package too small.";
    return false;
  }
  const uint32_t header_size = BE32(&d[0x340]);
  const uint8_t* vd = &d[0x379];
  const uint8_t block_sep = vd[2];
  const uint32_t ft_count = LE16(vd + 3);
  const uint32_t ft_block = uint32_t(vd[5]) | uint32_t(vd[6]) << 8 | uint32_t(vd[7]) << 16;
  const uint32_t shift = (block_sep & 1) ? 0 : 1;
  const uint64_t first = (uint64_t(header_size) + 0xFFF) & ~0xFFFull;
  auto data_block = [&](uint64_t b) -> uint64_t {
    const uint64_t r = (((b + 0xAA) / 0xAA) << shift) + b;
    if (b < 0xAA) return r;
    if (b < 0x70E4) return r + (((b + 0x70E4) / 0x70E4) << shift);
    return (1ull << shift) + r + (((b + 0x70E4) / 0x70E4) << shift);
  };
  auto block_off = [&](uint64_t b) { return first + (data_block(b) << 12); };
  auto level0_hash_off = [&](uint64_t b) {
    uint64_t n = (b / 0xAA) * (0xAA + (1ull << shift));
    if (b / 0xAA) n += ((b / 0x70E4) + 1) << shift;
    if (b / 0x70E4) n += 1ull << shift;
    return first + (n << 12);
  };
  auto next_block = [&](uint64_t b) -> uint64_t {
    const uint64_t e = level0_hash_off(b) + (b % 0xAA) * 0x18;
    if (e + 0x18 > d.size()) return ~0ull;
    return uint64_t(d[e + 0x15]) << 16 | uint64_t(d[e + 0x16]) << 8 | d[e + 0x17];
  };
  struct Entry { std::string name; uint8_t flags; uint32_t start; int16_t parent; uint32_t size; };
  std::vector<Entry> entries;
  uint64_t b = ft_block;
  for (uint32_t i = 0; i < ft_count && b != ~0ull; ++i) {
    const uint64_t off = block_off(b);
    if (off + 0x1000 > d.size()) break;
    for (int k = 0; k < 0x40; ++k) {
      const uint8_t* e = &d[off + k * 0x40];
      const uint8_t flags = e[0x28];
      const uint8_t nlen = flags & 0x3F;
      if (!nlen) continue;
      Entry en;
      en.name.assign(reinterpret_cast<const char*>(e), nlen);
      en.flags = flags;
      en.start = uint32_t(e[0x2F]) | uint32_t(e[0x30]) << 8 | uint32_t(e[0x31]) << 16;
      en.parent = int16_t(uint16_t(e[0x32]) << 8 | e[0x33]);
      en.size = BE32(e + 0x34);
      entries.push_back(en);
    }
    b = next_block(b);
  }
  std::function<std::string(int)> full = [&](int i) -> std::string {
    if (i < 0 || i >= int(entries.size())) return {};
    const Entry& e = entries[size_t(i)];
    return e.parent == -1 ? e.name : full(e.parent) + "/" + e.name;
  };
  for (int i = 0; i < int(entries.size()); ++i) {
    const Entry& e = entries[size_t(i)];
    if ((e.flags & 0x80) || full(i) != inner_path) continue;
    std::vector<uint8_t> out;
    out.reserve(e.size);
    uint64_t blk = e.start;
    while (out.size() < e.size) {
      const uint64_t o = block_off(blk);
      if (o + 0x1000 > d.size() && o + (e.size - out.size()) > d.size()) {
        *error = "The package is damaged (file data out of range).";
        return false;
      }
      const size_t n = std::min<size_t>(0x1000, e.size - out.size());
      out.insert(out.end(), d.begin() + std::ptrdiff_t(o), d.begin() + std::ptrdiff_t(o + n));
      blk = (e.flags & 0x40) ? blk + 1 : next_block(blk);
      if (blk == ~0ull) break;
    }
    if (out.size() != e.size) {
      *error = "The package is damaged (file truncated).";
      return false;
    }
    std::error_code ec;
    fs::create_directories(out_file.parent_path(), ec);
    std::ofstream o(out_file, std::ios::binary | std::ios::trunc);
    if (!o.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()))) {
      *error = "Cannot write " + out_file.string();
      return false;
    }
    return true;
  }
  *error = "\"" + inner_path + "\" was not found in the package.";
  return false;
}

}  // namespace launcher
