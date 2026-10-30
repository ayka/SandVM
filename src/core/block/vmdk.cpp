#include "core/block/vmdk.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <stdexcept>

#include "core/block/image.h"
#include "core/log.h"

namespace sandvm::block {
namespace {
constexpr uint32_t kMagic = 0x564D444B;  // "KDMV" as a little-endian u32
constexpr uint32_t kSector = 512;
constexpr uint32_t kFlagRedundantGt = 1u << 1;
constexpr uint32_t kFlagZeroedGte = 1u << 2;
constexpr uint32_t kFlagCompressed = 1u << 16;
constexpr size_t kUncleanShutdown = 72;
constexpr uint32_t kNoParent = 0xFFFFFFFF;

uint32_t le32(const uint8_t* p) { return p[0] | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24); }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t{le32(p + 4)} << 32); }
void put_le32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put_le64(uint8_t* p, uint64_t v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

std::runtime_error vmdk_error(const std::wstring& path, const std::string& what) {
  return std::runtime_error("VMDK " + narrow(path) + ": " + what);
}

std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::string unquote(std::string s) {
  s = trim(s);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

uint32_t random_cid() {
  std::random_device rd;
  uint32_t v;
  do v = rd(); while (v == 0 || v == kNoParent);
  return v;
}

struct Header {
  uint32_t version = 0, flags = 0, gtes = 0;
  uint64_t capacity = 0, grain = 0, desc_offset = 0, desc_size = 0, rgd_offset = 0, gd_offset = 0, overhead = 0;
  uint16_t compress = 0;
};

bool parse_header(const uint8_t* h, Header* out) {
  if (le32(h) != kMagic) return false;
  out->version = le32(h + 4);
  out->flags = le32(h + 8);
  out->capacity = le64(h + 12);
  out->grain = le64(h + 20);
  out->desc_offset = le64(h + 28);
  out->desc_size = le64(h + 36);
  out->gtes = le32(h + 44);
  out->rgd_offset = le64(h + 48);
  out->gd_offset = le64(h + 56);
  out->overhead = le64(h + 64);
  out->compress = static_cast<uint16_t>(h[77] | (h[78] << 8));
  return true;
}
}  // namespace

struct VmdkDisk::Extent {
  enum class Kind { Sparse, Flat, Zero } kind = Kind::Flat;
  uint64_t start = 0;    // first disk sector
  uint64_t sectors = 0;  // length in sectors
  bool writable = false;
  std::unique_ptr<HostFile> owned;
  HostFile* file = nullptr;
  uint64_t flat_offset = 0;  // FLAT: sectors into the file
  // SPARSE
  bool sparse_header_dirty = false;  // uncleanShutdown set by us
  uint64_t grain = 128;
  uint32_t gtes = 512;
  bool zeroed_gte = false;
  uint64_t gd_offset = 0, rgd_offset = 0;  // sectors; rgd_offset 0 = none
  std::vector<uint32_t> gd, rgd;
  std::vector<std::vector<uint32_t>> gts;  // cache, loaded on demand
  uint64_t next_free = 0;                  // sectors
};

bool VmdkDisk::probe(const HostFile& file) {
  uint8_t head[64] = {};
  if (!file.read(0, head, std::min<uint64_t>(sizeof(head), file.size()))) return false;
  if (le32(head) == kMagic) return true;
  return std::memcmp(head, "# Disk DescriptorFile", 21) == 0;
}

std::unique_ptr<VmdkDisk> VmdkDisk::open(std::unique_ptr<HostFile> file, int depth) {
  std::unique_ptr<VmdkDisk> d(new VmdkDisk());
  d->read_only_ = !file->writable();
  uint8_t h[512] = {};
  Header hdr;
  std::string text;
  if (file->size() >= kSector && file->read(0, h, kSector) && parse_header(h, &hdr)) {
    if (hdr.desc_offset == 0 || hdr.desc_size == 0) {
      throw vmdk_error(file->path(), "an extent file without a descriptor; open the descriptor .vmdk instead");
    }
    text.resize(hdr.desc_size * kSector);
    if (!file->read(hdr.desc_offset * kSector, text.data(), text.size())) throw vmdk_error(file->path(), "read failed");
    text.resize(std::strlen(text.c_str()));
    d->descriptor_offset_ = hdr.desc_offset * kSector;
    d->descriptor_space_ = hdr.desc_size * kSector;
  } else {
    const uint64_t size = file->size();
    if (size > (1u << 20)) throw vmdk_error(file->path(), "descriptor too large");
    text.resize(size);
    if (!file->read(0, text.data(), text.size())) throw vmdk_error(file->path(), "read failed");
  }
  d->descriptor_ = text;
  d->parse_descriptor(text, std::move(file), depth);
  return d;
}

void VmdkDisk::parse_descriptor(const std::string& text, std::unique_ptr<HostFile> self, int depth) {
  const std::wstring self_path = self->path();
  const std::wstring dir = directory_of(absolute_path(self_path));
  uint32_t parent_cid = kNoParent;
  std::string parent_hint;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const size_t eq = line.find('=');
    std::string key = eq == std::string::npos ? "" : trim(line.substr(0, eq));
    if (key == "CID") {
      cid_ = static_cast<uint32_t>(std::strtoul(unquote(line.substr(eq + 1)).c_str(), nullptr, 16));
    } else if (key == "parentCID") {
      parent_cid = static_cast<uint32_t>(std::strtoul(unquote(line.substr(eq + 1)).c_str(), nullptr, 16));
    } else if (key == "createType") {
      create_type_ = unquote(line.substr(eq + 1));
    } else if (key == "parentFileNameHint") {
      parent_hint = unquote(line.substr(eq + 1));
    } else if (line.rfind("RW ", 0) == 0 || line.rfind("RDONLY ", 0) == 0 || line.rfind("NOACCESS ", 0) == 0) {
      // <access> <sectors> <type> ["file"] [offset]
      std::istringstream f(line);
      std::string access, type;
      uint64_t sectors = 0;
      f >> access >> sectors >> type;
      std::string rest;
      std::getline(f, rest);
      rest = trim(rest);
      std::string name;
      uint64_t offset = 0;
      if (!rest.empty() && rest[0] == '"') {
        const size_t close = rest.find('"', 1);
        if (close == std::string::npos) throw vmdk_error(self_path, "bad extent line: " + line);
        name = rest.substr(1, close - 1);
        offset = std::strtoull(trim(rest.substr(close + 1)).c_str(), nullptr, 10);
      }
      auto e = std::make_unique<Extent>();
      e->start = sectors_;
      e->sectors = sectors;
      e->writable = access == "RW" && !read_only_;
      if (type == "ZERO") {
        e->kind = Extent::Kind::Zero;
      } else if (type == "FLAT" || type == "SPARSE") {
        e->kind = type == "FLAT" ? Extent::Kind::Flat : Extent::Kind::Sparse;
        e->flat_offset = offset;
        const std::wstring wname = widen_utf8(name);
        if (self && _wcsicmp(file_name_of(wname).c_str(), file_name_of(self_path).c_str()) == 0) {
          e->owned = std::move(self);
        } else {
          e->owned = HostFile::open(is_absolute_path(wname) ? wname : dir + wname, e->writable);
        }
        e->file = e->owned.get();
      } else {
        throw vmdk_error(self_path, "extent type " + type + " is not supported (ESXi disk?)");
      }
      sectors_ += sectors;
      extents_.push_back(std::move(e));
    }
  }
  if (extents_.empty()) throw vmdk_error(self_path, "no extents in the descriptor");
  if (create_type_ == "streamOptimized") {
    throw vmdk_error(self_path, "streamOptimized (compressed, OVA export) disks are not supported; convert it first");
  }
  // The descriptor itself: embedded in the first extent, or this file when it is plain text.
  if (descriptor_space_) {
    descriptor_file_ = extents_[0]->file;
  } else if (self) {
    descriptor_owner_ = std::move(self);
    descriptor_file_ = descriptor_owner_.get();
  } else {
    for (auto& e : extents_) {
      if (e->file && _wcsicmp(e->file->path().c_str(), self_path.c_str()) == 0) descriptor_file_ = e->file;
    }
  }

  for (auto& e : extents_) {
    if (e->kind != Extent::Kind::Sparse) continue;
    uint8_t h[512];
    Header hdr;
    if (!e->file->read(0, h, kSector) || !parse_header(h, &hdr)) throw vmdk_error(e->file->path(), "bad sparse header");
    if ((hdr.flags & kFlagCompressed) || hdr.compress) throw vmdk_error(e->file->path(), "compressed grains are not supported");
    if (hdr.grain == 0 || hdr.grain > 2048 || hdr.gtes == 0 || hdr.gtes > 65536 || hdr.gd_offset == ~uint64_t{0}) {
      throw vmdk_error(e->file->path(), "unsupported sparse layout");
    }
    e->grain = hdr.grain;
    e->gtes = hdr.gtes;
    e->zeroed_gte = hdr.flags & kFlagZeroedGte;
    e->gd_offset = hdr.gd_offset;
    e->rgd_offset = (hdr.flags & kFlagRedundantGt) ? hdr.rgd_offset : 0;
    const uint64_t grains = (hdr.capacity + hdr.grain - 1) / hdr.grain;
    const uint64_t tables = (grains + hdr.gtes - 1) / hdr.gtes;
    auto load_dir = [&](uint64_t at, std::vector<uint32_t>& out) {
      std::vector<uint8_t> raw(tables * 4);
      if (!e->file->read(at * kSector, raw.data(), raw.size())) throw vmdk_error(e->file->path(), "grain directory out of range");
      out.resize(tables);
      for (uint64_t i = 0; i < tables; ++i) out[i] = le32(raw.data() + 4 * i);
    };
    load_dir(e->gd_offset, e->gd);
    if (e->rgd_offset) load_dir(e->rgd_offset, e->rgd);
    e->gts.resize(tables);
    e->next_free = (e->file->size() + kSector - 1) / kSector;
    if (h[kUncleanShutdown] && e->rgd_offset) repair_tables(*e, e->writable);
    if (e->writable) {
      const uint8_t dirty = 1;
      if (!e->file->write(kUncleanShutdown, &dirty, 1) || !e->file->flush()) throw vmdk_error(e->file->path(), "write failed");
      e->sparse_header_dirty = true;
    }
  }

  if (parent_cid != kNoParent) {
    if (parent_hint.empty()) throw vmdk_error(self_path, "snapshot without parentFileNameHint");
    std::wstring p = widen_utf8(parent_hint);
    if (!is_absolute_path(p)) p = dir + p;
    parent_path_ = absolute_path(p);
    parent_ = open_image(parent_path_, /*read_only=*/true, depth + 1);
    auto* vparent = dynamic_cast<VmdkDisk*>(parent_.get());
    if (!vparent) throw vmdk_error(self_path, "parent " + narrow(parent_path_) + " is not a VMDK");
    if (vparent->cid() != parent_cid) {
      throw vmdk_error(self_path, "parent " + narrow(parent_path_) + " was modified after this snapshot was taken");
    }
    if (vparent->size_bytes() != size_bytes()) throw vmdk_error(self_path, "parent capacity differs");
  }
}

VmdkDisk::~VmdkDisk() {
  for (auto& e : extents_) {
    if (!e->sparse_header_dirty) continue;
    const uint8_t clean = 0;
    e->file->write(kUncleanShutdown, &clean, 1);
    e->file->flush();
  }
}

bool VmdkDisk::read_parent(uint64_t sector, uint8_t* p, uint64_t count) {
  const uint64_t psize = parent_ ? parent_->size_bytes() / kSector : 0;
  const uint64_t from_parent = sector >= psize ? 0 : std::min(count, psize - sector);
  if (from_parent && !parent_->read(sector * kSector, p, from_parent * kSector)) return false;
  std::memset(p + from_parent * kSector, 0, (count - from_parent) * kSector);
  return true;
}

std::vector<uint32_t>* VmdkDisk::grain_table(Extent& e, uint32_t index, bool allocate) {
  if (index >= e.gd.size()) return nullptr;
  const uint64_t gt_bytes = uint64_t{e.gtes} * 4;
  const uint64_t gt_sectors = (gt_bytes + kSector - 1) / kSector;
  if (e.gd[index] == 0) {
    if (!allocate) return nullptr;
    // Fresh zeroed tables at the end of the extent, then the directory entries pointing at them.
    std::vector<uint8_t> zeros(gt_sectors * kSector, 0);
    const uint64_t primary = e.next_free;
    const uint64_t redundant = e.rgd_offset ? primary + gt_sectors : 0;
    if (!e.file->write(primary * kSector, zeros.data(), zeros.size())) return nullptr;
    if (redundant && !e.file->write(redundant * kSector, zeros.data(), zeros.size())) return nullptr;
    e.next_free += redundant ? 2 * gt_sectors : gt_sectors;
    uint8_t entry[4];
    put_le32(entry, static_cast<uint32_t>(primary));
    if (!e.file->write(e.gd_offset * kSector + uint64_t{index} * 4, entry, 4)) return nullptr;
    if (redundant) {
      put_le32(entry, static_cast<uint32_t>(redundant));
      if (!e.file->write(e.rgd_offset * kSector + uint64_t{index} * 4, entry, 4)) return nullptr;
      e.rgd[index] = static_cast<uint32_t>(redundant);
    }
    e.gd[index] = static_cast<uint32_t>(primary);
    e.gts[index].assign(e.gtes, 0);
  }
  auto& gt = e.gts[index];
  if (gt.empty()) {
    std::vector<uint8_t> raw(gt_bytes);
    if (!e.file->read(uint64_t{e.gd[index]} * kSector, raw.data(), raw.size())) return nullptr;
    gt.resize(e.gtes);
    for (uint32_t i = 0; i < e.gtes; ++i) gt[i] = le32(raw.data() + 4 * i);
  }
  return &gt;
}

bool VmdkDisk::read_extent(Extent& e, uint64_t sector, uint8_t* p, uint64_t count) {
  switch (e.kind) {
    case Extent::Kind::Zero:
      std::memset(p, 0, count * kSector);
      return true;
    case Extent::Kind::Flat:
      return e.file->read((e.flat_offset + sector) * kSector, p, count * kSector);
    case Extent::Kind::Sparse:
      break;
  }
  while (count) {
    const uint64_t g = sector / e.grain;
    const uint64_t in = sector % e.grain;
    const uint64_t n = std::min(count, e.grain - in);
    const auto* gt = grain_table(e, static_cast<uint32_t>(g / e.gtes), false);
    const uint32_t entry = gt ? (*gt)[g % e.gtes] : 0;
    bool ok;
    if (entry == 0) ok = read_parent(e.start + sector, p, n);
    else if (entry == 1 && e.zeroed_gte) ok = (std::memset(p, 0, n * kSector), true);
    else ok = e.file->read((uint64_t{entry} + in) * kSector, p, n * kSector);
    if (!ok) return false;
    p += n * kSector;
    sector += n;
    count -= n;
  }
  return true;
}

bool VmdkDisk::write_extent(Extent& e, uint64_t sector, const uint8_t* p, uint64_t count) {
  if (!e.writable) return false;
  switch (e.kind) {
    case Extent::Kind::Zero:
      return false;
    case Extent::Kind::Flat:
      return e.file->write((e.flat_offset + sector) * kSector, p, count * kSector);
    case Extent::Kind::Sparse:
      break;
  }
  while (count) {
    const uint64_t g = sector / e.grain;
    const uint64_t in = sector % e.grain;
    const uint64_t n = std::min(count, e.grain - in);
    auto* gt = grain_table(e, static_cast<uint32_t>(g / e.gtes), true);
    if (!gt) return false;
    uint32_t& entry = (*gt)[g % e.gtes];
    if (entry == 0 || (entry == 1 && e.zeroed_gte)) {
      // New grain at the end of the file, holding what the guest saw before (parent or zeros) around
      // the part being written; then the table entries, primary and redundant.
      std::vector<uint8_t> grain(e.grain * kSector, 0);
      if (entry == 0 && !read_parent(e.start + g * e.grain, grain.data(), e.grain)) return false;
      std::memcpy(grain.data() + in * kSector, p, n * kSector);
      const uint64_t at = e.next_free;
      if (at > 0xFFFFFFFFull - e.grain) return false;
      if (!e.file->write(at * kSector, grain.data(), grain.size())) return false;
      e.next_free += e.grain;
      uint8_t raw[4];
      put_le32(raw, static_cast<uint32_t>(at));
      const uint32_t t = static_cast<uint32_t>(g / e.gtes);
      const uint64_t slot = (g % e.gtes) * 4;
      if (!e.file->write(uint64_t{e.gd[t]} * kSector + slot, raw, 4)) return false;
      if (e.rgd_offset && e.rgd[t] && !e.file->write(uint64_t{e.rgd[t]} * kSector + slot, raw, 4)) return false;
      entry = static_cast<uint32_t>(at);
    } else if (!e.file->write((uint64_t{entry} + in) * kSector, p, n * kSector)) {
      return false;
    }
    p += n * kSector;
    sector += n;
    count -= n;
  }
  return true;
}

bool VmdkDisk::read(uint64_t offset, void* data, size_t len) {
  std::lock_guard lock(mutex_);
  if (offset > size_bytes() || len > size_bytes() - offset) return false;
  // Byte-granular callers (none of the disk models today) go through a sector-aligned bounce buffer.
  if ((offset | len) % kSector) {
    const uint64_t a0 = offset / kSector * kSector;
    const uint64_t a1 = (offset + len + kSector - 1) / kSector * kSector;
    std::vector<uint8_t> buf(a1 - a0);
    uint64_t s = a0 / kSector, left = (a1 - a0) / kSector;
    uint8_t* q = buf.data();
    for (auto& e : extents_) {
      if (!left) break;
      if (s >= e->start + e->sectors) continue;
      const uint64_t n = std::min(left, e->start + e->sectors - s);
      if (!read_extent(*e, s - e->start, q, n)) return false;
      q += n * kSector;
      s += n;
      left -= n;
    }
    std::memcpy(data, buf.data() + (offset - a0), len);
    return true;
  }
  uint64_t s = offset / kSector, left = len / kSector;
  auto* p = static_cast<uint8_t*>(data);
  for (auto& e : extents_) {
    if (!left) break;
    if (s >= e->start + e->sectors) continue;
    const uint64_t n = std::min(left, e->start + e->sectors - s);
    if (!read_extent(*e, s - e->start, p, n)) return false;
    p += n * kSector;
    s += n;
    left -= n;
  }
  return left == 0;
}

bool VmdkDisk::write(uint64_t offset, const void* data, size_t len) {
  std::lock_guard lock(mutex_);
  if (read_only_ || offset > size_bytes() || len > size_bytes() - offset || (offset | len) % kSector) return false;
  if (!cid_updated_ && !update_cid()) return false;
  uint64_t s = offset / kSector, left = len / kSector;
  auto* p = static_cast<const uint8_t*>(data);
  for (auto& e : extents_) {
    if (!left) break;
    if (s >= e->start + e->sectors) continue;
    const uint64_t n = std::min(left, e->start + e->sectors - s);
    if (!write_extent(*e, s - e->start, p, n)) return false;
    p += n * kSector;
    s += n;
    left -= n;
  }
  return left == 0;
}

bool VmdkDisk::update_cid() {
  const size_t pos = descriptor_.find("\nCID=");
  if (!descriptor_file_ || !descriptor_file_->writable() || pos == std::string::npos) {
    cid_updated_ = true;  // nothing to tell VMware through
    return true;
  }
  const size_t end = descriptor_.find('\n', pos + 1);
  char buf[16];
  const uint32_t cid = random_cid();
  std::snprintf(buf, sizeof(buf), "%08x", cid);
  std::string text = descriptor_;
  text.replace(pos + 5, (end == std::string::npos ? text.size() : end) - (pos + 5), buf);
  bool ok;
  if (descriptor_space_) {
    if (text.size() > descriptor_space_) return false;
    std::vector<uint8_t> area(descriptor_space_, 0);
    std::memcpy(area.data(), text.data(), text.size());
    ok = descriptor_file_->write(descriptor_offset_, area.data(), area.size());
  } else {
    ok = descriptor_file_->write(0, text.data(), text.size()) && descriptor_file_->set_size(text.size());
  }
  // The new CID must be on disk before the first grain changes, or a snapshot would still match.
  if (!ok || !descriptor_file_->flush()) return false;
  descriptor_ = std::move(text);
  cid_ = cid;
  cid_updated_ = true;
  return true;
}

void VmdkDisk::repair_tables(Extent& e, bool writable) {
  // After an unclean shutdown the primary and redundant grain tables may disagree (one of the two GTE
  // writes of a new grain was lost). Keep, per entry, a value that points inside the file, preferring
  // the primary one (written first), and make both copies agree again.
  const uint64_t file_sectors = e.file->size() / kSector;
  auto valid = [&](uint32_t v) {
    return v == 0 || (v == 1 && e.zeroed_gte) || uint64_t{v} + e.grain <= file_sectors;
  };
  const uint64_t gt_bytes = uint64_t{e.gtes} * 4;
  unsigned fixed = 0;
  for (size_t t = 0; t < e.gd.size(); ++t) {
    if (!e.gd[t] || !e.rgd[t]) continue;
    std::vector<uint8_t> primary(gt_bytes), redundant(gt_bytes);
    if (!e.file->read(uint64_t{e.gd[t]} * kSector, primary.data(), gt_bytes) ||
        !e.file->read(uint64_t{e.rgd[t]} * kSector, redundant.data(), gt_bytes)) {
      throw vmdk_error(e.file->path(), "grain table out of range");
    }
    for (uint32_t i = 0; i < e.gtes; ++i) {
      const uint32_t p = le32(primary.data() + 4 * i), r = le32(redundant.data() + 4 * i);
      if (p == r) continue;
      const uint32_t keep = valid(p) ? p : valid(r) ? r : 0;
      ++fixed;
      if (!writable) continue;
      uint8_t raw[4];
      put_le32(raw, keep);
      e.file->write(uint64_t{e.gd[t]} * kSector + 4 * i, raw, 4);
      e.file->write(uint64_t{e.rgd[t]} * kSector + 4 * i, raw, 4);
    }
  }
  if (fixed) {
    SVM_WARN("VMDK %s was not closed cleanly: %u grain table entries disagreed%s", narrow(e.file->path()).c_str(),
             fixed, writable ? ", repaired" : " (open it writable to repair)");
  }
}

bool VmdkDisk::flush() {
  std::lock_guard lock(mutex_);
  bool ok = true;
  for (auto& e : extents_) {
    if (e->file && e->writable) ok = e->file->flush() && ok;
  }
  return ok;
}

void VmdkDisk::create_sparse(const std::wstring& path, uint64_t size_bytes) {
  constexpr uint64_t kGrain = 128;  // 64 KiB
  constexpr uint32_t kGtes = 512;
  const uint64_t capacity = (size_bytes + kGrain * kSector - 1) / (kGrain * kSector) * kGrain;
  if (capacity == 0 || capacity > (2ull << 40) / kSector) throw vmdk_error(path, "size must be 1 byte .. 2 TiB");
  const uint64_t tables = (capacity / kGrain + kGtes - 1) / kGtes;
  const uint64_t gt_sectors = kGtes * 4 / kSector;
  const uint64_t gd_sectors = (tables * 4 + kSector - 1) / kSector;
  constexpr uint64_t kDescOffset = 1, kDescSize = 20;
  const uint64_t rgd = kDescOffset + kDescSize;
  const uint64_t rgt = rgd + gd_sectors;
  const uint64_t gd = rgt + tables * gt_sectors;
  const uint64_t gt = gd + gd_sectors;
  const uint64_t overhead = (gt + tables * gt_sectors + kGrain - 1) / kGrain * kGrain;

  std::vector<uint8_t> image(overhead * kSector, 0);
  uint8_t* h = image.data();
  put_le32(h, kMagic);
  put_le32(h + 4, 1);
  put_le32(h + 8, 1 | kFlagRedundantGt);  // newline test valid, redundant tables
  put_le64(h + 12, capacity);
  put_le64(h + 20, kGrain);
  put_le64(h + 28, kDescOffset);
  put_le64(h + 36, kDescSize);
  put_le32(h + 44, kGtes);
  put_le64(h + 48, rgd);
  put_le64(h + 56, gd);
  put_le64(h + 64, overhead);
  h[73] = '\n';
  h[74] = ' ';
  h[75] = '\r';
  h[76] = '\n';
  for (uint64_t i = 0; i < tables; ++i) {
    put_le32(image.data() + rgd * kSector + 4 * i, static_cast<uint32_t>(rgt + i * gt_sectors));
    put_le32(image.data() + gd * kSector + 4 * i, static_cast<uint32_t>(gt + i * gt_sectors));
  }
  const uint64_t cylinders = std::min<uint64_t>(capacity / (16 * 63), 16383);
  char desc[1024];
  const int n = std::snprintf(desc, sizeof(desc),
                              "# Disk DescriptorFile\nversion=1\nencoding=\"UTF-8\"\nCID=%08x\nparentCID=ffffffff\n"
                              "createType=\"monolithicSparse\"\n\n# Extent description\nRW %llu SPARSE \"%s\"\n\n"
                              "# The Disk Data Base\n#DDB\n\nddb.virtualHWVersion = \"4\"\n"
                              "ddb.geometry.cylinders = \"%llu\"\nddb.geometry.heads = \"16\"\n"
                              "ddb.geometry.sectors = \"63\"\nddb.adapterType = \"ide\"\n",
                              random_cid(), static_cast<unsigned long long>(capacity),
                              narrow(file_name_of(path)).c_str(), static_cast<unsigned long long>(cylinders));
  if (n <= 0 || static_cast<uint64_t>(n) >= kDescSize * kSector) throw vmdk_error(path, "descriptor too long");
  std::memcpy(image.data() + kDescOffset * kSector, desc, n);

  auto file = HostFile::create(path);
  const bool ok = file->write(0, image.data(), image.size()) && file->flush();
  file.reset();
  if (!ok) {
    DeleteFileW(path.c_str());
    throw vmdk_error(path, "write failed");
  }
}

}  // namespace sandvm::block
