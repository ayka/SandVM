#include "core/pci/pci.h"

#include <stdexcept>
#include <utility>

#include "core/log.h"

namespace sandvm::pci {
namespace {
constexpr unsigned kCommand = 0x04;
constexpr uint16_t kCommandIo = 0x0001;
constexpr uint16_t kCommandMemory = 0x0002;
// 32-bit memory BARs must stay below the IOAPIC, local APIC and firmware ROM at the top of 4 GiB.
constexpr uint64_t kMemBarLimit = 0xFEC00000;
constexpr unsigned kBar0 = 0x10;
constexpr unsigned kInterruptLine = 0x3C;
constexpr unsigned kInterruptPin = 0x3D;
}  // namespace

PciFunction::PciFunction(IoBus& io, uint16_t vendor, uint16_t device, uint8_t revision, uint32_t class_code,
                         bool multifunction)
    : io_(io) {
  set_config(0x00, 2, vendor);
  set_config(0x02, 2, device);
  set_config(0x08, 4, (class_code << 8) | revision);
  set_config(0x0E, 1, multifunction ? 0x80 : 0x00);
  // I/O, memory, bus master, parity/SERR response and INTx disable are the commonly writable command bits.
  set_wmask(kCommand, 2, 0x0547);
}

void PciFunction::set_config(unsigned off, unsigned size, uint32_t value) {
  for (unsigned i = 0; i < size; ++i) config_.at(off + i) = static_cast<uint8_t>(value >> (8 * i));
}

uint32_t PciFunction::get_config(unsigned off, unsigned size) const {
  uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= uint32_t{config_.at(off + i)} << (8 * i);
  return value;
}

void PciFunction::set_wmask(unsigned off, unsigned size, uint32_t mask) {
  for (unsigned i = 0; i < size; ++i) wmask_.at(off + i) = static_cast<uint8_t>(mask >> (8 * i));
}

void PciFunction::set_interrupt_pin(uint8_t pin) {
  set_config(kInterruptPin, 1, pin);
  set_wmask(kInterruptLine, 1, 0xFF);
}

void PciFunction::add_io_bar(unsigned index, uint16_t size, std::shared_ptr<PioDevice> device) {
  if (index >= io_bars_.size() || size < 4 || (size & (size - 1)) || !device) {
    throw std::invalid_argument("add_io_bar: bad index, size or device");
  }
  io_bars_[index] = {size, std::move(device), 0};
  const unsigned off = kBar0 + 4 * index;
  set_config(off, 4, 0x1);  // bit 0 = I/O space indicator, read-only
  set_wmask(off, 4, ~uint32_t{size - 1u} & ~0x3u);
}

void PciFunction::add_mem_bar(unsigned index, uint32_t size, bool prefetchable, MemBarMap on_map) {
  if (index >= mem_bars_.size() || size < 0x1000 || (size & (size - 1)) || !on_map || io_bars_[index].device) {
    throw std::invalid_argument("add_mem_bar: bad index, size or callback");
  }
  mem_bars_[index] = {size, std::move(on_map), 0};
  const unsigned off = kBar0 + 4 * index;
  set_config(off, 4, prefetchable ? 0x8 : 0x0);  // bits 2:1 = 00: 32-bit, anywhere below 4 GiB
  set_wmask(off, 4, ~(size - 1));
}

uint32_t PciFunction::config_read(unsigned off, unsigned size) {
  std::lock_guard lock(mutex_);
  return get_config(off, size);
}

void PciFunction::config_write(unsigned off, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  for (unsigned i = 0; i < size; ++i) {
    const uint8_t mask = wmask_[off + i];
    const uint8_t byte = static_cast<uint8_t>(value >> (8 * i));
    config_[off + i] = static_cast<uint8_t>((config_[off + i] & ~mask) | (byte & mask));
  }
  if (off < kBar0 + 24 && off + size > kCommand) update_bars();
  after_config_write(off, size);
}

void PciFunction::update_bars() {
  const bool io_enabled = (get_config(kCommand, 2) & kCommandIo) != 0;
  std::array<uint32_t, 6> want{};
  for (unsigned i = 0; i < io_bars_.size(); ++i) {
    IoBar& bar = io_bars_[i];
    if (!bar.device) continue;
    const uint32_t addr = get_config(kBar0 + 4 * i, 4) & ~0x3u;
    // A BAR being sized reads back its mask (all upper bits set); software disables decoding first,
    // but never decode such a value regardless. I/O space is 64 KiB.
    const bool decodable = io_enabled && addr != 0 && addr + bar.size <= 0x10000;
    want[i] = decodable ? addr : 0;
  }
  // Two passes, so BARs that swap addresses free their old ranges before claiming new ones.
  for (unsigned i = 0; i < io_bars_.size(); ++i) {
    IoBar& bar = io_bars_[i];
    if (!bar.device || want[i] == bar.mapped_at || !bar.mapped_at) continue;
    io_.unregister_pio(static_cast<uint16_t>(bar.mapped_at), bar.device.get());
    bar.mapped_at = 0;
  }
  for (unsigned i = 0; i < io_bars_.size(); ++i) {
    IoBar& bar = io_bars_[i];
    if (!bar.device || !want[i] || want[i] == bar.mapped_at) continue;
    try {
      io_.register_pio(static_cast<uint16_t>(want[i]), bar.size, bar.device);
      bar.mapped_at = want[i];
    } catch (const std::invalid_argument&) {
      // Overlaps another device. Real hardware would fight on the bus; we leave the BAR undecoded.
      SVM_WARN("pci: I/O BAR%u at 0x%04x overlaps another device, not decoded", i, want[i]);
    }
  }

  const bool mem_enabled = (get_config(kCommand, 2) & kCommandMemory) != 0;
  for (unsigned i = 0; i < mem_bars_.size(); ++i) {
    MemBar& bar = mem_bars_[i];
    if (!bar.on_map) continue;
    const uint32_t addr = get_config(kBar0 + 4 * i, 4) & ~0xFu;
    // The size mask read back during sizing ends at 4 GiB, so the limit check also keeps it undecoded.
    const bool decodable = mem_enabled && addr != 0 && uint64_t{addr} + bar.size <= kMemBarLimit;
    const uint32_t target = decodable ? addr : 0;
    if (target == bar.mapped_at) continue;
    bar.mapped_at = target;
    bar.on_map(target);
  }
}

void PciBus::attach(unsigned devfn, std::shared_ptr<PciFunction> function) {
  if (devfn >= functions_.size() || !function) throw std::invalid_argument("PciBus::attach: bad devfn");
  std::lock_guard lock(mutex_);
  functions_[devfn] = std::move(function);
}

PciFunction* PciBus::target(unsigned* off) {
  uint32_t address;
  {
    std::lock_guard lock(mutex_);
    address = address_;
  }
  if (!(address & 0x80000000u)) return nullptr;  // enable bit clear: no config cycle
  const unsigned bus = (address >> 16) & 0xFF;
  const unsigned devfn = (address >> 8) & 0xFF;
  if (bus != 0) return nullptr;
  *off = address & 0xFC;
  return functions_[devfn].get();
}

uint32_t PciBus::pio_read(uint16_t port, unsigned size) {
  if (port < kConfigAddress + 4) {
    if (port == kConfigAddress + 1 && size == 1) {
      std::lock_guard lock(mutex_);
      return rcr_;
    }
    if (port != kConfigAddress || size != 4) return 0xFFFFFFFF >> (32 - 8 * size);
    std::lock_guard lock(mutex_);
    return address_;
  }
  unsigned off = 0;
  PciFunction* f = target(&off);
  const unsigned byte = port - (kConfigAddress + 4);
  if (!f || (byte & (size - 1))) return 0xFFFFFFFF >> (32 - 8 * size);
  return f->config_read(off + byte, size);
}

void PciBus::pio_write(uint16_t port, unsigned size, uint32_t value) {
  if (port < kConfigAddress + 4) {
    // Only dword writes reach CONFIG_ADDRESS; byte writes to 0xCFB are the PIIX's mechanism #2 enable,
    // which we do not implement (Linux's pci_check_type1 writes it and expects no harm).
    if (port == kConfigAddress + 1 && size == 1) {
      // Reset control register: a 0 -> 1 transition of bit 2 (RST_CPU) resets the system.
      bool reset;
      {
        std::lock_guard lock(mutex_);
        reset = (value & 0x04) && !(rcr_ & 0x04);
        rcr_ = static_cast<uint8_t>(value & 0x0A);  // bit 2 is self-clearing
      }
      if (reset && on_reset_) on_reset_();
      return;
    }
    if (port != kConfigAddress || size != 4) return;
    std::lock_guard lock(mutex_);
    address_ = value & 0x80FFFFFCu;
    return;
  }
  unsigned off = 0;
  PciFunction* f = target(&off);
  const unsigned byte = port - (kConfigAddress + 4);
  if (!f || (byte & (size - 1))) return;
  f->config_write(off + byte, size, value);
}

}  // namespace sandvm::pci
