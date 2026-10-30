#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>

#include "core/io_bus.h"

namespace sandvm::devices {

// Minimal 16550A UART: register file, divisor latch, RX queue, TX callback and IRQ output.
// Thread-safe: push_rx() may be called from a host input thread while a vCPU accesses the ports.
class Serial16550 : public PioDevice {
 public:
  static constexpr uint16_t kCom1Base = 0x3F8;
  static constexpr uint16_t kPortCount = 8;

  // Called without the device lock held.
  using TxCallback = std::function<void(uint8_t)>;
  // Drives the IRQ line; called with the device lock held, only when the level changes.
  using IrqCallback = std::function<void(bool level)>;

  explicit Serial16550(uint16_t base, TxCallback on_tx, IrqCallback on_irq = {});

  void push_rx(uint8_t byte);  // host -> guest

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  bool dlab() const { return (lcr_ & 0x80) != 0; }
  uint8_t read_reg(unsigned reg);
  // Returns a byte to transmit, if the write was a THR write outside loopback.
  std::optional<uint8_t> write_reg(unsigned reg, uint8_t value);
  // Pending interrupt source as reported by IIR bits 3..0 (0x01 = none), without side effects.
  uint8_t pending_iir() const;
  void update_irq();

  const uint16_t base_;
  const TxCallback on_tx_;
  const IrqCallback on_irq_;

  std::mutex mutex_;
  std::deque<uint8_t> rx_;
  uint8_t ier_ = 0;
  uint8_t fcr_ = 0;
  uint8_t lcr_ = 0x03;  // 8N1
  uint8_t mcr_ = 0;
  uint8_t scr_ = 0;
  uint16_t divisor_ = 0x000C;  // 9600 baud
  // THR-empty interrupt latch: set when THR becomes empty (we transmit instantly) or when
  // the guest enables ETBEI; cleared by a THR write or by reading IIR while it is the source.
  bool thre_pending_ = false;
  bool irq_level_ = false;
};

}  // namespace sandvm::devices
