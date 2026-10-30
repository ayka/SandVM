#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "core/io_bus.h"

namespace sandvm::devices {

// Intel 8254 PIT at 0x40-0x43 plus the PC/AT "port B" (0x61) bits that belong to it:
// channel 2 gate, speaker data enable, refresh toggle and OUT2 status.
//
// Counters are not ticked; their value is derived from QueryPerformanceCounter at read time.
// A host thread turns channel 0 OUT rising edges into IRQ0 pulses. If the host falls behind,
// missed periods are coalesced into one pulse (guests that count ticks resync from the counter).
// Binary counting only: BCD mode is accepted but counted in binary.
class Pit8254 : public PioDevice {
 public:
  static constexpr uint16_t kBase = 0x40;
  static constexpr uint16_t kPortCount = 4;
  static constexpr uint16_t kPortB = 0x61;
  static constexpr uint64_t kFrequencyHz = 1193182;

  // Drives IRQ0. Called from the timer thread or a vCPU thread, never with the PIT lock held.
  using IrqCallback = std::function<void(bool level)>;

  explicit Pit8254(IrqCallback on_irq0);
  ~Pit8254() override;
  Pit8254(const Pit8254&) = delete;
  Pit8254& operator=(const Pit8254&) = delete;

  // Stops the timer thread; no IRQ callback runs after this returns. Idempotent.
  void stop();

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  struct Channel {
    uint32_t count = 0x10000;  // reload value; a written 0 means 65536
    uint8_t mode = 0;
    uint8_t rw = 3;  // 1 = LSB only, 2 = MSB only, 3 = LSB then MSB
    bool bcd = false;
    bool gate = true;
    bool loaded = false;  // a full count has been written since the control word
    bool armed = false;   // counting; modes 1 and 5 additionally need a gate trigger
    bool null_count = true;
    // Elapsed ticks = base_ticks + ticks since `start` while running. Only modes 0 and 4 stop running,
    // when GATE goes low; base_ticks then holds the ticks counted so far.
    uint64_t start = 0;  // QPC value when counting (re)started or resumed
    uint64_t base_ticks = 0;
    bool running = true;
    uint8_t write_lsb = 0;
    bool write_msb_next = false;
    bool read_msb_next = false;
    uint16_t latched_count = 0;
    uint8_t latch_remaining = 0;  // bytes of latched_count still to be read
    bool status_latched = false;
    uint8_t status = 0;
  };

  uint64_t now() const;
  uint64_t elapsed_ticks(const Channel& c, uint64_t now) const;
  uint16_t current_count(const Channel& c, uint64_t now) const;
  bool output(const Channel& c, uint64_t now) const;
  // PIT ticks after `start` of the first OUT rising edge later than `ticks`, if any.
  static std::optional<uint64_t> next_rising_edge(const Channel& c, uint64_t ticks);

  uint8_t read_port(uint16_t port, uint64_t now);
  // Return true if channel 0 was reprogrammed and the timer thread must recompute its deadline.
  bool write_port(uint16_t port, uint8_t value, uint64_t now);
  bool write_control(uint8_t value, uint64_t now);
  bool write_counter(unsigned index, uint8_t value, uint64_t now);
  void load_count(Channel& c, uint32_t value, uint64_t now);
  void latch_count(Channel& c, uint64_t now);
  void latch_status(Channel& c, uint64_t now);
  void rearm_irq0(uint64_t now);

  void timer_thread();

  const IrqCallback on_irq0_;
  uint64_t qpc_frequency_ = 0;
  HANDLE timer_ = nullptr;
  HANDLE wake_ = nullptr;  // auto-reset: channel 0 changed, or stop()

  std::mutex mutex_;
  Channel channels_[3];
  uint8_t port_b_ = 0;  // writable bits 0-3 of port 0x61
  std::optional<uint64_t> next_irq0_edge_;  // in channel 0 ticks after its start
  bool quit_ = false;
  bool bcd_warned_ = false;

  std::thread thread_;
};

}  // namespace sandvm::devices
