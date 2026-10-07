// xiso_extract - extracts the game partition of an Xbox / Xbox 360 disc image
// (XDVDFS file system) to a folder.
//
//   xiso_extract <image.iso> <output-dir> [--skip-name NAME ...]
//
// Supports plain XISO images and full disc dumps (XGD1/XGD2/XGD3 layouts).
// Part of FNC Reborn (originally written for Saints Reborn).

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr uint64_t kSectorSize = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicLen = 20;

struct Image {
  FILE* f = nullptr;
  uint64_t partition = 0;

  bool Read(uint64_t offset, void* dst, size_t size) {
#ifdef _WIN32
    if (_fseeki64(f, int64_t(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(f, off_t(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(dst, 1, size, f) == size;
  }
};

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

std::set<std::string> g_skip;
uint64_t g_files = 0, g_bytes = 0;

bool ExtractFile(Image& img, uint32_t sector, uint32_t size, const fs::path& out) {
  FILE* o = nullptr;
#ifdef _WIN32
  o = _wfopen(out.c_str(), L"wb");
#else
  o = std::fopen(out.c_str(), "wb");
#endif
  if (!o) {
    std::fprintf(stderr, "error: cannot create %s\n", out.string().c_str());
    return false;
  }
  std::vector<uint8_t> buf(4 << 20);
  uint64_t offset = img.partition + uint64_t(sector) * kSectorSize;
  uint64_t left = size;
  while (left) {
    size_t chunk = size_t(std::min<uint64_t>(left, buf.size()));
    if (!img.Read(offset, buf.data(), chunk) || std::fwrite(buf.data(), 1, chunk, o) != chunk) {
      std::fprintf(stderr, "error: I/O failure on %s\n", out.string().c_str());
      std::fclose(o);
      return false;
    }
    offset += chunk;
    left -= chunk;
  }
  std::fclose(o);
  ++g_files;
  g_bytes += size;
  return true;
}

bool ExtractDir(Image& img, uint32_t sector, uint32_t size, const fs::path& out, int depth);

// Directory entries form a binary tree; offsets are in 4-byte units from the
// start of the directory table.
bool WalkTree(Image& img, const std::vector<uint8_t>& table, uint32_t offset,
              const fs::path& out, int depth) {
  std::vector<uint32_t> stack{offset};
  std::set<uint32_t> seen;
  while (!stack.empty()) {
    uint32_t pos = stack.back() * 4;
    stack.pop_back();
    if (pos + 14 > table.size() || !seen.insert(pos).second) continue;
    const uint8_t* e = table.data() + pos;
    uint16_t left = Le16(e), right = Le16(e + 2);
    if (left == 0xFFFF && right == 0xFFFF && Le32(e + 4) == 0xFFFFFFFF) continue;  // padding
    uint32_t entry_sector = Le32(e + 4), entry_size = Le32(e + 8);
    uint8_t attr = e[12], name_len = e[13];
    if (pos + 14 + name_len > table.size()) continue;
    std::string name(reinterpret_cast<const char*>(e + 14), name_len);
    if (left && left != 0xFFFF) stack.push_back(left);
    if (right && right != 0xFFFF) stack.push_back(right);
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos || g_skip.count(name)) {
      continue;
    }
    fs::path target = out / fs::u8path(name);
    if (attr & 0x10) {
      if (!ExtractDir(img, entry_sector, entry_size, target, depth + 1)) return false;
    } else {
      if (!ExtractFile(img, entry_sector, entry_size, target)) return false;
    }
  }
  return true;
}

bool ExtractDir(Image& img, uint32_t sector, uint32_t size, const fs::path& out, int depth) {
  std::error_code ec;
  fs::create_directories(out, ec);
  if (depth > 64 || size == 0) return true;
  if (size > (64u << 20)) {
    std::fprintf(stderr, "error: implausible directory size\n");
    return false;
  }
  std::vector<uint8_t> table(size);
  if (!img.Read(img.partition + uint64_t(sector) * kSectorSize, table.data(), size)) {
    std::fprintf(stderr, "error: cannot read directory table\n");
    return false;
  }
  return WalkTree(img, table, 0, out, depth);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: xiso_extract <image.iso> <output-dir> [--skip-name NAME ...]\n");
    return 2;
  }
  for (int i = 3; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--skip-name") == 0) g_skip.insert(argv[i + 1]);
  }

  Image img;
#ifdef _WIN32
  img.f = _wfopen(fs::u8path(argv[1]).c_str(), L"rb");
#else
  img.f = std::fopen(argv[1], "rb");
#endif
  if (!img.f) {
    std::fprintf(stderr, "error: cannot open %s\n", argv[1]);
    return 1;
  }

  // Plain XISO, XGD2, XGD1, XGD3 game partition offsets.
  const uint64_t candidates[] = {0, 0xFD90000ull, 0x18300000ull, 0x2080000ull};
  uint8_t vd[kSectorSize];
  bool found = false;
  for (uint64_t c : candidates) {
    if (img.Read(c + 32 * kSectorSize, vd, sizeof(vd)) && std::memcmp(vd, kMagic, kMagicLen) == 0 &&
        std::memcmp(vd + 0x7EC, kMagic, kMagicLen) == 0) {
      img.partition = c;
      found = true;
      break;
    }
  }
  if (!found) {
    std::fprintf(stderr, "error: no Xbox (XDVDFS) file system found in %s\n", argv[1]);
    return 1;
  }

  uint32_t root_sector = Le32(vd + 20), root_size = Le32(vd + 24);
  fs::path out = fs::u8path(argv[2]);
  if (!ExtractDir(img, root_sector, root_size, out, 0)) return 1;
  std::fclose(img.f);
  std::printf("Extracted %llu files (%.1f MB)\n", (unsigned long long)g_files, g_bytes / 1048576.0);
  return 0;
}
