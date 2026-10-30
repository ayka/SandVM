#pragma once

#include <cstdint>
#include <functional>
#include <utility>

#include "core/io_bus.h"

namespace sandvm::devices {

// Bochs/QEMU debug console: every byte written to the port is firmware log output.
// SeaBIOS builds shipped with QEMU (CONFIG_DEBUG_IO) log here instead of to a UART.
// Stateless, so no locking; the callback must be safe to call from a vCPU thread.
class DebugCon : public PioDevice {
 public:
  static constexpr uint16_t kPort = 0x402;
  static constexpr uint8_t kReadback = 0xE9;  // lets firmware detect the port, as on QEMU

  using TxCallback = std::function<void(uint8_t)>;

  explicit DebugCon(TxCallback on_tx) : on_tx_(std::move(on_tx)) {}

  uint32_t pio_read(uint16_t, unsigned) override { return kReadback; }
  void pio_write(uint16_t, unsigned size, uint32_t value) override {
    for (unsigned i = 0; i < size && i < 4; ++i) on_tx_(static_cast<uint8_t>(value >> (8 * i)));
  }

 private:
  const TxCallback on_tx_;
};

}  // namespace sandvm::devices
