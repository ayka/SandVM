#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

#include "core/io_bus.h"

namespace sandvm::devices {

// MC146818-compatible RTC and 128-byte CMOS RAM at the standard PC ports. Like every PC RTC it runs
// from 32.768 kHz only: divider select 010b is "running"; the MC146818's 4.19 / 1.05 MHz time bases
// (000b / 001b) do not exist on PCs and stop the clock here as other values do.
// The clock always shows host local time: times a guest writes under SET are not kept (known gap).
//
// Interrupts (IRQ8): periodic (status A rate select, status B PIE), update-ended once per host second
// (UIE) and alarm (AIE, with the 0xC0 "don't care" bytes). Their flags in status C are set whether or
// not enabled, IRQF when one of them is enabled; IRQ8 stays high until the guest reads status C, which
// is how every driver acknowledges it. A periodic interrupt the host was too late for is coalesced into
// one, like the PIT's. SeaBIOS's INT 15h AH=86h wait and the Windows ACPI HAL's clock depend on them.
//
// Thread-safe. A host thread wakes at the next periodic edge or second boundary while the matching
// interrupt is enabled. The IRQ callback is called with the device lock held (device -> PIC order).
class CmosRtc : public PioDevice {
 public:
  static constexpr uint16_t kIndexPort = 0x70;
  static constexpr uint16_t kDataPort = 0x71;
  static constexpr uint16_t kPortCount = 2;

  using IrqCallback = std::function<void(bool level)>;
  CmosRtc(uint64_t ram_below_4g, uint64_t ram_above_4g, IrqCallback on_irq = {});
  ~CmosRtc() override;
  CmosRtc(const CmosRtc&) = delete;
  CmosRtc& operator=(const CmosRtc&) = delete;

  // Stops the timer thread; no IRQ callback runs from it after this returns. Idempotent.
  void stop();

  bool nmi_masked() const;
  // Firmware settings in CMOS RAM (e.g. the boot order bytes SeaBIOS reads). Index 0x0E..0x7F.
  void set_nvram(uint8_t index, uint8_t value);

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  uint8_t read_register_locked(uint8_t index);
  void write_register_locked(uint8_t index, uint8_t value);
  uint8_t encode_locked(unsigned value) const;
  // Brings the status C flags up to `now` (QPC) and drives IRQ8 accordingly.
  void update_locked(uint64_t now);
  uint64_t periodic_ticks_locked() const;  // QPC ticks per periodic interrupt, 0 = off
  bool alarm_matches_locked();
  void timer_thread();

  const IrqCallback on_irq_;
  uint64_t qpc_frequency_ = 0;
  uint64_t periodic_start_ = 0;  // QPC when the periodic rate was last (re)programmed
  uint64_t periodic_seen_ = 0;   // periods elapsed since periodic_start_ already turned into PF
  uint64_t second_seen_ = 0;     // host time in whole seconds already turned into UF / AF
  bool irq_line_ = false;
  bool quit_ = false;
  HANDLE timer_ = nullptr;
  HANDLE wake_ = nullptr;  // auto-reset: interrupt setup changed, or stop()
  std::thread thread_;

  mutable std::mutex mutex_;
  std::array<uint8_t, 128> cmos_{};
  uint8_t index_ = 0;
  bool nmi_masked_ = false;
};

}  // namespace sandvm::devices
