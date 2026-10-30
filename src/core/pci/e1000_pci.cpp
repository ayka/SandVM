#include "core/pci/e1000_pci.h"

#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace sandvm::pci {

E1000Pci::E1000Pci(IoBus& io, std::shared_ptr<devices::E1000> nic)
    : PciFunction(io, 0x8086, 0x100E, 0x03, 0x020000), io_(io), nic_(std::move(nic)) {
  set_config(0x2C, 2, 0x8086);  // subsystem vendor
  set_config(0x2E, 2, 0x001E);  // subsystem: PRO/1000 MT Desktop
  set_interrupt_pin(1);
  add_mem_bar(0, devices::E1000::kMmioSize, /*prefetchable=*/false, [this](uint64_t base) { map_registers(base); });
  add_io_bar(1, devices::E1000::kIoSize, std::make_shared<IoWindow>(nic_));
}

void E1000Pci::after_config_write(unsigned off, unsigned size) {
  if (off <= 0x04 && off + size > 0x04) nic_->set_bus_master((get_config(0x04, 2) & 0x0004) != 0);
}

void E1000Pci::map_registers(uint64_t base) {
  if (registers_) {
    io_.unregister_mmio(registers_base_, registers_.get());
    registers_.reset();
  }
  if (!base) return;
  auto registers = std::make_shared<Registers>(nic_, base);
  try {
    io_.register_mmio(base, devices::E1000::kMmioSize, registers);
  } catch (const std::invalid_argument&) {
    SVM_WARN("e1000: register BAR at 0x%llx overlaps another device, not decoded", (unsigned long long)base);
    return;
  }
  registers_ = std::move(registers);
  registers_base_ = base;
}

uint64_t E1000Pci::Registers::mmio_read(uint64_t gpa, unsigned size) {
  return nic_->read(static_cast<uint32_t>(gpa - base_), size);
}

void E1000Pci::Registers::mmio_write(uint64_t gpa, unsigned size, uint64_t value) {
  nic_->write(static_cast<uint32_t>(gpa - base_), size, static_cast<uint32_t>(value));
}

// IOADDR (offset 0) selects a register, IODATA (offset 4) reads or writes it (8254x SDM 13.2.1).
uint32_t E1000Pci::IoWindow::pio_read(uint16_t port, unsigned /*size*/) {
  std::lock_guard lock(mutex_);
  const unsigned off = port & 7;
  if (off < 4) return address_ >> (8 * off);
  return nic_->read(address_ & 0x1FFFC, 4) >> (8 * (off - 4));
}

void E1000Pci::IoWindow::pio_write(uint16_t port, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  const unsigned off = port & 7;
  if (off == 0 && size == 4) address_ = value;
  else if (off == 4 && size == 4) nic_->write(address_ & 0x1FFFC, 4, value);
}

}  // namespace sandvm::pci
