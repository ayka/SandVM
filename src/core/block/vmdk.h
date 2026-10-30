#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/block/disk.h"
#include "core/block/host_file.h"

namespace sandvm::block {

// VMware virtual disk (VMDK), as VMware Workstation / Player write them ("Virtual Disk Format 1.1"):
// a text descriptor (embedded in a monolithicSparse file or a small .vmdk of its own) listing extents.
// Supported extents: hosted SPARSE (grain directory -> grain tables -> 64 KiB grains, allocated on
// write at the end of the extent file, primary and redundant tables kept in sync), FLAT (raw) and ZERO.
// Not supported: compressed / streamOptimized extents (OVA exports) and ESXi VMFS formats.
// A snapshot ("-000001.vmdk") reads unallocated grains from its parent (parentFileNameHint), whose
// CID must match parentCID, as VMware checks. The first write gives the disk a new CID so VMware
// notices the change too. Thread-safe.
class VmdkDisk : public Disk {
 public:
  static bool probe(const HostFile& file);
  static std::unique_ptr<VmdkDisk> open(std::unique_ptr<HostFile> file, int depth);
  // A new, empty monolithicSparse disk (what "single file" in VMware Workstation creates).
  static void create_sparse(const std::wstring& path, uint64_t size_bytes);
  ~VmdkDisk() override;

  uint64_t size_bytes() const override { return sectors_ * 512; }
  bool read_only() const override { return read_only_; }
  bool read(uint64_t offset, void* data, size_t len) override;
  bool write(uint64_t offset, const void* data, size_t len) override;
  bool flush() override;

  const std::string& create_type() const { return create_type_; }
  const std::wstring& parent_path() const { return parent_path_; }
  uint32_t cid() const { return cid_; }

 private:
  struct Extent;
  VmdkDisk() = default;

  void parse_descriptor(const std::string& text, std::unique_ptr<HostFile> self, int depth);
  bool read_extent(Extent& e, uint64_t sector, uint8_t* p, uint64_t count);
  bool write_extent(Extent& e, uint64_t sector, const uint8_t* p, uint64_t count);
  bool read_parent(uint64_t sector, uint8_t* p, uint64_t count);
  std::vector<uint32_t>* grain_table(Extent& e, uint32_t index, bool allocate);
  bool update_cid();
  void repair_tables(Extent& e, bool writable);

  std::vector<std::unique_ptr<Extent>> extents_;
  std::unique_ptr<Disk> parent_;
  std::wstring parent_path_;
  std::string create_type_;
  std::string descriptor_;          // text as read; rewritten with a new CID on the first write
  HostFile* descriptor_file_ = nullptr;  // owned by an extent or descriptor_owner_
  std::unique_ptr<HostFile> descriptor_owner_;
  uint64_t descriptor_offset_ = 0;  // bytes; embedded descriptors have a fixed-size area
  uint64_t descriptor_space_ = 0;   // 0 = separate file, rewritten whole
  uint64_t sectors_ = 0;
  uint32_t cid_ = 0;
  bool read_only_ = true;
  bool cid_updated_ = false;
  std::mutex mutex_;
};

}  // namespace sandvm::block
