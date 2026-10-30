#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

#include "core/devices/ide.h"
#include "core/pci/pci.h"

namespace sandvm::pci {

// The i440FX/PIIX3 chipset as QEMU's "pc" machine exposes it, which is what SeaBIOS, Linux and
// Windows all know: host bridge at 00:00.0, PIIX3 ISA bridge at 00:01.0, PIIX3 IDE at 00:01.1.

// 82441FX PMC (host bridge), 8086:1237. PAM (0x59-0x5F) and SMRAM (0x72) are storage only: guest RAM
// below 1 MiB is always plain RAM here, so the shadow-RAM state they select is already in effect.
class I440fxHostBridge : public PciFunction {
 public:
  explicit I440fxHostBridge(IoBus& io);
};

// 82371SB PIIX3 function 0 (PCI-to-ISA bridge), 8086:7000. Holds the PIRQ[A-D] -> ISA IRQ routing
// registers 0x60-0x63, fixed to acpi::kPirqIrq here, which route() reports to PCI devices.
class Piix3Isa : public PciFunction {
 public:
  explicit Piix3Isa(IoBus& io);
  // ISA IRQ that PIRQ line `pirq` (0 = A) is routed to, or -1 if disabled.
  int route(unsigned pirq);
};

// 82371SB PIIX3 function 1 (IDE), 8086:7010, both channels in legacy compatibility mode at
// 0x1F0/0x3F6 (IRQ14) and 0x170/0x376 (IRQ15). BAR4 is the bus-master block: 8 registers per channel,
// owned by the channels, which do the DMA into guest memory.
class Piix3Ide : public PciFunction {
 public:
  using IrqCallback = std::function<void(unsigned irq, bool level)>;
  Piix3Ide(IoBus& io, IrqCallback on_irq, devices::IdeChannel::DmaMemory dma_memory);

  devices::IdeChannel& channel(unsigned index) { return *channels_.at(index); }

 private:
  // BAR4 decoder: offsets 0-7 are the primary channel's registers, 8-15 the secondary's.
  class BusMaster : public PioDevice {
   public:
    explicit BusMaster(std::array<std::shared_ptr<devices::IdeChannel>, 2> channels)
        : channels_(std::move(channels)) {}
    uint32_t pio_read(uint16_t port, unsigned size) override;
    void pio_write(uint16_t port, unsigned size, uint32_t value) override;

   private:
    const std::array<std::shared_ptr<devices::IdeChannel>, 2> channels_;
  };

  std::array<std::shared_ptr<devices::IdeChannel>, 2> channels_;
};

// 82371AB PIIX4 function 3 (power management), 8086:7113, as QEMU pairs it with the PIIX3. Provides the
// ACPI fixed hardware the FADT points at: PM1 event/control, the 3.579545 MHz PM timer and GPE0 in the
// I/O block at PMBA (0x40, enabled by PMREGMISC bit 0), plus the APM control/status ports 0xB2/0xB3.
// SCI is IRQ 9. Writing SLP_EN with SLP_TYP 0 (our \_S5) powers the machine off.
//
// No SMM: ACPI_ENABLE / ACPI_DISABLE written to 0xB2 flip SCI_EN directly (as QEMU does), and any
// write to 0xB2 clears the APM status port, which is what firmware polls for after raising an SMI.
// TMR_STS (timer carry) is never set: no OS we target enables the PM timer overflow interrupt. (Doing
// it right needs a host timer to raise SCI while every vCPU is halted.)
class Piix4Pm : public PciFunction {
 public:
  using LineCallback = std::function<void(bool level)>;
  using EventCallback = std::function<void()>;
  Piix4Pm(IoBus& io, LineCallback on_sci, EventCallback on_power_off);

  // Firmware's view after POST: I/O block at `base`, decoding on. For guests started without firmware.
  void preset_pm_base(uint16_t base);
  // Where the PM I/O block currently decodes (0 if disabled), for building the FADT.
  uint16_t pm_base();
  // A press of the fixed-feature power button: the OS gets an SCI and normally shuts down cleanly.
  void press_power_button();

 protected:
  void after_config_write(unsigned off, unsigned size) override;

 private:
  class Regs : public PioDevice {
   public:
    Regs(LineCallback on_sci, EventCallback on_power_off);
    uint32_t pio_read(uint16_t port, unsigned size) override;
    void pio_write(uint16_t port, unsigned size, uint32_t value) override;
    void set_base(uint16_t base);
    void press_power_button();

   private:
    uint8_t read_byte(unsigned off);
    void write_byte(unsigned off, uint8_t value);
    uint32_t timer() const;
    void update_sci();

    const LineCallback on_sci_;
    const EventCallback on_power_off_;
    const int64_t qpc_start_;
    const int64_t qpc_freq_;
    std::mutex mutex_;
    uint16_t base_ = 0;
    uint16_t pm1_sts_ = 0, pm1_en_ = 0, pm1_cnt_ = 0, gpe_sts_ = 0, gpe_en_ = 0;
    uint8_t apm_cnt_ = 0, apm_sts_ = 0;
    uint32_t timer_sample_ = 0;
    bool sci_ = false;
  };

  void remap();

  IoBus& io_;
  std::shared_ptr<Regs> regs_;
  uint16_t mapped_ = 0;
};

}  // namespace sandvm::pci
