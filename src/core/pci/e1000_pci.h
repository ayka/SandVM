#pragma once

#include <cstdint>
#include <memory>

#include "core/devices/e1000.h"
#include "core/pci/pci.h"

namespace sandvm::pci {

// The 82540EM as a PCI function: 8086:100E, class 0x0200 (Ethernet), subsystem 8086:001E (PRO/1000 MT
// Desktop, which the Windows inbox driver matches), INTA#. BAR0 = 128 KiB register window, BAR1 = the
// 8-byte IOADDR/IODATA pair for indirect register access (in a 64-byte I/O BAR).
class E1000Pci : public PciFunction {
 public:
  E1000Pci(IoBus& io, std::shared_ptr<devices::E1000> nic);

  devices::E1000& nic() { return *nic_; }

 protected:
  void after_config_write(unsigned off, unsigned size) override;

 private:
  // One object per BAR0 placement, so accesses in flight keep the base they were dispatched with.
  class Registers : public MmioDevice {
   public:
    Registers(std::shared_ptr<devices::E1000> nic, uint64_t base) : nic_(std::move(nic)), base_(base) {}
    uint64_t mmio_read(uint64_t gpa, unsigned size) override;
    void mmio_write(uint64_t gpa, unsigned size, uint64_t value) override;

   private:
    const std::shared_ptr<devices::E1000> nic_;
    const uint64_t base_;
  };
  class IoWindow : public PioDevice {
   public:
    explicit IoWindow(std::shared_ptr<devices::E1000> nic) : nic_(std::move(nic)) {}
    uint32_t pio_read(uint16_t port, unsigned size) override;
    void pio_write(uint16_t port, unsigned size, uint32_t value) override;

   private:
    const std::shared_ptr<devices::E1000> nic_;
    std::mutex mutex_;
    uint32_t address_ = 0;  // IOADDR
  };

  void map_registers(uint64_t base);

  IoBus& io_;
  const std::shared_ptr<devices::E1000> nic_;
  std::shared_ptr<Registers> registers_;
  uint64_t registers_base_ = 0;
};

}  // namespace sandvm::pci
