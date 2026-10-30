#pragma once

#include <cstdint>
#include <functional>
#include <mutex>

#include "core/io_bus.h"

namespace sandvm::devices {

// Cascaded pair of Intel 8259A PICs as wired on a PC/AT: slave INT -> master IR2,
// master at 0x20/0x21, slave at 0xA0/0xA1, PIIX edge/level control (ELCR) at 0x4D0/0x4D1.
// Thread-safe: set_irq() may be called from any thread. Lock order: a device may call
// set_irq() while holding its own lock; the PIC never calls into devices.
class Pic8259 : public PioDevice {
 public:
  static constexpr uint16_t kMasterBase = 0x20;
  static constexpr uint16_t kSlaveBase = 0xA0;
  static constexpr uint16_t kElcrBase = 0x4D0;

  // Called (without the PIC lock held) when the INTR output goes from inactive to active,
  // so the CPU side can wake a halted vCPU or kick a running one.
  using IntrCallback = std::function<void()>;

  explicit Pic8259(IntrCallback on_intr);

  // Drives ISA IRQ line 0..15. Edge-triggered lines latch on a rising edge;
  // lines marked level-triggered in ELCR follow `level`.
  void set_irq(unsigned irq, bool level);

  // True if INTR is asserted (an unmasked request outranks everything in service).
  bool intr_pending();
  // INTA cycle: returns the vector for the highest priority request and moves it to
  // in-service. With nothing pending this is a spurious IRQ7 (master), like real hardware.
  uint8_t acknowledge();
  // Logs IRR/IMR/ISR of both chips at debug level.
  void log_state();

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  struct Chip {
    uint8_t irr = 0;       // interrupt request register
    uint8_t imr = 0;       // interrupt mask register
    uint8_t isr = 0;       // in-service register
    uint8_t line = 0;      // current input line levels, for edge detection
    uint8_t elcr = 0;      // 1 = level-triggered (PIIX ELCR, not part of the 8259 itself)
    uint8_t elcr_mask = 0;
    uint8_t priority_add = 0;  // rotation: IR(priority_add) has the highest priority
    uint8_t vector_base = 0;
    uint8_t init_state = 0;  // 0 = ready, 1..3 = expecting ICW2..ICW4
    bool init4 = false;
    bool single = false;
    bool read_isr = false;
    bool poll = false;
    bool special_mask = false;
    bool auto_eoi = false;
    bool rotate_on_auto_eoi = false;
    bool special_fully_nested = false;
  };

  static unsigned priority_of(const Chip& c, uint8_t mask);
  // Highest-priority IR that would be presented on INT, or -1.
  int pending_irq(const Chip& c, bool is_master) const;
  void set_line(Chip& c, unsigned irq, bool level);
  void ack_irq(Chip& c, unsigned irq);
  // Recomputes the cascade input and INTR output; returns true on a rising INTR edge.
  // Callers report that edge through on_intr_ after dropping the lock.
  bool update();
  uint8_t poll_read(Chip& c, bool is_master);
  uint8_t read_port(uint16_t port);
  void write_port(uint16_t port, uint8_t value);
  void write_command(Chip& c, uint8_t value);
  void write_data(Chip& c, uint8_t value);

  const IntrCallback on_intr_;
  std::mutex mutex_;
  Chip chips_[2];  // [0] master, [1] slave
  bool intr_ = false;
};

}  // namespace sandvm::devices
