#include "core/devices/ioapic.h"

#include <utility>

namespace sandvm::devices {

IoApic::IoApic(uint8_t id, Deliver deliver) : deliver_(std::move(deliver)), id_(id & 0x0F) {
  for (auto& entry : redirection_) entry = kMasked;  // reset state: all pins masked
}

void IoApic::set_irq(unsigned gsi, bool level) {
  if (gsi >= kPins) return;
  std::lock_guard lock(mutex_);
  const uint32_t bit = 1u << gsi;
  const bool was = (lines_ & bit) != 0;
  lines_ = level ? (lines_ | bit) : (lines_ & ~bit);
  const bool level_triggered = (redirection_[gsi] & kLevel) != 0;
  if (level_triggered) {
    if (level) pending_ |= bit;
    else pending_ &= ~bit;
  } else if (level && !was && !(redirection_[gsi] & kMasked)) {
    pending_ |= bit;  // rising edge; the 82093AA ignores edges on a masked pin
  }
  service(gsi);
}

void IoApic::eoi(uint8_t vector) {
  std::lock_guard lock(mutex_);
  for (unsigned pin = 0; pin < kPins; ++pin) {
    uint64_t& entry = redirection_[pin];
    if ((entry & 0xFF) != vector || !(entry & kRemoteIrr)) continue;
    entry &= ~kRemoteIrr;
    // Still asserted: a level-triggered source that was not serviced raises the interrupt again.
    if (lines_ & (1u << pin)) pending_ |= 1u << pin;
    service(pin);
  }
}

void IoApic::service(unsigned pin) {
  const uint32_t bit = 1u << pin;
  uint64_t& entry = redirection_[pin];
  if (!(pending_ & bit) || (entry & kMasked) || (entry & kRemoteIrr)) return;
  const bool level = (entry & kLevel) != 0;
  const uint8_t mode = static_cast<uint8_t>((entry >> 8) & 7);
  if (!deliver_({static_cast<uint8_t>(entry & 0xFF), mode, ((entry >> 11) & 1) != 0, level,
                 static_cast<uint8_t>(entry >> 56)})) {
    return;  // stays pending
  }
  pending_ &= ~bit;
  // Only fixed and lowest-priority interrupts are acknowledged with an EOI; for NMI/INIT/SMI/ExtINT
  // the 82093AA does not set Remote IRR, or the pin would wait forever.
  if (level && mode <= 1) entry |= kRemoteIrr;
}

uint32_t IoApic::read_register(uint8_t index) const {
  switch (index) {
    case 0x00: return uint32_t{id_} << 24;
    case 0x01: return ((kPins - 1) << 16) | 0x11;  // max redirection entry, version 0x11 (82093AA)
    case 0x02: return uint32_t{id_} << 24;         // arbitration ID
  }
  if (index >= 0x10 && index < 0x10 + 2 * kPins) {
    const uint64_t entry = redirection_[(index - 0x10) / 2];
    return (index & 1) ? static_cast<uint32_t>(entry >> 32) : static_cast<uint32_t>(entry);
  }
  return 0;
}

void IoApic::write_register(uint8_t index, uint32_t value) {
  if (index == 0x00) {
    id_ = (value >> 24) & 0x0F;
    return;
  }
  if (index < 0x10 || index >= 0x10 + 2 * kPins) return;
  const unsigned pin = (index - 0x10) / 2;
  uint64_t& entry = redirection_[pin];
  // Delivery status and Remote IRR are read-only.
  constexpr uint64_t kReadOnly = kDeliveryStatus | kRemoteIrr;
  if (index & 1) {
    entry = (entry & 0x00000000FFFFFFFFull) | (uint64_t{value} << 32);
  } else {
    entry = (entry & (0xFFFFFFFF00000000ull | kReadOnly)) | (value & ~kReadOnly);
    // Switching a pin to edge mode drops a Remote IRR it can no longer clear.
    if (!(entry & kLevel)) entry &= ~kRemoteIrr;
    // A level that is still asserted is pending again when the pin becomes level-triggered or unmasked.
    if ((entry & kLevel) && (lines_ & (1u << pin))) pending_ |= 1u << pin;
  }
  service(pin);
}

uint64_t IoApic::mmio_read(uint64_t gpa, unsigned size) {
  std::lock_guard lock(mutex_);
  const uint64_t off = gpa - kBase;
  if (off == 0x00) return select_;
  if (off == 0x10 && size == 4) return read_register(select_);
  return 0;  // the window is only defined for 32-bit accesses
}

void IoApic::mmio_write(uint64_t gpa, unsigned size, uint64_t value) {
  std::lock_guard lock(mutex_);
  const uint64_t off = gpa - kBase;
  if (off == 0x00) select_ = static_cast<uint8_t>(value);
  else if (off == 0x10 && size == 4) write_register(select_, static_cast<uint32_t>(value));
}

}  // namespace sandvm::devices
