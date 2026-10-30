#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include "core/io_bus.h"

namespace sandvm::pci {

// One PCI function with a type 0 (256-byte) configuration space. Subclasses describe their registers
// with write masks and BARs; the base class implements the generic semantics: read-only bits, BAR
// sizing (writing all-ones reads back the size mask) and BAR decoding, which follows the BAR values and
// the command register's I/O / memory enable bits (PCI Local Bus Spec 3.0, 6.2.5).
//
// Thread-safe. Config cycles take the function's lock; a subclass hook runs with it held.
class PciFunction {
 public:
  virtual ~PciFunction() = default;
  PciFunction(const PciFunction&) = delete;
  PciFunction& operator=(const PciFunction&) = delete;

  // off is naturally aligned for size (1, 2 or 4) and below 256.
  uint32_t config_read(unsigned off, unsigned size);
  void config_write(unsigned off, unsigned size, uint32_t value);

 protected:
  // class_code = base class << 16 | subclass << 8 | programming interface.
  PciFunction(IoBus& io, uint16_t vendor, uint16_t device, uint8_t revision, uint32_t class_code,
              bool multifunction = false);

  // An I/O BAR of `size` bytes (power of two, >= 4) decoded to `device` while I/O space is enabled.
  // Call from the subclass constructor.
  void add_io_bar(unsigned index, uint16_t size, std::shared_ptr<PioDevice> device);
  // A 32-bit memory BAR of `size` bytes (power of two, >= 4 KiB). The device decodes it itself: on_map
  // gets the new base whenever decoding moves (0 = no longer decoded). It runs with the function lock
  // held, on the vCPU thread doing the config write. Call from the subclass constructor.
  using MemBarMap = std::function<void(uint64_t base)>;
  void add_mem_bar(unsigned index, uint32_t size, bool prefetchable, MemBarMap on_map);

  void set_config(unsigned off, unsigned size, uint32_t value);  // initial values, bypassing masks
  uint32_t get_config(unsigned off, unsigned size) const;
  void set_wmask(unsigned off, unsigned size, uint32_t mask);
  void set_interrupt_pin(uint8_t pin);  // 1 = INTA# .. 4 = INTD#; makes INTERRUPT_LINE writable

  // Called after a guest config write with the lock held; `off`/`size` are what the guest wrote.
  virtual void after_config_write(unsigned /*off*/, unsigned /*size*/) {}

  std::mutex mutex_;

 private:
  struct IoBar {
    uint16_t size = 0;
    std::shared_ptr<PioDevice> device;
    uint32_t mapped_at = 0;  // 0 = not decoded
  };
  struct MemBar {
    uint32_t size = 0;
    MemBarMap on_map;
    uint32_t mapped_at = 0;  // 0 = not decoded
  };
  void update_bars();

  IoBus& io_;
  std::array<uint8_t, 256> config_{};
  std::array<uint8_t, 256> wmask_{};
  std::array<IoBar, 6> io_bars_{};
  std::array<MemBar, 6> mem_bars_{};
};

// Configuration mechanism #1: CONFIG_ADDRESS at 0xCF8 (dword only) and CONFIG_DATA at 0xCFC-0xCFF,
// bus 0 only. Absent functions read all-ones, which is how software enumerates.
class PciBus : public PioDevice {
 public:
  static constexpr uint16_t kConfigAddress = 0xCF8;
  static constexpr uint16_t kPortCount = 8;

  using ResetCallback = std::function<void()>;
  // on_reset: the guest pulsed a reset through the PIIX reset control register (0xCF9).
  explicit PciBus(ResetCallback on_reset = {}) : on_reset_(std::move(on_reset)) {}

  // devfn = device << 3 | function. Call before the VM runs.
  void attach(unsigned devfn, std::shared_ptr<PciFunction> function);

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  PciFunction* target(unsigned* off);

  const ResetCallback on_reset_;
  std::mutex mutex_;  // guards address_ and rcr_; never held across a function's config cycle
  uint32_t address_ = 0;
  uint8_t rcr_ = 0;  // reset control register at 0xCF9
  std::array<std::shared_ptr<PciFunction>, 256> functions_{};
};

}  // namespace sandvm::pci
