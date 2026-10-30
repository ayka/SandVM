#include "core/block/disk.h"

#include <cstring>

namespace sandvm::block {
namespace {
bool in_range(uint64_t offset, size_t len, uint64_t size) { return offset <= size && len <= size - offset; }
}  // namespace

std::unique_ptr<FileDisk> FileDisk::open(const std::wstring& path, bool read_only) {
  auto file = HostFile::open(path, !read_only);
  const uint64_t size = file->size();
  return std::make_unique<FileDisk>(std::move(file), size);
}

bool FileDisk::read(uint64_t offset, void* data, size_t len) {
  return in_range(offset, len, size_) && file_->read(offset, data, len);
}

bool FileDisk::write(uint64_t offset, const void* data, size_t len) {
  return in_range(offset, len, size_) && file_->write(offset, data, len);
}

bool MemoryDisk::read(uint64_t offset, void* data, size_t len) {
  std::lock_guard lock(mutex_);
  if (!in_range(offset, len, data_.size())) return false;
  std::memcpy(data, data_.data() + offset, len);
  return true;
}

bool MemoryDisk::write(uint64_t offset, const void* data, size_t len) {
  std::lock_guard lock(mutex_);
  if (!in_range(offset, len, data_.size())) return false;
  std::memcpy(data_.data() + offset, data, len);
  return true;
}

}  // namespace sandvm::block
