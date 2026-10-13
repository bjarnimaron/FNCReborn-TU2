// fnc_content - installs the player's own Xbox 360 content for FNC Reborn (TU2).
//
// Uses the ReXGlue SDK's own STFS reader (the same code the runtime mounts packages with), so
// it reads CON / LIVE / PIRS packages exactly as the game does.
//
//   fnc_content info <package>...
//       Prints each package's title id, content type and name.
//   fnc_content extract-file <package> <name> <out file>
//       Extracts one file (e.g. default.xexp from a title update package).
//   fnc_content install <package> <content root> [--license <hex mask>]
//       Installs a DLC package the way the runtime's ContentManager does:
//       <root>\0000000000000000\<title>\<type>\<package name>\ (the package's files) and
//       <root>\0000000000000000\<title>\Headers\<type>\<package name>.header
//       (XCONTENT_AGGREGATE_DATA followed by the license mask, default FFFFFFFF).
//   fnc_content unlzx <in> <out>
//       Decompresses an EA "chunklzx" file.
//   fnc_content big-extract <archive.big> <member name> <out>
//       Extracts one file from an EA .big archive.
//
// Exit code 0 on success; errors are printed to stderr.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <queue>
#include <span>
#include <string>
#include <vector>

#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem.h>
#include <rex/string.h>
#include <rex/system/xtypes.h>
#include <rex/system/lzx.h>
#include <rex/system/xam/content_manager.h>

namespace fs = std::filesystem;
using rex::filesystem::Entry;
using rex::filesystem::StfsContainerDevice;
using rex::X_STATUS;  // for X_STATUS_SUCCESS
using rex::system::xam::XCONTENT_AGGREGATE_DATA;

namespace {

int Fail(const std::string& message) {
  std::fprintf(stderr, "error: %s\n", message.c_str());
  return 1;
}

std::string Hex8(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", v);
  return buf;
}

// Absolute path with the \\?\ prefix, so output paths longer than 260 characters work.
fs::path LongPath(const fs::path& p) {
#ifdef _WIN32
  std::wstring w = fs::absolute(p).make_preferred().wstring();
  if (w.rfind(L"\\\\?\\", 0) != 0) w = L"\\\\?\\" + w;
  return fs::path(w);
#else
  return p;
#endif
}

std::unique_ptr<StfsContainerDevice> Open(const fs::path& package) {
  if (!fs::is_regular_file(package)) return nullptr;
  auto device = std::make_unique<StfsContainerDevice>("", LongPath(package));
  if (!device->Initialize()) return nullptr;
  return device;
}

uint32_t TitleId(const StfsContainerDevice& d) { return d.header().metadata.execution_info.title_id; }
uint32_t ContentType(const StfsContainerDevice& d) {
  return static_cast<uint32_t>(static_cast<rex::system::XContentType>(d.header().metadata.content_type));
}
std::string DisplayName(const StfsContainerDevice& d) {
  return rex::string::to_utf8(d.header().metadata.display_name(rex::system::XLanguage::kEnglish));
}

bool CopyEntry(Entry* entry, const fs::path& out) {
  auto* file = static_cast<rex::filesystem::File*>(nullptr);
  if (entry->Open(rex::filesystem::FileAccess::kFileReadData, &file) != X_STATUS_SUCCESS || !file) {
    return false;
  }
  std::ofstream os(out, std::ios::binary | std::ios::trunc);
  std::vector<uint8_t> buffer(1 << 20);
  size_t offset = 0, remaining = entry->size();
  bool ok = bool(os);
  while (ok && remaining) {
    size_t n = std::min(buffer.size(), remaining), got = 0;
    // Like the runtime's own ContentManager::InstallContent: the status can be non-zero on a
    // successful read, so only the byte count counts.
    file->ReadSync(std::span<uint8_t>(buffer.data(), n), offset, &got);
    if (!got) {
      ok = false;
      break;
    }
    os.write(reinterpret_cast<const char*>(buffer.data()), std::streamsize(got));
    offset += got;
    remaining -= got;
  }
  file->Destroy();
  return ok && bool(os);
}

// Extracts every file of the package under out_dir.
bool ExtractAll(StfsContainerDevice& device, const fs::path& out_dir, size_t* count) {
  auto* root = device.ResolvePath("");
  if (!root) return false;
  std::queue<Entry*> queue;
  queue.push(root);
  while (!queue.empty()) {
    Entry* entry = queue.front();
    queue.pop();
    for (auto& child : entry->children()) queue.push(child.get());
    if (entry == root) continue;
    fs::path target = out_dir / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));
    if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
      fs::create_directories(target);
    } else {
      fs::create_directories(target.parent_path());
      if (!CopyEntry(entry, target)) {
        std::fprintf(stderr, "error: cannot extract %s\n", entry->path().c_str());
        return false;
      }
      ++*count;
    }
  }
  return true;
}

int CmdInfo(const std::vector<std::string>& args) {
  int status = 0;
  for (const auto& a : args) {
    auto device = Open(rex::to_path(a));
    if (!device) {
      std::printf("%s\tnot a package\n", a.c_str());
      status = 1;
      continue;
    }
    std::printf("%s\t%s\t%s\t%s\n", a.c_str(), Hex8(TitleId(*device)).c_str(),
                Hex8(ContentType(*device)).c_str(), DisplayName(*device).c_str());
  }
  return status;
}

int CmdExtractFile(const fs::path& package, const std::string& name, const fs::path& out) {
  auto device = Open(package);
  if (!device) return Fail("cannot open package " + rex::path_to_utf8(package));
  auto* entry = device->ResolvePath(name);
  if (!entry) return Fail(name + " not found in " + rex::path_to_utf8(package));
  fs::path target = LongPath(out);
  fs::create_directories(target.parent_path());
  if (!CopyEntry(entry, target)) return Fail("cannot extract " + name);
  return 0;
}

int CmdInstall(const fs::path& package, const fs::path& root, uint32_t license) {
  auto device = Open(package);
  if (!device) return Fail("cannot open package " + rex::path_to_utf8(package));
  std::string name = rex::path_to_utf8(package.filename());
  if (name.size() > 42) return Fail("package file name longer than 42 characters: " + name);

  XCONTENT_AGGREGATE_DATA data{};
  data.device_id = 1;  // hard drive
  data.content_type = static_cast<rex::system::XContentType>(ContentType(*device));
  data.title_id = TitleId(*device);
  data.xuid = 0;
  data.set_file_name(name);
  auto display = device->header().metadata.display_name(rex::system::XLanguage::kEnglish);
  data.set_display_name(display.empty() ? rex::path_to_utf16(package.filename()) : display);

  fs::path base = LongPath(root) / "0000000000000000" / Hex8(TitleId(*device));
  fs::path content_dir = base / Hex8(ContentType(*device)) / name;
  fs::path header = base / "Headers" / Hex8(ContentType(*device)) / (name + ".header");

  std::error_code ec;
  fs::remove_all(content_dir, ec);
  fs::create_directories(content_dir);
  size_t files = 0;
  if (!ExtractAll(*device, content_dir, &files)) return Fail("extracting " + name + " failed");

  fs::create_directories(header.parent_path());
  std::ofstream h(header, std::ios::binary | std::ios::trunc);
  h.write(reinterpret_cast<const char*>(&data), sizeof(data));
  h.write(reinterpret_cast<const char*>(&license), sizeof(license));
  if (!h) return Fail("cannot write " + rex::path_to_utf8(header));
  std::printf("installed %s (%s, %zu files)\n", DisplayName(*device).c_str(), name.c_str(), files);
  return 0;
}

uint32_t Be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint32_t Be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

// EA "chunklzx": "chunklzx", u32 version, u32 total size, u32 chunk size, u32 chunk count,
// u32 alignment, padding to 0x28; then per chunk {u32 compressed size, u32 type, data}, with each
// chunk's data aligned to the alignment (its 8-byte header sits just before it). Type 3 data is
// xcompress-framed LZX: frames of [0xFF, u16 output size] (optional), u16 input size, input.
int CmdUnlzx(const fs::path& in_path, const fs::path& out_path) {
  std::ifstream is(LongPath(in_path), std::ios::binary);
  std::vector<uint8_t> in((std::istreambuf_iterator<char>(is)), {});
  if (in.size() < 0x28 || std::memcmp(in.data(), "chunklzx", 8) != 0) {
    return Fail(rex::path_to_utf8(in_path) + " is not a chunklzx file");
  }
  uint32_t total = Be32(&in[0xC]), chunk = Be32(&in[0x10]), count = Be32(&in[0x14]);
  uint32_t align = std::max<uint32_t>(1, Be32(&in[0x18]));
  std::vector<uint8_t> out(total), stream;
  size_t off = 0x28, done = 0;
  for (uint32_t c = 0; c < count; ++c) {
    if (off + 8 > in.size()) return Fail("truncated chunk header");
    uint32_t csize = Be32(&in[off]), type = Be32(&in[off + 4]);
    off += 8;
    size_t want = std::min<size_t>(chunk, total - done);
    if (off + csize > in.size()) return Fail("truncated chunk");
    if (type != 3) {
      std::memcpy(&out[done], &in[off], want);
    } else {
      stream.clear();
      for (size_t i = off, end = off + csize; i + 2 <= end;) {
        if (in[i] == 0xFF) i += 3;
        if (i + 2 > end) break;
        uint32_t n = Be16(&in[i]);
        i += 2;
        if (!n || i + n > end) break;
        stream.insert(stream.end(), in.begin() + i, in.begin() + i + n);
        i += n;
      }
      bool ok = false;
      for (uint32_t bits : {17u, 18u, 19u, 20u, 21u, 15u, 16u}) {
        if (lzx_decompress(stream.data(), stream.size(), &out[done], want, 1u << bits, nullptr, 0) == 0) {
          ok = true;
          break;
        }
      }
      if (!ok) return Fail("LZX decode failed in chunk " + std::to_string(c));
    }
    done += want;
    off += csize + 8;
    off = (off + align - 1) / align * align - 8;
  }
  std::ofstream os(LongPath(out_path), std::ios::binary | std::ios::trunc);
  os.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  if (!os) return Fail("cannot write " + rex::path_to_utf8(out_path));
  return 0;
}

// EA ".big" archive (the "EB" layout Fight Night Champion uses): u32 file count at 4, name table
// offset/size at 0xC/0x10, name record length at 0x14, path record length at 0x15; at 0x30 one
// {u32 offset >> 4, u32 flags, u32 size, u32 hash} per file; name records are {u16 path index,
// name}. Extracts the member called `name` (stored as it is, e.g. still chunklzx-compressed).
int CmdBigExtract(const fs::path& big, const std::string& name, const fs::path& out_path) {
  std::ifstream is(LongPath(big), std::ios::binary);
  std::vector<uint8_t> hdr(0x30);
  if (!is.read(reinterpret_cast<char*>(hdr.data()), hdr.size())) return Fail("cannot read " + rex::path_to_utf8(big));
  uint32_t count = Be32(&hdr[4]), names_off = Be32(&hdr[0xC]), names_size = Be32(&hdr[0x10]);
  uint32_t name_len = hdr[0x14];
  if (!count || name_len < 3) return Fail(rex::path_to_utf8(big) + " is not a .big archive");
  std::vector<uint8_t> table(size_t(count) * 16), names(names_size);
  is.seekg(0x30);
  is.read(reinterpret_cast<char*>(table.data()), table.size());
  is.seekg(names_off);
  is.read(reinterpret_cast<char*>(names.data()), names.size());
  if (!is) return Fail("truncated archive " + rex::path_to_utf8(big));
  for (uint32_t i = 0; i < count; ++i) {
    if (size_t(i + 1) * name_len > names.size()) break;
    const char* rec = reinterpret_cast<const char*>(&names[size_t(i) * name_len + 2]);
    std::string member(rec, strnlen(rec, name_len - 2));
    if (member != name) continue;
    uint64_t offset = uint64_t(Be32(&table[i * 16])) << 4;
    uint32_t size = Be32(&table[i * 16 + 8]);
    std::vector<uint8_t> data(size);
    is.seekg(std::streamoff(offset));
    if (!is.read(reinterpret_cast<char*>(data.data()), size)) return Fail("cannot read " + name);
    std::ofstream os(LongPath(out_path), std::ios::binary | std::ios::trunc);
    os.write(reinterpret_cast<const char*>(data.data()), size);
    return os ? 0 : Fail("cannot write " + rex::path_to_utf8(out_path));
  }
  return Fail(name + " not found in " + rex::path_to_utf8(big));
}

int Usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  fnc_content info <package>...\n"
               "  fnc_content extract-file <package> <name> <out file>\n"
               "  fnc_content install <package> <content root> [--license <hex mask>]\n"
               "  fnc_content unlzx <in> <out>\n"
               "  fnc_content big-extract <archive.big> <member name> <out>\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return Usage();
  const std::string cmd = args[0];
  args.erase(args.begin());
  if (cmd == "info" && !args.empty()) return CmdInfo(args);
  if (cmd == "extract-file" && args.size() == 3) {
    return CmdExtractFile(rex::to_path(args[0]), args[1], rex::to_path(args[2]));
  }
  if (cmd == "install" && (args.size() == 2 || args.size() == 4)) {
    uint32_t license = 0xFFFFFFFF;
    if (args.size() == 4) {
      if (args[2] != "--license") return Usage();
      license = uint32_t(std::stoul(args[3], nullptr, 16));
    }
    return CmdInstall(rex::to_path(args[0]), rex::to_path(args[1]), license);
  }
  if (cmd == "unlzx" && args.size() == 2) return CmdUnlzx(rex::to_path(args[0]), rex::to_path(args[1]));
  if (cmd == "big-extract" && args.size() == 3) {
    return CmdBigExtract(rex::to_path(args[0]), args[1], rex::to_path(args[2]));
  }
  return Usage();
}
