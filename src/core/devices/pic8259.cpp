#include "core/devices/pic8259.h"

#include <utility>

#include "core/log.h"

namespace sandvm::devices {
namespace {
constexpr unsigned kCascadeIrq = 2;  // slave INT is wired to master IR2
}  // namespace

Pic8259::Pic8259(IntrCallback on_intr) : on_intr_(std::move(on_intr)) {
  // PIIX ELCR: IRQ0/1/2 (timer, keyboard, cascade) and IRQ8/13 (RTC, FPU) are always edge-triggered.
  chips_[0].elcr_mask = 0xF8;
  chips_[1].elcr_mask = 0xDE;
}

void Pic8259::set_irq(unsigned irq, bool level) {
  if (irq == kCascadeIrq) irq = 9;  // the ISA bus IRQ2 pin is routed to IRQ9 on an AT
  if (irq > 15) return;
  bool rise;
  {
    std::lock_guard lock(mutex_);
    set_line(chips_[irq >> 3], irq & 7, level);
    rise = update();
  }
  if (rise && on_intr_) on_intr_();
}

bool Pic8259::intr_pending() {
  std::lock_guard lock(mutex_);
  return intr_;
}

void Pic8259::log_state() {
  std::lock_guard lock(mutex_);
  SVM_DEBUG("pic: master irr=%02x imr=%02x isr=%02x  slave irr=%02x imr=%02x isr=%02x  intr=%d", chips_[0].irr,
            chips_[0].imr, chips_[0].isr, chips_[1].irr, chips_[1].imr, chips_[1].isr, int(intr_));
}

uint8_t Pic8259::acknowledge() {
  uint8_t vector;
  bool rise;
  {
    std::lock_guard lock(mutex_);
    Chip& master = chips_[0];
    Chip& slave = chips_[1];
    int irq = pending_irq(master, true);
    if (irq < 0) {
      // The request went away between INTR and INTA: the 8259 answers with IR7 and sets no ISR bit.
      vector = static_cast<uint8_t>(master.vector_base + 7);
    } else {
      ack_irq(master, static_cast<unsigned>(irq));
      if (irq == static_cast<int>(kCascadeIrq)) {
        int irq2 = pending_irq(slave, false);
        if (irq2 >= 0) {
          ack_irq(slave, static_cast<unsigned>(irq2));
          vector = static_cast<uint8_t>(slave.vector_base + irq2);
        } else {
          vector = static_cast<uint8_t>(slave.vector_base + 7);
        }
      } else {
        vector = static_cast<uint8_t>(master.vector_base + irq);
      }
    }
    rise = update();
  }
  if (rise && on_intr_) on_intr_();
  return vector;
}

uint32_t Pic8259::pio_read(uint16_t port, unsigned size) {
  uint32_t value = 0;
  bool rise;
  {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < size && i < 4; ++i) value |= uint32_t{read_port(static_cast<uint16_t>(port + i))} << (8 * i);
    rise = update();  // a poll-mode read acknowledges an interrupt
  }
  if (rise && on_intr_) on_intr_();
  return value;
}

void Pic8259::pio_write(uint16_t port, unsigned size, uint32_t value) {
  bool rise;
  {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < size && i < 4; ++i) write_port(static_cast<uint16_t>(port + i), static_cast<uint8_t>(value >> (8 * i)));
    rise = update();
  }
  if (rise && on_intr_) on_intr_();
}

unsigned Pic8259::priority_of(const Chip& c, uint8_t mask) {
  if (mask == 0) return 8;
  unsigned priority = 0;
  while (!(mask & (1u << ((priority + c.priority_add) & 7)))) ++priority;
  return priority;
}

int Pic8259::pending_irq(const Chip& c, bool is_master) const {
  unsigned request = priority_of(c, static_cast<uint8_t>(c.irr & ~c.imr));
  if (request == 8) return -1;
  const unsigned irq = (request + c.priority_add) & 7;
  uint8_t in_service = c.isr;
  // Special mask mode: a masked in-service level no longer inhibits lower priorities.
  if (c.special_mask) in_service &= static_cast<uint8_t>(~c.imr);
  // Special fully nested mode: while a slave interrupt is in service on IR2, the master still lets a
  // new cascade request through (the slave has already ranked it higher). It does not relax IR2's
  // blocking of the master's own lower-priority inputs.
  if (is_master && c.special_fully_nested && irq == kCascadeIrq) {
    in_service &= static_cast<uint8_t>(~(1u << kCascadeIrq));
  }
  if (request < priority_of(c, in_service)) return static_cast<int>(irq);
  return -1;
}

void Pic8259::set_line(Chip& c, unsigned irq, bool level) {
  const uint8_t mask = static_cast<uint8_t>(1u << irq);
  if (c.elcr & mask) {
    if (level) c.irr |= mask;
    else c.irr &= static_cast<uint8_t>(~mask);
  } else if (level && !(c.line & mask)) {
    c.irr |= mask;  // edge mode latches the rising edge; a falling edge does not withdraw it
  }
  if (level) c.line |= mask;
  else c.line &= static_cast<uint8_t>(~mask);
}

void Pic8259::ack_irq(Chip& c, unsigned irq) {
  const uint8_t mask = static_cast<uint8_t>(1u << irq);
  if (c.auto_eoi) {
    if (c.rotate_on_auto_eoi) c.priority_add = static_cast<uint8_t>((irq + 1) & 7);
  } else {
    c.isr |= mask;
  }
  if (!(c.elcr & mask)) c.irr &= static_cast<uint8_t>(~mask);
}

bool Pic8259::update() {
  Chip& master = chips_[0];
  // The cascade input follows the slave INT output as a level, so a second slave request that is
  // already pending when the first one is acknowledged is not lost for want of a new edge.
  if (pending_irq(chips_[1], false) >= 0) master.irr |= 1u << kCascadeIrq;
  else master.irr &= static_cast<uint8_t>(~(1u << kCascadeIrq));

  const bool intr = pending_irq(master, true) >= 0;
  const bool rise = intr && !intr_;
  intr_ = intr;
  return rise;
}

uint8_t Pic8259::poll_read(Chip& c, bool is_master) {
  // OCW3 poll: the read itself acts as INTA. Bit 7 = request present, bits 0-2 = its level.
  int irq = pending_irq(c, is_master);
  if (irq < 0) return 0;
  ack_irq(c, static_cast<unsigned>(irq));
  return static_cast<uint8_t>(0x80 | irq);
}

uint8_t Pic8259::read_port(uint16_t port) {
  if (port == kElcrBase || port == kElcrBase + 1) return chips_[port - kElcrBase].elcr;
  const bool is_master = (port & ~1u) == kMasterBase;
  if (!is_master && (port & ~1u) != kSlaveBase) return 0xFF;
  Chip& c = chips_[is_master ? 0 : 1];
  if (c.poll) {
    c.poll = false;
    return poll_read(c, is_master);
  }
  if ((port & 1) == 0) return c.read_isr ? c.isr : c.irr;
  return c.imr;
}

void Pic8259::write_port(uint16_t port, uint8_t value) {
  if (port == kElcrBase || port == kElcrBase + 1) {
    Chip& c = chips_[port - kElcrBase];
    c.elcr = value & c.elcr_mask;
    c.irr = static_cast<uint8_t>((c.irr & ~c.elcr) | (c.line & c.elcr));
    return;
  }
  const bool is_master = (port & ~1u) == kMasterBase;
  if (!is_master && (port & ~1u) != kSlaveBase) return;
  Chip& c = chips_[is_master ? 0 : 1];
  if ((port & 1) == 0) write_command(c, value);
  else write_data(c, value);
}

void Pic8259::write_command(Chip& c, uint8_t value) {
  if (value & 0x10) {  // ICW1
    // LTIM is ignored on PIIX-based PCs; trigger mode comes from ELCR.
    if (value & 0x08) SVM_DEBUG("8259: ICW1 LTIM set, ignored (ELCR controls trigger mode)");
    // The edge sense latch is reset: an edge line needs a fresh low-to-high transition.
    c.irr = c.line & c.elcr;
    c.imr = 0;
    c.isr = 0;
    c.priority_add = 0;
    c.read_isr = false;
    c.poll = false;
    c.special_mask = false;
    c.auto_eoi = false;
    c.rotate_on_auto_eoi = false;
    c.special_fully_nested = false;
    c.init4 = (value & 0x01) != 0;
    c.single = (value & 0x02) != 0;
    c.init_state = 1;
    return;
  }
  if (value & 0x08) {  // OCW3
    if (value & 0x04) c.poll = true;
    if (value & 0x02) c.read_isr = (value & 0x01) != 0;
    if (value & 0x40) c.special_mask = (value & 0x20) != 0;
    return;
  }
  // OCW2: R / SL / EOI in bits 7..5, level in bits 2..0.
  const unsigned level = value & 7;
  switch (value >> 5) {
    case 0: c.rotate_on_auto_eoi = false; break;
    case 4: c.rotate_on_auto_eoi = true; break;
    case 1:    // non-specific EOI
    case 5: {  // rotate on non-specific EOI
      unsigned priority = priority_of(c, c.isr);
      if (priority == 8) break;
      unsigned irq = (priority + c.priority_add) & 7;
      c.isr &= static_cast<uint8_t>(~(1u << irq));
      if ((value >> 5) == 5) c.priority_add = static_cast<uint8_t>((irq + 1) & 7);
      break;
    }
    case 3: c.isr &= static_cast<uint8_t>(~(1u << level)); break;  // specific EOI
    case 6: c.priority_add = static_cast<uint8_t>((level + 1) & 7); break;  // set lowest priority
    case 7:  // rotate on specific EOI
      c.isr &= static_cast<uint8_t>(~(1u << level));
      c.priority_add = static_cast<uint8_t>((level + 1) & 7);
      break;
    default: break;  // 2: no operation
  }
}

void Pic8259::write_data(Chip& c, uint8_t value) {
  switch (c.init_state) {
    case 0: c.imr = value; break;  // OCW1
    case 1:                        // ICW2: vector base, low three bits come from the IR level
      c.vector_base = value & 0xF8;
      c.init_state = c.single ? (c.init4 ? 3 : 0) : 2;
      break;
    case 2:  // ICW3: the master/slave wiring is fixed on a PC, nothing to configure
      c.init_state = c.init4 ? 3 : 0;
      break;
    case 3:  // ICW4
      c.special_fully_nested = (value & 0x10) != 0;
      c.auto_eoi = (value & 0x02) != 0;
      c.init_state = 0;
      break;
  }
}

}  // namespace sandvm::devices
