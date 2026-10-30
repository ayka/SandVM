#include "core/devices/serial16550.h"

#include <utility>

namespace sandvm::devices {
namespace {
constexpr unsigned kRbrThr = 0;  // DLAB=1: divisor low
constexpr unsigned kIer = 1;     // DLAB=1: divisor high
constexpr unsigned kIirFcr = 2;
constexpr unsigned kLcr = 3;
constexpr unsigned kMcr = 4;
constexpr unsigned kLsr = 5;
constexpr unsigned kMsr = 6;
constexpr unsigned kScr = 7;

constexpr uint8_t kLsrDataReady = 0x01;
constexpr uint8_t kLsrThrEmpty = 0x20;
constexpr uint8_t kLsrTxEmpty = 0x40;
constexpr uint8_t kMcrOut2 = 0x08;
constexpr uint8_t kMcrLoopback = 0x10;
}  // namespace

Serial16550::Serial16550(uint16_t base, TxCallback on_tx, IrqCallback on_irq)
    : base_(base), on_tx_(std::move(on_tx)), on_irq_(std::move(on_irq)) {}

void Serial16550::push_rx(uint8_t byte) {
  std::lock_guard lock(mutex_);
  rx_.push_back(byte);
  update_irq();
}

uint8_t Serial16550::pending_iir() const {
  if ((ier_ & 0x01) && !rx_.empty()) return 0x04;  // RX data available outranks THRE
  if ((ier_ & 0x02) && thre_pending_) return 0x02;
  return 0x01;
}

void Serial16550::update_irq() {
  // On a PC the UART INTR pin reaches the PIC only through a buffer enabled by MCR OUT2,
  // which is why DOS, Linux and Windows drivers all set OUT2 before expecting interrupts.
  const bool level = (mcr_ & kMcrOut2) && !(mcr_ & kMcrLoopback) && pending_iir() != 0x01;
  if (level == irq_level_) return;
  irq_level_ = level;
  if (on_irq_) on_irq_(level);
}

uint32_t Serial16550::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  // Wider accesses are split into consecutive byte registers; IoBus keeps them inside our 8 ports.
  uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= uint32_t{read_reg(port - base_ + i)} << (8 * i);
  update_irq();  // RBR and IIR reads can retire the pending source
  return value;
}

void Serial16550::pio_write(uint16_t port, unsigned size, uint32_t value) {
  uint8_t tx[4];
  unsigned tx_count = 0;
  {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < size && i < 4; ++i) {
      if (auto byte = write_reg(port - base_ + i,static_cast<uint8_t>(value >> (8 * i)))) tx[tx_count++] = *byte;
    }
    update_irq();
  }
  // Outside the lock: the backend may block or call back into the device.
  if (on_tx_) {
    for (unsigned i = 0; i < tx_count; ++i) on_tx_(tx[i]);
  }
}

uint8_t Serial16550::read_reg(unsigned reg) {
  switch (reg) {
    case kRbrThr:
      if (dlab()) return static_cast<uint8_t>(divisor_);
      if (rx_.empty()) return 0;
      {
        uint8_t byte = rx_.front();
        rx_.pop_front();
        return byte;
      }
    case kIer:
      return dlab() ? static_cast<uint8_t>(divisor_ >> 8) : ier_;
    case kIirFcr: {
      uint8_t iir = pending_iir();
      if (iir == 0x02) thre_pending_ = false;  // reading IIR acknowledges THRE when it is the reported source
      if (fcr_ & 0x01) iir |= 0xC0;  // FIFOs enabled
      return iir;
    }
    case kLcr: return lcr_;
    case kMcr: return mcr_;
    case kLsr: return kLsrThrEmpty | kLsrTxEmpty | (rx_.empty() ? 0 : kLsrDataReady);
    case kMsr:
      // Loopback: OUT2->DCD, OUT1->RI, DTR->DSR, RTS->CTS. Otherwise report DCD|DSR|CTS.
      if (mcr_ & kMcrLoopback) {
        return static_cast<uint8_t>(((mcr_ & 0x0C) << 4) | ((mcr_ & 0x01) << 5) | ((mcr_ & 0x02) << 3));
      }
      return 0xB0;
    case kScr: return scr_;
  }
  return 0xFF;
}

std::optional<uint8_t> Serial16550::write_reg(unsigned reg, uint8_t value) {
  switch (reg) {
    case kRbrThr:
      if (dlab()) {
        divisor_ = static_cast<uint16_t>((divisor_ & 0xFF00) | value);
        break;
      }
      // Transmission is instantaneous, so THR is empty again right after the write.
      thre_pending_ = true;
      if (mcr_ & kMcrLoopback) {
        rx_.push_back(value);
        break;
      }
      return value;
    case kIer:
      if (dlab()) {
        divisor_ = static_cast<uint16_t>((divisor_ & 0x00FF) | (value << 8));
      } else {
        // Enabling ETBEI while THR is empty raises THRE immediately (16550 behavior).
        if ((value & 0x02) && !(ier_ & 0x02)) thre_pending_ = true;
        ier_ = value & 0x0F;
      }
      break;
    case kIirFcr:
      fcr_ = value;
      if (value & 0x02) rx_.clear();
      break;
    case kLcr: lcr_ = value; break;
    case kMcr: mcr_ = value & 0x1F; break;
    case kScr: scr_ = value; break;
    default: break;  // LSR/MSR are read-only
  }
  return std::nullopt;
}

}  // namespace sandvm::devices
