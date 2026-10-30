#include "core/devices/fw_cfg.h"

#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace sandvm::devices {
namespace {
constexpr uint16_t kFirstFile = 0x0020;
constexpr size_t kNameSize = 56;

void put_be(std::vector<uint8_t>& out, uint64_t value, unsigned bytes) {
  for (unsigned i = bytes; i-- > 0;) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}
}  // namespace

FwCfg::FwCfg() {
  add_bytes(kSignature, {'Q', 'E', 'M', 'U'});
  add_bytes(kId, {0x01, 0x00, 0x00, 0x00});  // feature bits: traditional interface only, no DMA
  rebuild_directory();
}

void FwCfg::add_bytes(uint16_t key, std::vector<uint8_t> data) {
  std::lock_guard lock(mutex_);
  items_[key] = {std::move(data), {}};
}

void FwCfg::add_u16(uint16_t key, uint16_t value) {
  add_bytes(key, {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)});
}

void FwCfg::add_u64(uint16_t key, uint64_t value) {
  std::vector<uint8_t> data;
  for (unsigned i = 0; i < 8; ++i) data.push_back(static_cast<uint8_t>(value >> (8 * i)));
  add_bytes(key, std::move(data));
}

void FwCfg::add_file(const std::string& name, std::vector<uint8_t> data, Refresh refresh) {
  if (name.size() >= kNameSize) throw std::invalid_argument("fw_cfg file name too long: " + name);
  std::lock_guard lock(mutex_);
  const uint16_t key = static_cast<uint16_t>(kFirstFile + files_.size());
  items_[key] = {std::move(data), std::move(refresh)};
  files_.emplace_back(name, key);
  rebuild_directory();
}

void FwCfg::rebuild_directory() {
  // Big-endian count, then {u32 size, u16 select, u16 reserved, char name[56]} per file.
  std::vector<uint8_t> dir;
  put_be(dir, files_.size(), 4);
  for (const auto& [name, key] : files_) {
    put_be(dir, items_[key].data.size(), 4);
    put_be(dir, key, 2);
    put_be(dir, 0, 2);
    std::vector<uint8_t> field(kNameSize, 0);
    std::copy(name.begin(), name.end(), field.begin());
    dir.insert(dir.end(), field.begin(), field.end());
  }
  items_[kFileDir] = {std::move(dir), {}};
}

uint32_t FwCfg::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  if (port != kDataPort) return 0;  // the selector is write-only
  // The data register is 8 bits wide; wider reads return successive bytes, as QEMU's port does.
  uint32_t value = 0;
  auto it = items_.find(selected_);
  for (unsigned i = 0; i < size; ++i) {
    uint8_t byte = 0;
    if (it != items_.end() && offset_ < it->second.data.size()) byte = it->second.data[offset_++];
    value |= uint32_t{byte} << (8 * i);
  }
  return value;
}

void FwCfg::pio_write(uint16_t port, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  if (port != kSelectorPort || size < 2) return;  // writes through the data port are not supported
  // Bit 14 marks a write selection and bit 15 the architecture-specific range; the item key keeps both.
  selected_ = static_cast<uint16_t>(value);
  offset_ = 0;
  auto it = items_.find(selected_);
  if (it == items_.end() || !it->second.refresh) return;
  std::vector<uint8_t> fresh = it->second.refresh();
  if (fresh.size() != it->second.data.size()) {
    SVM_WARN("fw_cfg: refreshed item 0x%04x changed size (%zu -> %zu), keeping the old contents", selected_,
             it->second.data.size(), fresh.size());
    return;
  }
  it->second.data = std::move(fresh);
}

}  // namespace sandvm::devices
