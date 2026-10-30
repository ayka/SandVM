#pragma once

#include <windows.h>
#include <WinHvPlatform.h>

#include <cstdint>
#include <memory>

#include "core/devices/vga.h"
#include "core/pci/pci.h"

namespace sandvm::pci {

// QEMU's standard VGA as a PCI function, 1234:1111, class 0x0300 (VGA-compatible), so SeaBIOS runs
// vgabios-stdvga.bin for it and guests bind their generic VGA / Bochs drivers. BAR0 is the linear
// framebuffer (prefetchable, the size of VRAM), mapped straight into the partition with
// WHvMapGpaRange; the same range is also registered as MMIO for accesses that reach the instruction
// emulator. No expansion ROM BAR: SeaBIOS takes the option ROM from fw_cfg ("pci1234,1111.rom").
//
// The legacy ports and the 0xA0000 window are hard-decoded by the owner (Vm), as on every PC.
// VRAM must outlive the partition's mapping of it: the owner deletes the partition before the Vga.
class StdVga : public PciFunction {
 public:
  static constexpr uint16_t kVendor = 0x1234;
  static constexpr uint16_t kDevice = 0x1111;

  StdVga(IoBus& io, WHV_PARTITION_HANDLE partition, std::shared_ptr<devices::Vga> vga);

  devices::Vga& vga() { return *vga_; }

 private:
  // One object per BAR placement, so an access already dispatched to the old range still subtracts
  // the base it was registered with.
  class Lfb : public MmioDevice {
   public:
    Lfb(std::shared_ptr<devices::Vga> vga, uint64_t base) : vga_(std::move(vga)), base_(base) {}
    uint64_t mmio_read(uint64_t gpa, unsigned size) override { return vga_->lfb_read(gpa - base_, size); }
    void mmio_write(uint64_t gpa, unsigned size, uint64_t value) override {
      vga_->lfb_write(gpa - base_, size, value);
    }

   private:
    const std::shared_ptr<devices::Vga> vga_;
    const uint64_t base_;
  };

  void map_lfb(uint64_t base);

  IoBus& io_;
  const WHV_PARTITION_HANDLE partition_;
  const std::shared_ptr<devices::Vga> vga_;
  std::shared_ptr<Lfb> lfb_;  // the current registration, null when not decoded
  uint64_t base_of_current_ = 0;
  bool hv_mapped_ = false;    // the partition maps VRAM at lfb_'s base
  bool stuck_ = false;        // an unmap failed: the old mapping stays and the BAR is never moved again
};

}  // namespace sandvm::pci
