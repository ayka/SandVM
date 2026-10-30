#include "core/io_bus.h"

#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace sandvm {
namespace {
uint64_t all_ones(unsigned size) { return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1; }
}  // namespace

void IoBus::register_pio(uint16_t base, uint16_t count, std::shared_ptr<PioDevice> device) {
  if (!device || count == 0) throw std::invalid_argument("register_pio: null device or empty range");
  std::unique_lock lock(mutex_);
  for (const auto& r : pio_) {
    if (base < r.base + r.count && r.base < uint32_t{base} + count) throw std::invalid_argument("overlapping PIO range");
  }
  pio_.push_back({base, count, std::move(device)});
}

void IoBus::register_mmio(uint64_t base, uint64_t size, std::shared_ptr<MmioDevice> device) {
  if (!device || size == 0 || base + size < base) throw std::invalid_argument("register_mmio: bad device or range");
  std::unique_lock lock(mutex_);
  for (const auto& r : mmio_) {
    if (base < r.base + r.size && r.base < base + size) throw std::invalid_argument("overlapping MMIO range");
  }
  mmio_.push_back({base, size, std::move(device)});
}

void IoBus::unregister_pio(uint16_t base, const PioDevice* device) {
  std::unique_lock lock(mutex_);
  std::erase_if(pio_, [&](const PioRange& r) { return r.base == base && r.device.get() == device; });
}

void IoBus::unregister_mmio(uint64_t base, const MmioDevice* device) {
  std::unique_lock lock(mutex_);
  std::erase_if(mmio_, [&](const MmioRange& r) { return r.base == base && r.device.get() == device; });
}

std::optional<IoBus::PioRange> IoBus::find_pio(uint16_t port) const {
  std::shared_lock lock(mutex_);
  for (const auto& r : pio_) {
    if (port >= r.base && port - r.base < r.count) return r;
  }
  return std::nullopt;
}

std::optional<IoBus::MmioRange> IoBus::find_mmio(uint64_t gpa) const {
  std::shared_lock lock(mutex_);
  for (const auto& r : mmio_) {
    if (gpa >= r.base && gpa - r.base < r.size) return r;
  }
  return std::nullopt;
}

uint32_t IoBus::pio_read(uint16_t port, unsigned size) {
  auto r = find_pio(port);
  if (r && uint32_t{port} + size <= r->base + r->count) return r->device->pio_read(port, size);
  if (size == 1) {  // a single byte is always inside its range, so this one is unclaimed
    SVM_DEBUG("unclaimed PIO read  port=0x%04x", port);
    return static_cast<uint32_t>(all_ones(1));
  }
  // Crosses a range boundary or starts unclaimed: one byte at a time, each to its own owner.
  uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= pio_read(static_cast<uint16_t>(port + i), 1) << (8 * i);
  return value;
}

void IoBus::pio_write(uint16_t port, unsigned size, uint32_t value) {
  auto r = find_pio(port);
  if (r && uint32_t{port} + size <= r->base + r->count) return r->device->pio_write(port, size, value);
  if (size == 1) {
    SVM_DEBUG("unclaimed PIO write port=0x%04x value=0x%02x", port, value);
    return;
  }
  for (unsigned i = 0; i < size; ++i) pio_write(static_cast<uint16_t>(port + i), 1, (value >> (8 * i)) & 0xFF);
}

uint64_t IoBus::mmio_read(uint64_t gpa, unsigned size) {
  auto r = find_mmio(gpa);
  if (r && size <= r->size - (gpa - r->base)) return r->device->mmio_read(gpa, size);
  if (size == 1) {  // a single byte is always inside its range, so this one is unclaimed
    SVM_DEBUG("unclaimed MMIO read  gpa=0x%llx", static_cast<unsigned long long>(gpa));
    return all_ones(1);
  }
  uint64_t value = 0;
  for (unsigned i = 0; i < size && i < 8; ++i) value |= mmio_read(gpa + i, 1) << (8 * i);
  return value;
}

void IoBus::mmio_write(uint64_t gpa, unsigned size, uint64_t value) {
  auto r = find_mmio(gpa);
  if (r && size <= r->size - (gpa - r->base)) return r->device->mmio_write(gpa, size, value);
  if (size == 1) {
    SVM_DEBUG("unclaimed MMIO write gpa=0x%llx value=0x%02llx", static_cast<unsigned long long>(gpa),
              static_cast<unsigned long long>(value));
    return;
  }
  for (unsigned i = 0; i < size && i < 8; ++i) mmio_write(gpa + i, 1, (value >> (8 * i)) & 0xFF);
}

}  // namespace sandvm
