#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/block/host_file.h"

namespace sandvm::block {

// Byte-addressed backing store for an emulated disk. Implementations are thread-safe.
class Disk {
 public:
  virtual ~Disk() = default;
  virtual uint64_t size_bytes() const = 0;
  virtual bool read_only() const = 0;
  // Return false on I/O error or if [offset, offset+len) is outside the disk.
  virtual bool read(uint64_t offset, void* data, size_t len) = 0;
  virtual bool write(uint64_t offset, const void* data, size_t len) = 0;
  virtual bool flush() = 0;
  // Identity that a differencing image records for its parent (VHD unique ID); false if the format has none.
  virtual bool unique_id(uint8_t out[16]) const {
    (void)out;
    return false;
  }
};

// Raw image file ("dd" format), or the data part of a fixed VHD (`size` leaves its footer out).
// Opened with sharing for reads only, so two VMs cannot write one image.
class FileDisk : public Disk {
 public:
  // Throws std::runtime_error if the file cannot be opened.
  static std::unique_ptr<FileDisk> open(const std::wstring& path, bool read_only);
  FileDisk(std::unique_ptr<HostFile> file, uint64_t size) : file_(std::move(file)), size_(size) {}

  uint64_t size_bytes() const override { return size_; }
  bool read_only() const override { return !file_->writable(); }
  bool read(uint64_t offset, void* data, size_t len) override;
  bool write(uint64_t offset, const void* data, size_t len) override;
  bool flush() override { return file_->flush(); }

 private:
  std::unique_ptr<HostFile> file_;
  const uint64_t size_;
};

// In-memory disk, used by tests.
class MemoryDisk : public Disk {
 public:
  explicit MemoryDisk(std::vector<uint8_t> data) : data_(std::move(data)) {}

  uint64_t size_bytes() const override { return data_.size(); }
  bool read_only() const override { return false; }
  bool read(uint64_t offset, void* data, size_t len) override;
  bool write(uint64_t offset, const void* data, size_t len) override;
  bool flush() override { return true; }

 private:
  std::mutex mutex_;
  std::vector<uint8_t> data_;
};

}  // namespace sandvm::block
