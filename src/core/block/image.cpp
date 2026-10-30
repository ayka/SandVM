#include "core/block/image.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "core/block/host_file.h"
#include "core/block/vhd.h"
#include "core/block/vmdk.h"

namespace sandvm::block {
namespace {
constexpr int kMaxChainDepth = 64;
constexpr size_t kCopyChunk = 1u << 20;

bool ends_with_ci(const std::wstring& s, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i]) return false;
  }
  return true;
}

uint64_t allocated_size(const std::wstring& path) {
  DWORD high = 0;
  const DWORD low = GetCompressedFileSizeW(path.c_str(), &high);
  if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return 0;
  return (uint64_t{high} << 32) | low;
}

std::unique_ptr<VhdDisk> open_vhd(const std::wstring& path, bool writable, bool open_parent) {
  auto file = HostFile::open(path, writable);
  if (!VhdDisk::probe(*file)) throw std::runtime_error(narrow(path) + " is not a VHD image");
  return VhdDisk::open(std::move(file), open_parent, 0);
}
}  // namespace

const char* to_string(ImageFormat format) {
  switch (format) {
    case ImageFormat::Raw: return "raw";
    case ImageFormat::Vhd: return "vhd";
    case ImageFormat::Vmdk: return "vmdk";
  }
  return "?";
}

ImageFormat format_from_name(const std::wstring& path) {
  if (ends_with_ci(path, L".vhd")) return ImageFormat::Vhd;
  if (ends_with_ci(path, L".vmdk")) return ImageFormat::Vmdk;
  return ImageFormat::Raw;
}

ImageFormat probe_format(const std::wstring& path) {
  auto file = HostFile::open(path, false);
  if (VhdDisk::probe(*file)) return ImageFormat::Vhd;
  if (VmdkDisk::probe(*file)) return ImageFormat::Vmdk;
  return ImageFormat::Raw;
}

std::unique_ptr<Disk> open_image(const std::wstring& path, bool read_only, int depth) {
  if (depth > kMaxChainDepth) throw std::runtime_error("image chain too deep (a parent loop?) at " + narrow(path));
  auto file = HostFile::open(path, !read_only);
  if (VhdDisk::probe(*file)) return VhdDisk::open(std::move(file), /*open_parent=*/true, depth);
  if (VmdkDisk::probe(*file)) return VmdkDisk::open(std::move(file), depth);
  const uint64_t size = file->size();
  return std::make_unique<FileDisk>(std::move(file), size);
}

void create_image(const std::wstring& path, ImageFormat format, uint64_t size_bytes) {
  switch (format) {
    case ImageFormat::Vhd:
      VhdDisk::create_dynamic(path, size_bytes);
      return;
    case ImageFormat::Vmdk:
      VmdkDisk::create_sparse(path, size_bytes);
      return;
    case ImageFormat::Raw:
      break;
  }
  if (size_bytes == 0 || size_bytes % 512) throw std::runtime_error("raw image size must be a non-zero multiple of 512");
  auto file = HostFile::create(path);
  // Not sparse on file systems without support (FAT): the image is then fully allocated.
  file->set_sparse();
  const bool ok = file->set_size(size_bytes);
  file.reset();
  if (!ok) {
    DeleteFileW(path.c_str());
    throw std::runtime_error("cannot size " + narrow(path));
  }
}

void create_differencing_image(const std::wstring& path, const std::wstring& parent) {
  VhdDisk::create_differencing(path, parent);
}

std::vector<ImageInfo> image_chain(const std::wstring& path) {
  std::vector<ImageInfo> chain;
  std::wstring current = absolute_path(path);
  while (!current.empty() && chain.size() <= kMaxChainDepth) {
    ImageInfo info;
    info.path = current;
    info.file_size = allocated_size(current);
    auto file = HostFile::open(current, false);
    if (VhdDisk::probe(*file)) {
      info.format = ImageFormat::Vhd;
      auto vhd = VhdDisk::open(std::move(file), /*open_parent=*/false, 0);
      info.virtual_size = vhd->size_bytes();
      switch (vhd->type()) {
        case VhdDisk::Type::Fixed: info.subformat = "fixed"; break;
        case VhdDisk::Type::Dynamic: info.subformat = "dynamic"; break;
        case VhdDisk::Type::Differencing: info.subformat = "differencing"; break;
      }
      info.parent = vhd->parent_path();
    } else if (VmdkDisk::probe(*file)) {
      info.format = ImageFormat::Vmdk;
      // Opens the parents as well; only this level is reported here, the loop walks the rest.
      auto vmdk = VmdkDisk::open(std::move(file), 0);
      info.virtual_size = vmdk->size_bytes();
      info.subformat = vmdk->create_type();
      info.parent = vmdk->parent_path();
    } else {
      info.format = ImageFormat::Raw;
      info.virtual_size = file->size();
      info.subformat = "raw";
    }
    current = info.parent;
    chain.push_back(std::move(info));
  }
  return chain;
}

void commit_image(const std::wstring& child_path, const Progress& progress) {
  auto child = open_vhd(child_path, false, /*open_parent=*/false);
  if (child->type() != VhdDisk::Type::Differencing) throw std::runtime_error(narrow(child_path) + " has no parent");
  auto parent = open_image(child->parent_path(), /*read_only=*/false);
  uint64_t total = 0, done = 0, end = 0;
  child->for_each_own_range([&](uint64_t offset, uint64_t len) {
    total += len;
    end = std::max(end, offset + len);
    return true;
  });
  // A parent shorter than the child (a raw image whose size is not a multiple of the sector) cannot
  // take sectors past its end; refuse instead of dropping them.
  if (end > parent->size_bytes()) {
    throw std::runtime_error("the child holds data past the end of its parent; convert it instead of committing");
  }
  std::vector<uint8_t> buf(kCopyChunk);
  bool ok = true;
  const bool walked = child->for_each_own_range([&](uint64_t offset, uint64_t len) {
    while (len) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(len, kCopyChunk));
      if (!child->read(offset, buf.data(), n) || !parent->write(offset, buf.data(), n)) return ok = false;
      offset += n;
      len -= n;
      done += n;
      if (progress) progress(done, total);
    }
    return true;
  });
  if (!walked || !ok || !parent->flush()) throw std::runtime_error("commit failed: I/O error");
  // Other snapshots of this parent were taken from its old contents.
  if (auto* vhd = dynamic_cast<VhdDisk*>(parent.get())) vhd->new_unique_id();
}

void merge_parent_into(const std::wstring& child_path, const Progress& progress) {
  auto child = open_vhd(child_path, true, /*open_parent=*/true);
  if (child->type() != VhdDisk::Type::Differencing) throw std::runtime_error(narrow(child_path) + " has no parent");
  auto parent = open_vhd(child->parent_path(), false, /*open_parent=*/false);
  if (parent->type() != VhdDisk::Type::Differencing) {
    throw std::runtime_error("the parent " + narrow(child->parent_path()) +
                             " is the base image; only a differencing parent can be merged into its child");
  }
  const std::wstring grandparent_path = parent->parent_path();
  uint64_t total = 0, done = 0;
  parent->for_each_own_range([&](uint64_t, uint64_t len) { return total += len, true; });
  std::vector<uint8_t> buf(kCopyChunk);
  bool ok = true;
  // Over every range the parent holds, read what the guest sees through the child (its own sectors,
  // else the parent's) and write it back into the child: its own sectors get the same bytes again, the
  // others become its own with the parent's contents. After that the parent is no longer needed.
  const bool walked = parent->for_each_own_range([&](uint64_t offset, uint64_t len) {
    while (len) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(len, kCopyChunk));
      if (!child->read(offset, buf.data(), n) || !child->write(offset, buf.data(), n)) return ok = false;
      offset += n;
      len -= n;
      done += n;
      if (progress) progress(done, total);
    }
    return true;
  });
  if (!walked || !ok || !child->flush()) throw std::runtime_error("merge failed: I/O error");
  auto grandparent = open_image(grandparent_path, /*read_only=*/true);
  child->set_parent(grandparent_path, *grandparent);
}

void convert_image(const std::wstring& src, const std::wstring& dst, ImageFormat format, const Progress& progress) {
  auto in = open_image(src, /*read_only=*/true);
  const uint64_t size = (in->size_bytes() + 511) / 512 * 512;
  create_image(dst, format, size);
  std::unique_ptr<Disk> out;
  try {
    out = open_image(dst, /*read_only=*/false);
    std::vector<uint8_t> buf(kCopyChunk);
    for (uint64_t offset = 0; offset < size;) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(kCopyChunk, size - offset));
      // The source may end mid-sector (raw files); the rest of the last sector reads as zeros.
      const size_t avail = static_cast<size_t>(std::min<uint64_t>(n, in->size_bytes() - std::min(offset, in->size_bytes())));
      std::memset(buf.data() + avail, 0, n - avail);
      if (avail && !in->read(offset, buf.data(), avail)) throw std::runtime_error("read failed at " + std::to_string(offset));
      if (!all_zero(buf.data(), n) && !out->write(offset, buf.data(), n)) {
        throw std::runtime_error("write failed at " + std::to_string(offset));
      }
      offset += n;
      if (progress) progress(offset, size);
    }
    if (!out->flush()) throw std::runtime_error("flush failed");
  } catch (...) {
    out.reset();
    DeleteFileW(dst.c_str());
    throw;
  }
}

uint64_t parse_size(const std::string& text) {
  if (text.empty()) return 0;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || value <= 0) return 0;
  std::string unit(end);
  for (auto& c : unit) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (!unit.empty() && unit.back() == 'B') unit.pop_back();
  if (unit.size() == 2 && unit[1] == 'I') unit.pop_back();  // GiB / MiB
  double mult;
  if (unit.empty() || unit == "M") mult = double(1 << 20);
  else if (unit == "K") mult = 1024.0;
  else if (unit == "G") mult = double(1u << 30);
  else if (unit == "T") mult = double(1ull << 40);
  else return 0;
  const uint64_t bytes = static_cast<uint64_t>(value * mult);
  return (bytes + 511) / 512 * 512;
}

}  // namespace sandvm::block
