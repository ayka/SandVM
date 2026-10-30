#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <span>

#include "core/io_bus.h"

namespace sandvm::devices {

// Intel 8042 keyboard controller with a PS/2 keyboard on the first port and a PS/2 mouse on the
// auxiliary port, plus System Control Port A (0x92: fast A20 and fast reset).
//
// The controller answers instantly, so IBF never shows as set. Bytes from the controller, the
// keyboard and the mouse share one output queue; the head of the queue is the output buffer.
// IRQ1 / IRQ12 follow "output buffer full with keyboard / mouse data, that interface enabled and that
// interrupt enabled in the command byte".
//
// The mouse streams standard 3-byte packets and becomes an IntelliMouse (ID 3, wheel) or IntelliMouse
// Explorer (ID 4, wheel + buttons 4/5) after the usual sample-rate knocks (200,100,80 / 200,200,80).
// Host motion is accumulated and sent one packet at a time, only while no mouse byte is still waiting
// in the output queue, so a guest that reads slowly gets fewer, larger packets instead of a backlog.
// Resolution and 2:1 scaling are stored but not applied (host counts go out 1:1). Wrap mode is not
// modelled.
//
// Known gap: push_keyboard() takes translated set-1 bytes only, so a guest that turns translation off
// (Linux i8042.direct=1) would get the wrong codes. A20 is tracked but not enforced: WHPX has no A20
// mask, so addresses above 1 MiB never wrap even with A20 "disabled".
// Thread-safe. Lock order: the IRQ callback is called with the device lock held (device -> PIC).
class I8042 : public PioDevice {
 public:
  static constexpr uint16_t kDataPort = 0x60;
  static constexpr uint16_t kCommandPort = 0x64;
  static constexpr uint16_t kPortA = 0x92;

  using IrqCallback = std::function<void(unsigned irq, bool level)>;
  // Guest pulsed the CPU reset line (controller command 0xFE, output port bit 0, port 0x92 bit 0).
  // Called without the device lock held.
  using ResetCallback = std::function<void()>;

  I8042(IrqCallback on_irq, ResetCallback on_reset);

  // Host keyboard input: scancode set 1 bytes as the guest sees them with translation enabled
  // (e.g. 0x1E, 0x9E for 'A' down/up). Dropped while the keyboard is disabled.
  void push_keyboard(std::span<const uint8_t> bytes);

  // Host mouse input, relative: dx right, dy down (screen direction), dz wheel notches (positive =
  // towards the user, i.e. scroll down). buttons: bit 0 left, 1 right, 2 middle, 3 button 4, 4 button 5.
  // Dropped while the guest has not enabled reporting.
  static constexpr unsigned kMouseLeft = 1, kMouseRight = 2, kMouseMiddle = 4, kMouse4 = 8, kMouse5 = 16;
  void push_mouse(int dx, int dy, int dz, unsigned buttons);

  bool a20_enabled();

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  enum class Source : uint8_t { Controller, Keyboard, Mouse };
  struct Byte {
    uint8_t value;
    Source source;
  };

  uint8_t read_port(uint16_t port);
  // Return true if the guest asked for a CPU reset.
  bool write_port(uint16_t port, uint8_t value);
  bool controller_command(uint8_t command);
  bool controller_data(uint8_t value);
  void keyboard_command(uint8_t value);
  void mouse_command(uint8_t value);
  void mouse_rate_knock(uint8_t rate);
  void reset_mouse_counters();
  void flush_mouse();  // queues a packet if streaming, there is news, and none is queued yet
  void mouse_packet();
  bool write_output_port(uint8_t value);

  void push(uint8_t value, Source source);
  void update_irq();

  const IrqCallback on_irq_;
  const ResetCallback on_reset_;

  std::mutex mutex_;
  std::deque<Byte> out_;
  uint8_t command_byte_ = 0x03;    // keyboard and mouse interrupts enabled, as QEMU powers up
  uint8_t pending_command_ = 0;    // controller command waiting for its data byte on port 0x60
  bool last_write_command_ = true;  // status bit 3: last write went to 0x64
  uint8_t output_port_ = 0x03;     // bit 0 = reset line (active low), bit 1 = A20 gate
  uint8_t port_a_ = 0;             // 0x92: bit 1 = A20
  bool irq1_ = false;
  bool irq12_ = false;

  // Keyboard device state.
  uint8_t kbd_pending_ = 0;  // command waiting for its parameter (0xED, 0xF0, 0xF3)
  bool kbd_scanning_ = true;
  uint8_t kbd_scancode_set_ = 2;

  // Mouse device state.
  uint8_t mouse_pending_ = 0;  // command waiting for its parameter (0xE8, 0xF3)
  bool mouse_reporting_ = false;
  bool mouse_remote_ = false;  // remote mode (0xF0): no streaming
  uint8_t mouse_rate_ = 100;
  uint8_t mouse_resolution_ = 2;
  bool mouse_scaling_21_ = false;
  uint8_t mouse_id_ = 0;              // 0 = PS/2, 3 = IntelliMouse, 4 = IntelliMouse Explorer
  std::array<uint8_t, 3> mouse_knock_{};  // last three sample rates set
  int mouse_dx_ = 0;                  // motion not sent yet
  int mouse_dy_ = 0;                  // PS/2 direction: positive = up
  int mouse_dz_ = 0;
  unsigned mouse_buttons_ = 0;
  bool mouse_dirty_ = false;          // buttons changed since the last packet
};

}  // namespace sandvm::devices
