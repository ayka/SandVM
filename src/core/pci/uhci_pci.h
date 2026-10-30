#pragma once

#include <memory>

#include "core/devices/uhci.h"
#include "core/pci/pci.h"

namespace sandvm::pci {

// PIIX3 USB function (00:01.2): 8086:7020, class 0x0C0300 (UHCI), I/O BAR4 of 32 bytes, INTD#,
// serial bus release 1.0 at 0x60 and the legacy support register at 0xC0 (its SMI write-one-to-clear
// bits read as zero: no SMM here, so there is nothing for firmware to hand over).
class Piix3Usb : public PciFunction {
 public:
  Piix3Usb(IoBus& io, std::shared_ptr<devices::Uhci> uhci);

 protected:
  void after_config_write(unsigned off, unsigned size) override;

 private:
  const std::shared_ptr<devices::Uhci> uhci_;
};

}  // namespace sandvm::pci
