#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace sandvm {

// Devices receive absolute port numbers / guest physical addresses.
class PioDevice {
 public:
  virtual ~PioDevice() = default;
  virtual uint32_t pio_read(uint16_t port, unsigned size) = 0;
  virtual void pio_write(uint16_t port, unsigned size, uint32_t value) = 0;
};

class MmioDevice {
 public:
  virtual ~MmioDevice() = default;
  virtual uint64_t mmio_read(uint64_t gpa, unsigned size) = 0;
  virtual void mmio_write(uint64_t gpa, unsigned size, uint64_t value) = 0;
};

// Routes trapped port I/O and MMIO accesses to registered devices. Unclaimed reads return
// all-ones, unclaimed writes are dropped. A device only ever sees accesses that lie entirely
// inside one of its ranges: a wider access that crosses a range boundary is split into bytes,
// each routed on its own, as the ISA bus would do.
//
// The bus shares ownership of its devices, so a device stays alive while any vCPU may still
// dispatch to it. Devices are called without any bus lock held and must do their own locking;
// a device may therefore call back into the bus or block without deadlocking other vCPUs.
class IoBus {
 public:
  void register_pio(uint16_t base, uint16_t count, std::shared_ptr<PioDevice> device);
  void register_mmio(uint64_t base, uint64_t size, std::shared_ptr<MmioDevice> device);
  // Removes the range registered at exactly `base` for `device` (PCI BAR moves). No-op if absent.
  void unregister_pio(uint16_t base, const PioDevice* device);
  void unregister_mmio(uint64_t base, const MmioDevice* device);

  uint32_t pio_read(uint16_t port, unsigned size);
  void pio_write(uint16_t port, unsigned size, uint32_t value);
  uint64_t mmio_read(uint64_t gpa, unsigned size);
  void mmio_write(uint64_t gpa, unsigned size, uint64_t value);

 private:
  struct PioRange {
    uint32_t base;
    uint32_t count;
    std::shared_ptr<PioDevice> device;
  };
  struct MmioRange {
    uint64_t base;
    uint64_t size;
    std::shared_ptr<MmioDevice> device;
  };

  // Returns a copy so the device can be called after the lock is dropped.
  std::optional<PioRange> find_pio(uint16_t port) const;
  std::optional<MmioRange> find_mmio(uint64_t gpa) const;

  mutable std::shared_mutex mutex_;  // guards the range tables only, never held across a device call
  std::vector<PioRange> pio_;
  std::vector<MmioRange> mmio_;
};

}  // namespace sandvm
