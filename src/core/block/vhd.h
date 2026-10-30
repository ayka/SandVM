#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/block/disk.h"
#include "core/block/host_file.h"

namespace sandvm::block {

// Microsoft Virtual Hard Disk (VHD) image, format version 1.0 (the "Virtual Hard Disk Image Format
// Specification" Microsoft published for Virtual PC / Hyper-V; Windows mounts these natively):
//  - fixed: raw data followed by a 512-byte footer;
//  - dynamic: a block allocation table (BAT) of 2 MiB blocks, each a sector bitmap plus data, allocated
//    on first write at the end of the file;
//  - differencing: like dynamic, but a sector whose bitmap bit is clear comes from the parent image.
//    That is how snapshots and linked clones work: the parent is frozen, the child takes the writes.
// All multi-byte fields are big-endian. Thread-safe (one lock per image).
class VhdDisk : public Disk {
 public:
  enum class Type : uint32_t { Fixed = 2, Dynamic = 3, Differencing = 4 };
  static constexpr uint32_t kDefaultBlockSize = 2u << 20;
  static constexpr uint64_t kMaxSize = 2040ull << 30;  // the format's limit (CHS geometry, 32-bit BAT)

  // Throws std::runtime_error on a malformed image or a missing / mismatched parent. With
  // open_parent = false a differencing image reads parent sectors as zeros (tools working on the
  // child's own data); parent_path() is still resolved.
  static std::unique_ptr<VhdDisk> open(std::unique_ptr<HostFile> file, bool open_parent, int depth);
  // True if the file ends with (or, for dynamic images, starts with) a VHD footer.
  static bool probe(const HostFile& file);

  // New images; both throw std::runtime_error and remove a half-written file.
  static void create_dynamic(const std::wstring& path, uint64_t size_bytes);
  // A differencing image on top of `parent_path`, which may be any image open_image() reads (a raw
  // parent has no identity to check, so it must simply not change any more).
  static void create_differencing(const std::wstring& path, const std::wstring& parent_path);

  uint64_t size_bytes() const override { return size_; }
  bool read_only() const override { return !file_->writable(); }
  bool read(uint64_t offset, void* data, size_t len) override;
  bool write(uint64_t offset, const void* data, size_t len) override;
  bool flush() override { return file_->flush(); }
  bool unique_id(uint8_t out[16]) const override;

  Type type() const { return type_; }
  const std::wstring& path() const { return file_->path(); }
  const std::wstring& parent_path() const { return parent_path_; }  // differencing only
  Disk* parent() { return parent_.get(); }
  uint64_t file_bytes() const { return file_->size(); }
  uint32_t allocated_blocks() const;
  uint32_t block_size() const { return block_size_; }

  // Every run of sectors this file holds itself (for a dynamic image: whole allocated blocks), in
  // ascending order; stops early when fn returns false. Returns false on an I/O error.
  bool for_each_own_range(const std::function<bool(uint64_t offset, uint64_t length)>& fn);
  // Points a differencing image at a new parent (after the old parent was merged into it). The new
  // parent's identity (VHD unique ID, else its modification time) is recorded; nothing is checked
  // against the data.
  void set_parent(const std::wstring& parent_path, const Disk& parent);
  // A fresh unique ID, after the contents were changed by a tool (commit): every differencing image
  // made from the old contents then refuses to open instead of reading a mix.
  void new_unique_id();

 private:
  static constexpr uint32_t kUnused = 0xFFFFFFFF;

  explicit VhdDisk(std::unique_ptr<HostFile> file) : file_(std::move(file)) {}

  bool read_block(uint32_t block, uint32_t in, uint8_t* p, uint32_t n);
  bool write_block(uint32_t block, uint32_t in, const uint8_t* p, uint32_t n);
  bool read_parent(uint64_t offset, uint8_t* p, size_t n);
  bool allocate(uint32_t block);
  const std::vector<uint8_t>* bitmap(uint32_t block);
  bool write_footer(uint64_t at);
  bool write_header();
  void resolve_parent(const uint8_t* header, bool open_parent, int depth);
  uint64_t block_offset(uint32_t block) const { return uint64_t{bat_[block]} * 512; }

  std::unique_ptr<HostFile> file_;
  std::unique_ptr<Disk> parent_;
  std::wstring parent_path_;
  Type type_ = Type::Fixed;
  uint64_t size_ = 0;
  uint8_t footer_[512] = {};
  uint8_t header_[1024] = {};
  uint64_t header_offset_ = 0;
  uint64_t bat_offset_ = 0;
  uint32_t block_size_ = kDefaultBlockSize;
  uint32_t bitmap_bytes_ = 512;
  std::vector<uint32_t> bat_;
  std::vector<std::vector<uint8_t>> bitmaps_;  // differencing: per block, loaded on first use
  uint64_t next_free_ = 0;                      // where the next block goes; the footer sits there

  std::mutex mutex_;
};

}  // namespace sandvm::block
