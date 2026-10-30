#include "core/devices/i8042.h"

#include <algorithm>
#include <utility>

#include "core/log.h"

namespace sandvm::devices {
namespace {
constexpr uint8_t kAck = 0xFA;
constexpr uint8_t kResend = 0xFE;
constexpr uint8_t kSelfTestPassed = 0xAA;

// Command byte (CCB) bits.
constexpr uint8_t kCcbKbdIrq = 0x01;
constexpr uint8_t kCcbMouseIrq = 0x02;
constexpr uint8_t kCcbSystemFlag = 0x04;
constexpr uint8_t kCcbKbdDisabled = 0x10;
constexpr uint8_t kCcbMouseDisabled = 0x20;
constexpr uint8_t kCcbTranslate = 0x40;

// Status register bits.
constexpr uint8_t kStatusOutputFull = 0x01;
constexpr uint8_t kStatusCommand = 0x08;
constexpr uint8_t kStatusUnlocked = 0x10;
constexpr uint8_t kStatusMouseData = 0x20;
}  // namespace

I8042::I8042(IrqCallback on_irq, ResetCallback on_reset)
    : on_irq_(std::move(on_irq)), on_reset_(std::move(on_reset)) {}

void I8042::push_keyboard(std::span<const uint8_t> bytes) {
  std::lock_guard lock(mutex_);
  if ((command_byte_ & kCcbKbdDisabled) || !kbd_scanning_) return;
  for (uint8_t b : bytes) push(b, Source::Keyboard);
}

void I8042::push_mouse(int dx, int dy, int dz, unsigned buttons) {
  std::lock_guard lock(mutex_);
  // The counters run whatever the reporting state, so Read Data (0xEB) in remote mode sees the motion.
  // Bounded (in 64-bit, so a huge host delta cannot overflow) for a guest that stopped reading;
  // the excess is dropped rather than reported through the overflow bits, which drivers handle
  // inconsistently (some discard the whole packet).
  auto add = [](int acc, int64_t delta, int limit) {
    return static_cast<int>(std::clamp<int64_t>(acc + delta, -limit, limit));
  };
  mouse_dx_ = add(mouse_dx_, dx, 4096);
  mouse_dy_ = add(mouse_dy_, -int64_t{dy}, 4096);
  mouse_dz_ = add(mouse_dz_, dz, 64);
  if (buttons != mouse_buttons_) {
    mouse_buttons_ = buttons;
    mouse_dirty_ = true;
  }
  flush_mouse();
}

void I8042::flush_mouse() {
  // Stream mode only; in remote mode the guest polls with 0xEB.
  if (!mouse_reporting_ || mouse_remote_ || (command_byte_ & kCcbMouseDisabled)) return;
  if (!mouse_dirty_ && !mouse_dx_ && !mouse_dy_ && !mouse_dz_) return;
  for (const Byte& b : out_) {
    if (b.source == Source::Mouse) return;  // the guest has not taken the previous packet yet
  }
  mouse_packet();
}

void I8042::mouse_packet() {
  // Movement is 9-bit two's complement per packet; the rest stays for the next one.
  const int dx = std::clamp(mouse_dx_, -255, 255);
  const int dy = std::clamp(mouse_dy_, -255, 255);
  mouse_dx_ -= dx;
  mouse_dy_ -= dy;
  uint8_t b0 = static_cast<uint8_t>(0x08 | (mouse_buttons_ & 7));
  if (dx < 0) b0 |= 0x10;
  if (dy < 0) b0 |= 0x20;
  push(b0, Source::Mouse);
  push(static_cast<uint8_t>(dx), Source::Mouse);
  push(static_cast<uint8_t>(dy), Source::Mouse);
  if (mouse_id_ == 3) {
    // IntelliMouse: the whole fourth byte is a signed wheel count.
    const int dz = std::clamp(mouse_dz_, -128, 127);
    mouse_dz_ -= dz;
    push(static_cast<uint8_t>(dz), Source::Mouse);
  } else if (mouse_id_ == 4) {
    // Byte 4: wheel in bits 3:0, button 4 in bit 4, button 5 in bit 5.
    const int dz = std::clamp(mouse_dz_, -8, 7);
    mouse_dz_ -= dz;
    push(static_cast<uint8_t>((dz & 0x0F) | ((mouse_buttons_ & kMouse4) ? 0x10 : 0) |
                              ((mouse_buttons_ & kMouse5) ? 0x20 : 0)),
         Source::Mouse);
  } else {
    mouse_dz_ = 0;  // a plain PS/2 mouse has no wheel
  }
  mouse_dirty_ = false;
}

bool I8042::a20_enabled() {
  std::lock_guard lock(mutex_);
  return (output_port_ & 0x02) || (port_a_ & 0x02);
}

uint32_t I8042::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  uint32_t value = 0;
  for (unsigned i = 0; i < size && i < 4; ++i) value |= uint32_t{read_port(static_cast<uint16_t>(port + i))} << (8 * i);
  return value;
}

void I8042::pio_write(uint16_t port, unsigned size, uint32_t value) {
  bool reset = false;
  {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < size && i < 4; ++i) {
      reset |= write_port(static_cast<uint16_t>(port + i), static_cast<uint8_t>(value >> (8 * i)));
    }
  }
  if (reset) {
    SVM_INFO("i8042: guest requested a CPU reset");
    if (on_reset_) on_reset_();
  }
}

uint8_t I8042::read_port(uint16_t port) {
  switch (port) {
    case kDataPort: {
      if (out_.empty()) return 0;
      const uint8_t value = out_.front().value;
      out_.pop_front();
      // Reading empties the output buffer, so the IRQ line drops even when another byte of the same
      // kind is queued; that byte then raises it again, giving the edge-triggered PIC a new edge.
      if (irq1_) {
        irq1_ = false;
        if (on_irq_) on_irq_(1, false);
      }
      if (irq12_) {
        irq12_ = false;
        if (on_irq_) on_irq_(12, false);
      }
      update_irq();
      // The last byte of a packet is gone: motion collected meanwhile goes out as the next packet.
      flush_mouse();
      return value;
    }
    case kCommandPort: {
      uint8_t status = kStatusUnlocked;
      if (!out_.empty()) {
        status |= kStatusOutputFull;
        if (out_.front().source == Source::Mouse) status |= kStatusMouseData;
      }
      if (command_byte_ & kCcbSystemFlag) status |= 0x04;
      if (last_write_command_) status |= kStatusCommand;
      return status;
    }
    case kPortA: return port_a_;
    default: return 0xFF;
  }
}

bool I8042::write_port(uint16_t port, uint8_t value) {
  switch (port) {
    case kDataPort:
      last_write_command_ = false;
      if (pending_command_) return controller_data(value);
      // Sending a byte to the keyboard enables its interface again, as on real controllers.
      command_byte_ &= ~kCcbKbdDisabled;
      keyboard_command(value);
      update_irq();
      return false;
    case kCommandPort:
      last_write_command_ = true;
      return controller_command(value);
    case kPortA: {
      // Bit 0 is a fast reset on its 0 -> 1 transition and reads back as 0.
      const bool reset = (value & 0x01) && !(port_a_ & 0x01);
      port_a_ = value & ~0x01;
      return reset;
    }
    default: return false;
  }
}

bool I8042::controller_command(uint8_t command) {
  switch (command) {
    case 0x20: push(command_byte_, Source::Controller); break;  // read command byte
    case 0x60:                                                // write command byte
    case 0xD1:                                                // write output port
    case 0xD2:                                                // write keyboard output buffer
    case 0xD3:                                                // write mouse output buffer
    case 0xD4:                                                // write to mouse
      pending_command_ = command;
      break;
    case 0xA7: command_byte_ |= kCcbMouseDisabled; break;
    case 0xA8: command_byte_ &= ~kCcbMouseDisabled; break;
    case 0xA9: push(0x00, Source::Controller); break;  // mouse port test: no error
    case 0xAA:                                         // controller self-test
      command_byte_ |= kCcbSystemFlag;
      push(0x55, Source::Controller);
      break;
    case 0xAB: push(0x00, Source::Controller); break;  // keyboard port test: no error
    case 0xAD: command_byte_ |= kCcbKbdDisabled; break;
    case 0xAE: command_byte_ &= ~kCcbKbdDisabled; break;
    case 0xC0: push(0x80, Source::Controller); break;  // input port: keyboard not inhibited
    case 0xD0: push(output_port_, Source::Controller); break;
    case 0xE0: push(0x00, Source::Controller); break;  // test inputs
    default:
      if (command >= 0xF0) return !(command & 0x01);  // pulse output lines; bit 0 low pulses reset
      SVM_DEBUG("i8042: unknown controller command 0x%02x", command);
      break;
  }
  update_irq();
  return false;
}

bool I8042::controller_data(uint8_t value) {
  const uint8_t command = pending_command_;
  pending_command_ = 0;
  bool reset = false;
  switch (command) {
    case 0x60: command_byte_ = value; break;
    case 0xD1: reset = write_output_port(value); break;
    case 0xD2: push(value, Source::Keyboard); break;
    case 0xD3: push(value, Source::Mouse); break;
    case 0xD4:
      command_byte_ &= ~kCcbMouseDisabled;  // likewise for the mouse interface
      mouse_command(value);
      break;
  }
  update_irq();
  return reset;
}

bool I8042::write_output_port(uint8_t value) {
  output_port_ = value;
  return !(value & 0x01);  // the reset line is active low
}

void I8042::keyboard_command(uint8_t value) {
  if (kbd_pending_) {
    const uint8_t command = kbd_pending_;
    kbd_pending_ = 0;
    if (command == 0xF0) {
      if (value > 3) {
        push(kResend, Source::Keyboard);
        return;
      }
      push(kAck, Source::Keyboard);
      if (value == 0) {
        // With translation on, the controller also translates the reported set number (2 -> 0x41).
        static constexpr uint8_t kTranslated[] = {0, 0x43, 0x41, 0x3F};
        push((command_byte_ & kCcbTranslate) ? kTranslated[kbd_scancode_set_] : kbd_scancode_set_, Source::Keyboard);
      } else {
        kbd_scancode_set_ = value;
      }
      return;
    }
    // 0xED (LEDs) and 0xF3 (typematic rate) just take their parameter.
    push(kAck, Source::Keyboard);
    return;
  }
  switch (value) {
    case 0xED:
    case 0xF0:
    case 0xF3:
      push(kAck, Source::Keyboard);
      kbd_pending_ = value;
      break;
    case 0xEE: push(0xEE, Source::Keyboard); break;  // echo
    case 0xF2:                                        // identify: MF2 keyboard
      push(kAck, Source::Keyboard);
      push(0xAB, Source::Keyboard);
      // With translation on, the controller turns the second ID byte 0x83 into 0x41.
      push((command_byte_ & kCcbTranslate) ? 0x41 : 0x83, Source::Keyboard);
      break;
    case 0xF4:
      kbd_scanning_ = true;
      push(kAck, Source::Keyboard);
      break;
    case 0xF5:  // disable scanning and restore defaults
      kbd_scanning_ = false;
      kbd_scancode_set_ = 2;
      push(kAck, Source::Keyboard);
      break;
    case 0xF6:  // set defaults; unlike 0xF5, scanning stays (or becomes) enabled
      kbd_scanning_ = true;
      kbd_scancode_set_ = 2;
      push(kAck, Source::Keyboard);
      break;
    case 0xFF:  // reset; the self-test result follows the ACK
      kbd_scanning_ = true;
      kbd_scancode_set_ = 2;
      push(kAck, Source::Keyboard);
      push(kSelfTestPassed, Source::Keyboard);
      break;
    default:
      SVM_DEBUG("i8042: unknown keyboard command 0x%02x", value);
      push(kResend, Source::Keyboard);
      break;
  }
}

void I8042::mouse_command(uint8_t value) {
  if (mouse_pending_) {
    if (mouse_pending_ == 0xE8) {
      mouse_resolution_ = value & 3;
    } else {
      mouse_rate_ = value;
      mouse_rate_knock(value);
    }
    mouse_pending_ = 0;
    reset_mouse_counters();
    push(kAck, Source::Mouse);
    return;
  }
  switch (value) {
    case 0xE6: mouse_scaling_21_ = false; push(kAck, Source::Mouse); break;
    case 0xE7: mouse_scaling_21_ = true; push(kAck, Source::Mouse); break;
    case 0xE8:
    case 0xF3:
      mouse_pending_ = value;
      push(kAck, Source::Mouse);
      break;
    case 0xE9: {  // status request: flags, resolution, sample rate
      // Flags: bit 6 remote, 5 reporting, 4 scaling 2:1, then left (2), middle (1), right (0).
      const uint8_t flags = static_cast<uint8_t>(
          (mouse_remote_ ? 0x40 : 0) | (mouse_reporting_ ? 0x20 : 0) | (mouse_scaling_21_ ? 0x10 : 0) |
          ((mouse_buttons_ & kMouseLeft) ? 0x04 : 0) | ((mouse_buttons_ & kMouseMiddle) ? 0x02 : 0) |
          ((mouse_buttons_ & kMouseRight) ? 0x01 : 0));
      push(kAck, Source::Mouse);
      push(flags, Source::Mouse);
      push(mouse_resolution_, Source::Mouse);
      push(mouse_rate_, Source::Mouse);
      reset_mouse_counters();
      break;
    }
    case 0xEB:  // read data: one packet with the current state
      push(kAck, Source::Mouse);
      mouse_packet();
      break;
    case 0xF2:  // identify
      push(kAck, Source::Mouse);
      push(mouse_id_, Source::Mouse);
      break;
    case 0xF4:
    case 0xF5:
      // Both clear the movement counters, so enabling never replays motion from before.
      mouse_reporting_ = value == 0xF4;
      reset_mouse_counters();
      push(kAck, Source::Mouse);
      break;
    case 0xF6:
    case 0xFF:
      mouse_reporting_ = false;
      mouse_remote_ = false;
      mouse_rate_ = 100;
      mouse_resolution_ = 2;
      mouse_scaling_21_ = false;
      reset_mouse_counters();
      push(kAck, Source::Mouse);
      if (value == 0xFF) {
        mouse_id_ = 0;  // reset drops the IntelliMouse extensions; set defaults (0xF6) keeps them
        mouse_knock_ = {};
        push(kSelfTestPassed, Source::Mouse);
        push(0x00, Source::Mouse);  // device ID
      }
      break;
    case 0xEA:  // stream mode
    case 0xF0:  // remote mode: packets only on Read Data
      mouse_remote_ = value == 0xF0;
      reset_mouse_counters();
      push(kAck, Source::Mouse);
      break;
    case 0xEC:  // reset wrap mode
    case 0xEE:  // wrap mode (echo); not modelled, nobody uses it
      push(kAck, Source::Mouse);
      break;
    default:
      SVM_DEBUG("i8042: unknown mouse command 0x%02x", value);
      push(kResend, Source::Mouse);
      break;
  }
}

void I8042::reset_mouse_counters() {
  // The PS/2 mouse clears its movement counters on most commands (stream/remote mode, status request,
  // resolution, sample rate, enable/disable, defaults, reset), so motion from before never leaks out.
  mouse_dx_ = mouse_dy_ = mouse_dz_ = 0;
  mouse_dirty_ = false;
}

void I8042::mouse_rate_knock(uint8_t rate) {
  // Microsoft's IntelliMouse detection: setting the sample rate to 200, 100, 80 turns on the wheel
  // (ID 3); afterwards 200, 200, 80 turns on buttons 4/5 as well (ID 4).
  mouse_knock_ = {mouse_knock_[1], mouse_knock_[2], rate};
  if (mouse_knock_ == std::array<uint8_t, 3>{200, 100, 80} && mouse_id_ == 0) mouse_id_ = 3;
  else if (mouse_knock_ == std::array<uint8_t, 3>{200, 200, 80} && mouse_id_ == 3) mouse_id_ = 4;
}

void I8042::push(uint8_t value, Source source) {
  out_.push_back({value, source});
  update_irq();
}

void I8042::update_irq() {
  // Each byte that reaches the output buffer is a new rising edge for the edge-triggered PIC input:
  // reading port 0x60 drops the line, the next queued byte raises it again.
  // A disabled interface (CCB bits 4/5) holds its device's bytes back from the CPU side, so they
  // raise no interrupt until it is enabled again; controller replies are never held back.
  const bool full = !out_.empty();
  const Source source = full ? out_.front().source : Source::Controller;
  const bool kbd_visible =
      source == Source::Controller || (source == Source::Keyboard && !(command_byte_ & kCcbKbdDisabled));
  const bool mouse_visible = source == Source::Mouse && !(command_byte_ & kCcbMouseDisabled);
  const bool irq1 = full && kbd_visible && (command_byte_ & kCcbKbdIrq);
  const bool irq12 = full && mouse_visible && (command_byte_ & kCcbMouseIrq);
  if (irq1 != irq1_) {
    irq1_ = irq1;
    if (on_irq_) on_irq_(1, irq1);
  }
  if (irq12 != irq12_) {
    irq12_ = irq12;
    if (on_irq_) on_irq_(12, irq12);
  }
}

}  // namespace sandvm::devices
