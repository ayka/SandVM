#include "core/pci/uhci_pci.h"

#include <utility>

namespace sandvm::pci {

Piix3Usb::Piix3Usb(IoBus& io, std::shared_ptr<devices::Uhci> uhci)
    : PciFunction(io, 0x8086, 0x7020, 0x01, 0x0C0300), uhci_(std::move(uhci)) {
  set_interrupt_pin(4);
  set_config(0x60, 1, 0x10);    // SBRN: USB 1.0
  set_config(0xC0, 2, 0x2000);  // LEGSUP: PIRQ enable, as after reset
  // Read-only and write-one-to-clear bits (USBLEGSUP_RO 0x5040, RWC 0x8F00) stay zero.
  set_wmask(0xC0, 2, 0x20BF);
  add_io_bar(4, devices::Uhci::kIoSize, uhci_);
}

void Piix3Usb::after_config_write(unsigned off, unsigned size) {
  if (off <= 0x04 && off + size > 0x04) uhci_->set_bus_master((get_config(0x04, 2) & 0x0004) != 0);
}

}  // namespace sandvm::pci
