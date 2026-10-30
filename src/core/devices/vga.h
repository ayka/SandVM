#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "core/io_bus.h"

namespace sandvm::devices {

// Standard VGA plus the Bochs VBE "DISPI" extension, the programming model of QEMU's `-vga std`, so
// that SeaBIOS's vgabios-stdvga.bin drives it and guests find a plain VGA-compatible adapter.
//
//  - Ports 0x3B0-0x3DF: sequencer, graphics controller, CRTC (mono or colour address per misc output
//    bit 0), attribute controller, DAC. Ports 0x1CE/0x1CF (0x1D0 alias): DISPI index / data.
//  - 0xA0000-0xBFFFF: the legacy memory window with the full planar access logic (write modes 0-3,
//    read modes 0-1, set/reset, rotate, logical ops, bit mask, odd/even, chain 4). With VBE enabled it
//    is a 64 KiB bank window into VRAM (DISPI bank register).
//  - The linear framebuffer is VRAM itself; whoever owns the PCI BAR maps vram() into the guest.
//
// VRAM layout follows QEMU: planar byte (plane p, offset o) lives at vram[o * 4 + p], so chain-4 and
// unchained 256-colour modes see a linear array, and odd/even text mode keeps character and attribute
// of cell i at vram[i * 4] and vram[i * 4 + 1].
//
// Thread-safe: register, window and emulated LFB accesses take the device lock, and render() holds it
// while it reads VRAM. Guest stores through the LFB the partition maps cannot be locked; render() may
// read a frame half old, half new (tearing), nothing worse.
class Vga : public PioDevice, public MmioDevice {
 public:
  static constexpr uint16_t kVgaPortBase = 0x3B0;
  static constexpr uint16_t kVgaPortCount = 0x30;
  static constexpr uint16_t kVbePortBase = 0x1CE;
  static constexpr uint16_t kVbePortCount = 3;
  static constexpr uint64_t kWindowBase = 0xA0000;
  static constexpr uint64_t kWindowSize = 0x20000;

  // vram_bytes: power of two, at least 256 KiB.
  explicit Vga(size_t vram_bytes);
  ~Vga() override;
  Vga(const Vga&) = delete;
  Vga& operator=(const Vga&) = delete;

  uint8_t* vram() { return vram_; }
  size_t vram_size() const { return vram_size_; }

  // The legacy ports and the 0xA0000 window.
  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;
  uint64_t mmio_read(uint64_t gpa, unsigned size) override;
  void mmio_write(uint64_t gpa, unsigned size, uint64_t value) override;

  // Accesses to the linear framebuffer that reach the emulator instead of the mapped VRAM (e.g. an
  // instruction that also touched MMIO). offset is relative to the BAR.
  uint64_t lfb_read(uint64_t offset, unsigned size);
  void lfb_write(uint64_t offset, unsigned size, uint64_t value);

  // One displayed frame, 0x00RRGGBB per pixel. Text and planar modes come out in dot-clock resolution
  // (80x25 text: 720x400, mode 13h: 640x400), VBE modes in their own resolution.
  struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> pixels;
  };
  void render(Frame* frame);

 private:
  uint8_t window_read(uint32_t addr);  // addr relative to 0xA0000
  void window_write(uint32_t addr, uint8_t value);
  uint8_t port_read(uint16_t port);
  void port_write(uint16_t port, uint8_t value);
  uint16_t vbe_read();
  void vbe_write(uint16_t value);
  void vbe_fixup();
  void vbe_update_vga_regs();
  bool port_ignored(uint16_t port) const;
  bool vbe_enabled() const;

  uint32_t dac_rgb(unsigned index) const;
  uint32_t attribute_color(unsigned index) const;  // 4-bit pixel -> attribute palette -> DAC
  void render_text(Frame* frame);
  void render_graphics(Frame* frame);
  void render_vbe(Frame* frame);

  std::mutex mutex_;
  uint8_t* vram_ = nullptr;
  size_t vram_size_ = 0;

  uint8_t misc_ = 0;
  uint8_t feature_ = 0;
  uint8_t status1_ = 0;  // input status 1; retrace and display enable toggle on every read
  uint8_t seq_index_ = 0;
  std::array<uint8_t, 8> seq_{};
  uint8_t gc_index_ = 0;
  std::array<uint8_t, 16> gc_{};
  uint8_t crtc_index_ = 0;
  std::array<uint8_t, 0x19> crtc_{};
  uint8_t attr_index_ = 0;
  bool attr_flip_flop_ = false;  // false: next 0x3C0 write is an index
  std::array<uint8_t, 0x15> attr_{};
  uint8_t dac_mask_ = 0xFF;
  uint8_t dac_read_index_ = 0;
  uint8_t dac_write_index_ = 0;
  uint8_t dac_sub_index_ = 0;
  uint8_t dac_state_ = 0;
  std::array<uint8_t, 3> dac_cache_{};
  std::array<uint8_t, 768> palette_{};
  uint32_t latch_ = 0;

  uint16_t vbe_index_ = 0;
  std::array<uint16_t, 11> vbe_{};
  uint32_t vbe_start_ = 0;        // bytes
  uint32_t vbe_line_offset_ = 0;  // bytes
  uint32_t bank_offset_ = 0;      // bytes added to window addresses in memory map mode 1
  bool dac_8bit_ = false;

  uint32_t frame_count_ = 0;  // drives cursor and character blink
  uint32_t logged_mode_ = ~0u;  // last display mode written to the debug log
};

}  // namespace sandvm::devices
