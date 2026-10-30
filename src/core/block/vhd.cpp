#include "core/block/vhd.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>
#include <stdexcept>

#include "core/block/image.h"
#include "core/log.h"

namespace sandvm::block {
namespace {
constexpr uint32_t kSector = 512;
constexpr uint64_t kNoOffset = ~uint64_t{0};
// Seconds between 1970-01-01 and 2000-01-01, the VHD epoch.
constexpr int64_t kVhdEpoch = 946684800;

// Footer field offsets.
constexpr size_t kFtCookie = 0, kFtFeatures = 8, kFtVersion = 12, kFtDataOffset = 16, kFtTimestamp = 24,
                 kFtCreatorApp = 28, kFtCreatorVer = 32, kFtCreatorOs = 36, kFtOrigSize = 40, kFtCurSize = 48,
                 kFtGeometry = 56, kFtType = 60, kFtChecksum = 64, kFtUid = 68;
// Dynamic header field offsets.
constexpr size_t kHdCookie = 0, kHdDataOffset = 8, kHdTableOffset = 16, kHdVersion = 24, kHdMaxEntries = 28,
                 kHdBlockSize = 32, kHdChecksum = 36, kHdParentUid = 40, kHdParentTime = 56, kHdParentName = 64,
                 kHdLocators = 576;
constexpr size_t kLocatorSize = 24, kLocatorCount = 8;

uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t{be32(p)} << 32) | be32(p + 4); }
void put_be32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24);
  p[1] = static_cast<uint8_t>(v >> 16);
  p[2] = static_cast<uint8_t>(v >> 8);
  p[3] = static_cast<uint8_t>(v);
}
void put_be64(uint8_t* p, uint64_t v) {
  put_be32(p, static_cast<uint32_t>(v >> 32));
  put_be32(p + 4, static_cast<uint32_t>(v));
}
uint32_t fourcc(const char* s) { return be32(reinterpret_cast<const uint8_t*>(s)); }

// One's complement of the byte sum, the checksum field itself counted as zero.
uint32_t checksum(const uint8_t* p, size_t len, size_t field) {
  uint32_t sum = 0;
  for (size_t i = 0; i < len; ++i) {
    if (i < field || i >= field + 4) sum += p[i];
  }
  return ~sum;
}

bool valid_footer(const uint8_t* f) {
  return std::memcmp(f + kFtCookie, "conectix", 8) == 0 && be32(f + kFtChecksum) == checksum(f, 512, kFtChecksum);
}

uint32_t vhd_now() { return static_cast<uint32_t>(static_cast<int64_t>(std::time(nullptr)) - kVhdEpoch); }

uint32_t vhd_time_of(uint64_t filetime) {
  if (!filetime) return 0;
  // FILETIME counts 100 ns since 1601; 11644473600 s separate 1601 and 1970.
  const int64_t unix_seconds = static_cast<int64_t>(filetime / 10000000) - 11644473600LL;
  return static_cast<uint32_t>(unix_seconds - kVhdEpoch);
}

uint64_t file_last_write(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
  return (uint64_t{data.ftLastWriteTime.dwHighDateTime} << 32) | data.ftLastWriteTime.dwLowDateTime;
}

void random_bytes(uint8_t* out, size_t len) {
  std::random_device rd;
  for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(rd());
}

// CHS geometry from the specification's appendix (what Virtual PC / Windows report to the BIOS).
uint32_t chs_geometry(uint64_t size) {
  uint64_t total = size / kSector;
  total = std::min<uint64_t>(total, 65535ull * 16 * 255);
  uint64_t spt, heads, cth;
  if (total >= 65535ull * 16 * 63) {
    spt = 255;
    heads = 16;
    cth = total / spt;
  } else {
    spt = 17;
    cth = total / spt;
    heads = std::max<uint64_t>((cth + 1023) / 1024, 4);
    if (cth >= heads * 1024 || heads > 16) {
      spt = 31;
      heads = 16;
      cth = total / spt;
    }
    if (cth >= heads * 1024) {
      spt = 63;
      heads = 16;
      cth = total / spt;
    }
  }
  const uint64_t cyl = std::min<uint64_t>(cth / heads, 65535);
  return static_cast<uint32_t>((cyl << 16) | (heads << 8) | spt);
}

void build_footer(uint8_t* f, VhdDisk::Type type, uint64_t size, uint32_t geometry, uint64_t data_offset) {
  std::memset(f, 0, 512);
  std::memcpy(f + kFtCookie, "conectix", 8);
  put_be32(f + kFtFeatures, 2);  // "reserved, must always be set"
  put_be32(f + kFtVersion, 0x00010000);
  put_be64(f + kFtDataOffset, data_offset);
  put_be32(f + kFtTimestamp, vhd_now());
  put_be32(f + kFtCreatorApp, fourcc("svm "));
  put_be32(f + kFtCreatorVer, 0x00010000);
  put_be32(f + kFtCreatorOs, fourcc("Wi2k"));
  put_be64(f + kFtOrigSize, size);
  put_be64(f + kFtCurSize, size);
  put_be32(f + kFtGeometry, geometry);
  put_be32(f + kFtType, static_cast<uint32_t>(type));
  random_bytes(f + kFtUid, 16);
  put_be32(f + kFtChecksum, checksum(f, 512, kFtChecksum));
}

uint64_t round_up(uint64_t v, uint64_t to) { return (v + to - 1) / to * to; }

bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i]) return false;
  }
  return true;
}

bool bit(const std::vector<uint8_t>& bm, uint32_t sector) { return bm[sector >> 3] & (0x80 >> (sector & 7)); }

// Parent locator data as UTF-16LE (platform codes W2ru / W2ku) without trailing NULs.
std::wstring utf16le(const std::vector<uint8_t>& data) {
  std::wstring s;
  for (size_t i = 0; i + 1 < data.size(); i += 2) s.push_back(static_cast<wchar_t>(data[i] | (data[i + 1] << 8)));
  while (!s.empty() && s.back() == L'\0') s.pop_back();
  return s;
}

std::vector<uint8_t> to_utf16le(const std::wstring& s) {
  std::vector<uint8_t> out;
  for (wchar_t c : s) {
    out.push_back(static_cast<uint8_t>(c));
    out.push_back(static_cast<uint8_t>(c >> 8));
  }
  return out;
}

// ".\name" when the parent sits in the child's directory or below it, else empty.
std::wstring relative_parent(const std::wstring& child_abs, const std::wstring& parent_abs) {
  const std::wstring dir = directory_of(child_abs);
  if (dir.empty() || parent_abs.size() <= dir.size()) return {};
  if (_wcsnicmp(parent_abs.c_str(), dir.c_str(), dir.size()) != 0) return {};
  return L".\\" + parent_abs.substr(dir.size());
}

std::runtime_error vhd_error(const std::wstring& path, const std::string& what) {
  return std::runtime_error("VHD " + narrow(path) + ": " + what);
}
}  // namespace

bool VhdDisk::probe(const HostFile& file) {
  const uint64_t size = file.size();
  if (size < kSector) return false;
  uint8_t f[512];
  if (file.read(size - kSector, f, kSector) && std::memcmp(f, "conectix", 8) == 0) return true;
  // A dynamic image keeps a copy at the start; the one at the end may be gone after a crash.
  return file.read(0, f, kSector) && std::memcmp(f, "conectix", 8) == 0 && be32(f + kFtType) != 2;
}

std::unique_ptr<VhdDisk> VhdDisk::open(std::unique_ptr<HostFile> file, bool open_parent, int depth) {
  std::unique_ptr<VhdDisk> d(new VhdDisk(std::move(file)));
  const std::wstring& path = d->file_->path();
  const uint64_t fsize = d->file_->size();
  if (fsize < kSector) throw vhd_error(path, "file too small");

  bool footer_at_end = d->file_->read(fsize - kSector, d->footer_, kSector) && valid_footer(d->footer_);
  if (!footer_at_end) {
    if (!d->file_->read(0, d->footer_, kSector) || !valid_footer(d->footer_) || be32(d->footer_ + kFtType) == 2) {
      throw vhd_error(path, "no valid footer (corrupted image?)");
    }
    SVM_WARN("VHD %s: footer at the end is damaged, using the copy at the start", narrow(path).c_str());
  }
  const uint32_t type = be32(d->footer_ + kFtType);
  if (type != 2 && type != 3 && type != 4) throw vhd_error(path, "unsupported disk type " + std::to_string(type));
  d->type_ = static_cast<Type>(type);
  d->size_ = be64(d->footer_ + kFtCurSize);
  if (be32(d->footer_ + kFtVersion) >> 16 != 1) throw vhd_error(path, "unsupported format version");

  if (d->type_ == Type::Fixed) {
    if (d->size_ > fsize - kSector) throw vhd_error(path, "fixed image shorter than its size");
    return d;
  }

  d->header_offset_ = be64(d->footer_ + kFtDataOffset);
  if (d->header_offset_ == kNoOffset || d->header_offset_ + 1024 > fsize ||
      !d->file_->read(d->header_offset_, d->header_, 1024)) {
    throw vhd_error(path, "dynamic header out of range");
  }
  const uint8_t* h = d->header_;
  if (std::memcmp(h + kHdCookie, "cxsparse", 8) != 0 || be32(h + kHdChecksum) != checksum(h, 1024, kHdChecksum)) {
    throw vhd_error(path, "bad dynamic header");
  }
  d->block_size_ = be32(h + kHdBlockSize);
  if (d->block_size_ < 4096 || (d->block_size_ & (d->block_size_ - 1)) || d->block_size_ > (256u << 20)) {
    throw vhd_error(path, "unsupported block size");
  }
  d->bitmap_bytes_ = static_cast<uint32_t>(round_up(d->block_size_ / kSector / 8, kSector));
  const uint32_t entries = be32(h + kHdMaxEntries);
  if (uint64_t{entries} * d->block_size_ < d->size_) throw vhd_error(path, "BAT smaller than the disk");
  d->bat_offset_ = be64(h + kHdTableOffset);
  const uint64_t bat_bytes = uint64_t{entries} * 4;
  std::vector<uint8_t> raw(bat_bytes);
  if (d->bat_offset_ + bat_bytes > fsize || !d->file_->read(d->bat_offset_, raw.data(), raw.size())) {
    throw vhd_error(path, "BAT out of range");
  }
  // Only the entries that cover the disk matter (the table may be larger).
  const uint32_t used = static_cast<uint32_t>((d->size_ + d->block_size_ - 1) / d->block_size_);
  d->bat_.resize(used);
  uint64_t end = round_up(d->bat_offset_ + bat_bytes, kSector);
  const uint64_t block_total = uint64_t{d->bitmap_bytes_} + d->block_size_;
  for (uint32_t i = 0; i < used; ++i) {
    d->bat_[i] = be32(raw.data() + 4 * i);
    if (d->bat_[i] == kUnused) continue;
    const uint64_t start = uint64_t{d->bat_[i]} * kSector;
    // A block cut short by a crash still reads (zeros past the end are not ours to invent): reject it
    // instead, the image needs repair.
    if (start + block_total > fsize - (footer_at_end ? kSector : 0)) throw vhd_error(path, "block past the end");
    end = std::max(end, start + block_total);
  }
  if (d->type_ == Type::Differencing) d->bitmaps_.resize(used);
  d->next_free_ = footer_at_end ? std::max(end, fsize - kSector) : end;
  if (d->next_free_ % kSector) d->next_free_ = round_up(d->next_free_, kSector);
  if (!footer_at_end && d->file_->writable() && !d->write_footer(d->next_free_)) {
    throw vhd_error(path, "cannot repair the footer");
  }
  if (d->type_ == Type::Differencing) d->resolve_parent(h, open_parent, depth);
  return d;
}

void VhdDisk::resolve_parent(const uint8_t* h, bool open_parent, int depth) {
  const std::wstring& path = file_->path();
  const std::wstring dir = directory_of(absolute_path(path));
  std::vector<std::wstring> candidates;
  // W2ru (relative) first, so a folder moved as a whole keeps working; then W2ku (absolute).
  for (const char* code : {"W2ru", "W2ku"}) {
    for (size_t i = 0; i < kLocatorCount; ++i) {
      const uint8_t* loc = h + kHdLocators + i * kLocatorSize;
      if (be32(loc) != fourcc(code)) continue;
      const uint32_t len = be32(loc + 8);
      const uint64_t off = be64(loc + 16);
      if (len == 0 || len > 65536) continue;
      std::vector<uint8_t> data(len);
      if (!file_->read(off, data.data(), len)) continue;
      std::wstring p = utf16le(data);
      if (p.empty()) continue;
      if (!is_absolute_path(p)) {
        if (p.rfind(L".\\", 0) == 0 || p.rfind(L"./", 0) == 0) p = p.substr(2);
        p = dir + p;
      }
      candidates.push_back(p);
    }
  }
  // Last resort: the parent's file name (the Unicode name field, UTF-16BE) next to the child.
  std::wstring name;
  for (size_t i = 0; i < 256; ++i) {
    const wchar_t c = static_cast<wchar_t>((h[kHdParentName + 2 * i] << 8) | h[kHdParentName + 2 * i + 1]);
    if (!c) break;
    name.push_back(c);
  }
  if (!name.empty()) candidates.push_back(dir + file_name_of(name));

  for (const auto& c : candidates) {
    if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
      parent_path_ = absolute_path(c);
      break;
    }
  }
  if (parent_path_.empty()) {
    throw vhd_error(path, "parent image not found" + (candidates.empty() ? std::string() : ": " + narrow(candidates[0])));
  }
  if (!open_parent) return;
  parent_ = open_image(parent_path_, /*read_only=*/true, depth + 1);
  uint8_t want[16], have[16];
  std::memcpy(want, h + kHdParentUid, 16);
  if (parent_->unique_id(have)) {
    if (std::memcmp(want, have, 16) != 0) {
      throw vhd_error(path, "parent " + narrow(parent_path_) + " has changed since this image was created");
    }
  } else if (!all_zero(want, 16)) {
    throw vhd_error(path, "parent " + narrow(parent_path_) + " is not the VHD this image was created from");
  } else {
    // A raw or VMDK parent (a SandVM extension: Windows only accepts VHD parents) has no identity
    // field, so its modification time stands in for one.
    const uint32_t recorded = be32(h + kHdParentTime);
    const uint32_t current = vhd_time_of(file_last_write(parent_path_));
    if (recorded && current && recorded != current) {
      throw vhd_error(path, "parent " + narrow(parent_path_) +
                                " was modified after this snapshot was taken (the snapshot is no longer valid)");
    }
  }
  if (parent_->size_bytes() > size_) SVM_WARN("VHD %s: parent is larger than the child", narrow(path).c_str());
}

bool VhdDisk::unique_id(uint8_t out[16]) const {
  std::memcpy(out, footer_ + kFtUid, 16);
  return true;
}

uint32_t VhdDisk::allocated_blocks() const {
  return static_cast<uint32_t>(std::count_if(bat_.begin(), bat_.end(), [](uint32_t e) { return e != kUnused; }));
}

bool VhdDisk::read(uint64_t offset, void* data, size_t len) {
  std::lock_guard lock(mutex_);
  if (offset > size_ || len > size_ - offset) return false;
  if (type_ == Type::Fixed) return file_->read(offset, data, len);
  auto* p = static_cast<uint8_t*>(data);
  while (len) {
    const uint32_t block = static_cast<uint32_t>(offset / block_size_);
    const uint32_t in = static_cast<uint32_t>(offset % block_size_);
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(len, block_size_ - in));
    if (!read_block(block, in, p, n)) return false;
    p += n;
    offset += n;
    len -= n;
  }
  return true;
}

bool VhdDisk::read_parent(uint64_t offset, uint8_t* p, size_t n) {
  const uint64_t psize = parent_ ? parent_->size_bytes() : 0;
  const size_t from_parent = offset >= psize ? 0 : static_cast<size_t>(std::min<uint64_t>(n, psize - offset));
  if (from_parent && !parent_->read(offset, p, from_parent)) return false;
  std::memset(p + from_parent, 0, n - from_parent);
  return true;
}

const std::vector<uint8_t>* VhdDisk::bitmap(uint32_t block) {
  auto& bm = bitmaps_[block];
  if (bm.empty()) {
    bm.resize(bitmap_bytes_);
    if (!file_->read(block_offset(block), bm.data(), bm.size())) {
      bm.clear();
      return nullptr;
    }
  }
  return &bm;
}

bool VhdDisk::read_block(uint32_t block, uint32_t in, uint8_t* p, uint32_t n) {
  const uint64_t disk_base = uint64_t{block} * block_size_;
  if (bat_[block] == kUnused) {
    if (type_ == Type::Differencing) return read_parent(disk_base + in, p, n);
    std::memset(p, 0, n);
    return true;
  }
  const uint64_t data = block_offset(block) + bitmap_bytes_;
  if (type_ != Type::Differencing) return file_->read(data + in, p, n);
  const auto* bm = bitmap(block);
  if (!bm) return false;
  // Runs of sectors that come from the same place.
  const uint32_t end = in + n;
  uint32_t pos = in;
  while (pos < end) {
    const bool own = bit(*bm, pos / kSector);
    uint32_t run_end = (pos / kSector + 1) * kSector;
    while (run_end < end && bit(*bm, run_end / kSector) == own) run_end += kSector;
    run_end = std::min(run_end, end);
    const uint32_t chunk = run_end - pos;
    if (own ? !file_->read(data + pos, p, chunk) : !read_parent(disk_base + pos, p, chunk)) return false;
    p += chunk;
    pos = run_end;
  }
  return true;
}

bool VhdDisk::write(uint64_t offset, const void* data, size_t len) {
  std::lock_guard lock(mutex_);
  if (!file_->writable() || offset > size_ || len > size_ - offset) return false;
  if (type_ == Type::Fixed) return file_->write(offset, data, len);
  auto* p = static_cast<const uint8_t*>(data);
  while (len) {
    const uint32_t block = static_cast<uint32_t>(offset / block_size_);
    const uint32_t in = static_cast<uint32_t>(offset % block_size_);
    const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(len, block_size_ - in));
    if (!write_block(block, in, p, n)) return false;
    p += n;
    offset += n;
    len -= n;
  }
  return true;
}

bool VhdDisk::write_block(uint32_t block, uint32_t in, const uint8_t* p, uint32_t n) {
  // Writing zeros into a block that does not exist yet changes nothing a reader sees on a dynamic image.
  if (bat_[block] == kUnused && type_ == Type::Dynamic && all_zero(p, n)) return true;
  if (bat_[block] == kUnused && !allocate(block)) return false;
  const uint64_t data = block_offset(block) + bitmap_bytes_;
  if (type_ != Type::Differencing) return file_->write(data + in, p, n);

  // Sectors not present yet that this write covers only partly take the rest from the parent first.
  const uint32_t first = in / kSector, last = (in + n - 1) / kSector;
  const auto* bm = bitmap(block);
  if (!bm) return false;
  const bool head_partial = (in % kSector) && !bit(*bm, first);
  const bool tail_partial = ((in + n) % kSector) && !bit(*bm, last);
  if (head_partial || tail_partial) {
    const uint32_t a0 = first * kSector, a1 = (last + 1) * kSector;
    std::vector<uint8_t> buf(a1 - a0);
    if (!read_block(block, a0, buf.data(), a1 - a0)) return false;
    std::memcpy(buf.data() + (in - a0), p, n);
    if (!file_->write(data + a0, buf.data(), buf.size())) return false;
  } else if (!file_->write(data + in, p, n)) {
    return false;
  }
  // Data before bitmap: a crash in between leaves the old contents visible, never garbage.
  auto& bmw = bitmaps_[block];
  bool changed = false;
  for (uint32_t s = first; s <= last; ++s) {
    const uint8_t mask = static_cast<uint8_t>(0x80 >> (s & 7));
    if (!(bmw[s >> 3] & mask)) {
      bmw[s >> 3] |= mask;
      changed = true;
    }
  }
  if (!changed) return true;
  const uint32_t b0 = first / 8 / kSector * kSector, b1 = static_cast<uint32_t>(round_up(last / 8 + 1, kSector));
  return file_->write(block_offset(block) + b0, bmw.data() + b0, b1 - b0);
}

bool VhdDisk::allocate(uint32_t block) {
  const uint64_t pos = next_free_;
  if (pos / kSector >= kUnused) return false;  // the BAT holds 32-bit sector numbers
  const uint64_t total = uint64_t{bitmap_bytes_} + block_size_;
  std::vector<uint8_t> buf(total, 0);
  // Dynamic images: every sector of an allocated block is present (zeros until written).
  if (type_ == Type::Dynamic) std::memset(buf.data(), 0xFF, bitmap_bytes_);
  if (!file_->write(pos, buf.data(), buf.size())) return false;
  if (!write_footer(pos + total)) return false;
  // The BAT entry goes last, and only once the block is on disk: an entry that reaches the disk before
  // the block it points at would leave an image that no longer opens after a host crash.
  if (!file_->flush()) return false;
  uint8_t entry[4];
  put_be32(entry, static_cast<uint32_t>(pos / kSector));
  if (!file_->write(bat_offset_ + uint64_t{block} * 4, entry, 4)) return false;
  bat_[block] = static_cast<uint32_t>(pos / kSector);
  next_free_ = pos + total;
  if (type_ == Type::Differencing) bitmaps_[block].assign(bitmap_bytes_, 0);
  return true;
}

bool VhdDisk::write_footer(uint64_t at) { return file_->write(at, footer_, kSector); }

bool VhdDisk::write_header() {
  put_be32(header_ + kHdChecksum, checksum(header_, 1024, kHdChecksum));
  return file_->write(header_offset_, header_, 1024);
}

bool VhdDisk::for_each_own_range(const std::function<bool(uint64_t, uint64_t)>& fn) {
  // Collected under the lock, reported without it: callers read this image from fn.
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  {
    std::lock_guard lock(mutex_);
    if (type_ == Type::Fixed) {
      ranges.emplace_back(0, size_);
    }
    for (uint32_t b = 0; type_ != Type::Fixed && b < bat_.size(); ++b) {
      if (bat_[b] == kUnused) continue;
      const uint64_t base = uint64_t{b} * block_size_;
      const uint32_t limit = static_cast<uint32_t>(std::min<uint64_t>(block_size_, size_ - base));
      if (type_ == Type::Dynamic) {
        ranges.emplace_back(base, limit);
        continue;
      }
      const auto* bm = bitmap(b);
      if (!bm) return false;
      const uint32_t sectors = (limit + kSector - 1) / kSector;
      for (uint32_t s = 0; s < sectors;) {
        if (!bit(*bm, s)) {
          ++s;
          continue;
        }
        uint32_t e = s;
        while (e < sectors && bit(*bm, e)) ++e;
        const uint64_t start = base + uint64_t{s} * kSector;
        ranges.emplace_back(start, std::min<uint64_t>(uint64_t{e - s} * kSector, size_ - start));
        s = e;
      }
    }
  }
  for (const auto& [offset, length] : ranges) {
    if (!fn(offset, length)) break;
  }
  return true;
}

void VhdDisk::new_unique_id() {
  std::lock_guard lock(mutex_);
  random_bytes(footer_ + kFtUid, 16);
  put_be32(footer_ + kFtChecksum, checksum(footer_, 512, kFtChecksum));
  const uint64_t at = type_ == Type::Fixed ? size_ : next_free_;
  if (!write_footer(at) || (type_ != Type::Fixed && !file_->write(0, footer_, kSector)) || !file_->flush()) {
    throw vhd_error(file_->path(), "write failed");
  }
}

void VhdDisk::set_parent(const std::wstring& parent_path, const Disk& parent) {
  std::lock_guard lock(mutex_);
  if (type_ != Type::Differencing) throw vhd_error(file_->path(), "not a differencing image");
  const std::wstring child_abs = absolute_path(file_->path());
  const std::wstring parent_abs = absolute_path(parent_path);
  uint8_t* h = header_;
  std::memset(h + kHdParentUid, 0, 16);
  parent.unique_id(h + kHdParentUid);
  put_be32(h + kHdParentTime, vhd_time_of(file_last_write(parent_abs)));
  std::memset(h + kHdParentName, 0, 512);
  for (size_t i = 0; i < parent_abs.size() && i < 255; ++i) {
    h[kHdParentName + 2 * i] = static_cast<uint8_t>(parent_abs[i] >> 8);
    h[kHdParentName + 2 * i + 1] = static_cast<uint8_t>(parent_abs[i]);
  }
  // New locator data goes where the footer is now (the old data becomes unused space), then the footer
  // moves behind it. Header last, so a crash leaves the old parent in place.
  std::memset(h + kHdLocators, 0, kLocatorSize * kLocatorCount);
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> locators;
  const std::wstring rel = relative_parent(child_abs, parent_abs);
  if (!rel.empty()) locators.emplace_back(fourcc("W2ru"), to_utf16le(rel));
  locators.emplace_back(fourcc("W2ku"), to_utf16le(parent_abs));
  uint64_t pos = next_free_;
  for (size_t i = 0; i < locators.size(); ++i) {
    const auto& [code, bytes] = locators[i];
    const uint64_t space = round_up(bytes.size(), kSector);
    std::vector<uint8_t> padded(space, 0);
    std::memcpy(padded.data(), bytes.data(), bytes.size());
    if (!file_->write(pos, padded.data(), padded.size())) throw vhd_error(file_->path(), "write failed");
    uint8_t* loc = h + kHdLocators + i * kLocatorSize;
    put_be32(loc, code);
    put_be32(loc + 4, static_cast<uint32_t>(space));  // what Windows writes: bytes, despite the spec
    put_be32(loc + 8, static_cast<uint32_t>(bytes.size()));
    put_be64(loc + 16, pos);
    pos += space;
  }
  if (!write_footer(pos)) throw vhd_error(file_->path(), "write failed");
  next_free_ = pos;
  if (!write_header() || !file_->flush()) throw vhd_error(file_->path(), "write failed");
  // Header copy of the footer at offset 0 does not change.
  parent_path_ = parent_abs;
}

void VhdDisk::create_dynamic(const std::wstring& path, uint64_t size_bytes) {
  const uint64_t size = round_up(size_bytes, kSector);
  if (size == 0 || size > kMaxSize) throw vhd_error(path, "size must be 1 byte .. 2040 GiB");
  const uint32_t entries = static_cast<uint32_t>((size + kDefaultBlockSize - 1) / kDefaultBlockSize);
  const uint64_t bat_bytes = round_up(uint64_t{entries} * 4, kSector);
  constexpr uint64_t kHeaderAt = 512, kBatAt = 1536;

  uint8_t footer[512];
  build_footer(footer, Type::Dynamic, size, chs_geometry(size), kHeaderAt);
  uint8_t h[1024] = {};
  std::memcpy(h + kHdCookie, "cxsparse", 8);
  put_be64(h + kHdDataOffset, kNoOffset);
  put_be64(h + kHdTableOffset, kBatAt);
  put_be32(h + kHdVersion, 0x00010000);
  put_be32(h + kHdMaxEntries, entries);
  put_be32(h + kHdBlockSize, kDefaultBlockSize);
  put_be32(h + kHdChecksum, checksum(h, 1024, kHdChecksum));
  std::vector<uint8_t> bat(bat_bytes, 0xFF);

  auto file = HostFile::create(path);
  const bool ok = file->write(0, footer, 512) && file->write(kHeaderAt, h, 1024) &&
                  file->write(kBatAt, bat.data(), bat.size()) && file->write(kBatAt + bat_bytes, footer, 512) &&
                  file->flush();
  file.reset();
  if (!ok) {
    DeleteFileW(path.c_str());
    throw vhd_error(path, "write failed");
  }
}

void VhdDisk::create_differencing(const std::wstring& path, const std::wstring& parent_path) {
  const std::wstring parent_abs = absolute_path(parent_path);
  const std::wstring child_abs = absolute_path(path);
  if (_wcsicmp(parent_abs.c_str(), child_abs.c_str()) == 0) throw vhd_error(path, "an image cannot be its own parent");
  uint64_t size;
  uint32_t geometry;
  uint8_t parent_uid[16] = {};
  {
    auto parent = open_image(parent_abs, /*read_only=*/true);
    size = round_up(parent->size_bytes(), kSector);
    if (size == 0 || size > kMaxSize) throw vhd_error(path, "parent size not supported by VHD");
    geometry = chs_geometry(size);
    parent->unique_id(parent_uid);
    if (auto* vhd = dynamic_cast<VhdDisk*>(parent.get())) geometry = be32(vhd->footer_ + kFtGeometry);
  }
  const uint64_t parent_time = file_last_write(parent_abs);

  const uint32_t entries = static_cast<uint32_t>((size + kDefaultBlockSize - 1) / kDefaultBlockSize);
  const uint64_t bat_bytes = round_up(uint64_t{entries} * 4, kSector);
  constexpr uint64_t kHeaderAt = 512, kBatAt = 1536;

  uint8_t footer[512];
  build_footer(footer, Type::Differencing, size, geometry, kHeaderAt);
  uint8_t h[1024] = {};
  std::memcpy(h + kHdCookie, "cxsparse", 8);
  put_be64(h + kHdDataOffset, kNoOffset);
  put_be64(h + kHdTableOffset, kBatAt);
  put_be32(h + kHdVersion, 0x00010000);
  put_be32(h + kHdMaxEntries, entries);
  put_be32(h + kHdBlockSize, kDefaultBlockSize);
  std::memcpy(h + kHdParentUid, parent_uid, 16);
  put_be32(h + kHdParentTime, vhd_time_of(parent_time));
  for (size_t i = 0; i < parent_abs.size() && i < 255; ++i) {
    h[kHdParentName + 2 * i] = static_cast<uint8_t>(parent_abs[i] >> 8);
    h[kHdParentName + 2 * i + 1] = static_cast<uint8_t>(parent_abs[i]);
  }
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> locators;
  const std::wstring rel = relative_parent(child_abs, parent_abs);
  if (!rel.empty()) locators.emplace_back(fourcc("W2ru"), to_utf16le(rel));
  locators.emplace_back(fourcc("W2ku"), to_utf16le(parent_abs));

  std::vector<uint8_t> image(kBatAt + bat_bytes, 0);
  std::memset(image.data() + kBatAt, 0xFF, bat_bytes);
  for (size_t i = 0; i < locators.size(); ++i) {
    const auto& [code, bytes] = locators[i];
    const uint64_t space = round_up(bytes.size(), kSector);
    const uint64_t at = image.size();
    image.resize(at + space, 0);
    std::memcpy(image.data() + at, bytes.data(), bytes.size());
    uint8_t* loc = h + kHdLocators + i * kLocatorSize;
    put_be32(loc, code);
    put_be32(loc + 4, static_cast<uint32_t>(space));
    put_be32(loc + 8, static_cast<uint32_t>(bytes.size()));
    put_be64(loc + 16, at);
  }
  put_be32(h + kHdChecksum, checksum(h, 1024, kHdChecksum));
  std::memcpy(image.data(), footer, 512);
  std::memcpy(image.data() + kHeaderAt, h, 1024);
  image.insert(image.end(), footer, footer + 512);

  auto file = HostFile::create(path);
  const bool ok = file->write(0, image.data(), image.size()) && file->flush();
  file.reset();
  if (!ok) {
    DeleteFileW(path.c_str());
    throw vhd_error(path, "write failed");
  }
}

}  // namespace sandvm::block
