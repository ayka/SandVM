#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>

#include "core/io_bus.h"

namespace sandvm::devices {

// Intel 82093AA I/O APIC: 24 redirection entries behind the IOREGSEL (0x00) / IOWIN (0x10) window at
// 0xFEC00000. Turns GSI line changes into interrupt messages for the local APICs, which the hypervisor
// emulates; level-triggered entries keep Remote IRR set until the vector's EOI comes back (eoi()).
//
// Thread-safe: set_irq() may be called from any thread. The message callback runs with the device lock
// held, so it must not call back into the IOAPIC (lock order: device -> IOAPIC -> hypervisor).
class IoApic : public MmioDevice {
 public:
  static constexpr uint64_t kBase = 0xFEC00000;
  static constexpr uint64_t kSize = 0x1000;
  static constexpr unsigned kPins = 24;

  struct Message {
    uint8_t vector;
    uint8_t delivery_mode;  // 0 fixed, 1 lowest priority, 2 SMI, 4 NMI, 5 INIT, 7 ExtINT
    bool logical;           // destination mode
    bool level;             // trigger mode
    uint8_t destination;    // APIC ID (physical) or logical mask
  };
  // Returns false if the message could not be sent (it stays pending and is retried on the next change).
  using Deliver = std::function<bool(const Message&)>;

  // id: the 4-bit I/O APIC ID the MADT reports.
  IoApic(uint8_t id, Deliver deliver);

  void set_irq(unsigned gsi, bool level);
  // A local APIC signalled EOI for a level-triggered `vector`.
  void eoi(uint8_t vector);

  uint64_t mmio_read(uint64_t gpa, unsigned size) override;
  void mmio_write(uint64_t gpa, unsigned size, uint64_t value) override;

 private:
  // Redirection entry bits.
  static constexpr uint64_t kMasked = 1ull << 16;
  static constexpr uint64_t kLevel = 1ull << 15;
  static constexpr uint64_t kRemoteIrr = 1ull << 14;
  static constexpr uint64_t kDeliveryStatus = 1ull << 12;

  uint32_t read_register(uint8_t index) const;
  void write_register(uint8_t index, uint32_t value);
  // Sends pin's message if it is asserted (level) or latched (edge), unmasked and not waiting for EOI.
  void service(unsigned pin);

  const Deliver deliver_;
  std::mutex mutex_;
  uint8_t select_ = 0;
  uint8_t id_;
  std::array<uint64_t, kPins> redirection_{};
  uint32_t lines_ = 0;    // current input levels
  uint32_t pending_ = 0;  // edge seen while masked or level asserted, not yet delivered
};

}  // namespace sandvm::devices
