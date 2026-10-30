#include "core/devices/vga.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "core/log.h"

namespace sandvm::devices {
namespace {
// DISPI register indices and ENABLE bits (Bochs vbe.h, as QEMU implements them).
constexpr uint16_t kVbeId = 0;
constexpr uint16_t kVbeXres = 1;
constexpr uint16_t kVbeYres = 2;
constexpr uint16_t kVbeBpp = 3;
constexpr uint16_t kVbeEnable = 4;
constexpr uint16_t kVbeBank = 5;
constexpr uint16_t kVbeVirtWidth = 6;
constexpr uint16_t kVbeVirtHeight = 7;
constexpr uint16_t kVbeXOffset = 8;
constexpr uint16_t kVbeYOffset = 9;
constexpr uint16_t kVbeVideoMemory64k = 0xA;
constexpr uint16_t kVbeRegCount = 0xA;  // 0..9 are stored; 0xA is computed

constexpr uint16_t kVbeEnabled = 0x01;
constexpr uint16_t kVbeGetCaps = 0x02;
constexpr uint16_t kVbe8BitDac = 0x20;
constexpr uint16_t kVbeNoClearMem = 0x80;

constexpr uint16_t kVbeId0 = 0xB0C0;
constexpr uint16_t kVbeId5 = 0xB0C5;
constexpr uint16_t kVbeMaxXres = 2560;
constexpr uint16_t kVbeMaxYres = 1600;
constexpr uint16_t kVbeMaxBpp = 32;

constexpr unsigned kMaxDisplay = 4096;  // larger CRTC timings are garbage, not a mode

// Writable bits per register, from the VGA register reference (QEMU uses the same masks).
constexpr uint8_t kSeqMask[8] = {0x03, 0x3D, 0x0F, 0x3F, 0x0E, 0x00, 0x00, 0xFF};
constexpr uint8_t kGcMask[16] = {0x0F, 0x0F, 0x0F, 0x1F, 0x03, 0x7B, 0x0F, 0x0F, 0xFF, 0, 0, 0, 0, 0, 0, 0};

// Expands a 4-bit plane mask to one 0xFF byte per selected plane.
constexpr uint32_t expand_planes(unsigned mask) {
  uint32_t v = 0;
  for (unsigned p = 0; p < 4; ++p) {
    if (mask & (1u << p)) v |= 0xFFu << (8 * p);
  }
  return v;
}

uint8_t plane_byte(uint32_t dword, unsigned plane) { return static_cast<uint8_t>(dword >> (8 * plane)); }

uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t{r} << 16) | (uint32_t{g} << 8) | b; }
}  // namespace

Vga::Vga(size_t vram_bytes) {
  if (vram_bytes < (256u << 10) || (vram_bytes & (vram_bytes - 1)) || vram_bytes > (256u << 20)) {
    throw std::invalid_argument("Vga: VRAM size must be a power of two between 256 KiB and 256 MiB");
  }
  // Page-granular and zeroed, because the PCI BAR maps it straight into the guest.
  vram_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, vram_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (!vram_) throw std::runtime_error("VirtualAlloc failed for VRAM");
  vram_size_ = vram_bytes;
  vbe_[kVbeId] = kVbeId5;
}

Vga::~Vga() { VirtualFree(vram_, 0, MEM_RELEASE); }

bool Vga::vbe_enabled() const { return (vbe_[kVbeEnable] & kVbeEnabled) != 0; }

// --- Legacy memory window -----------------------------------------------------------------------

uint64_t Vga::mmio_read(uint64_t gpa, unsigned size) {
  std::lock_guard lock(mutex_);
  uint64_t value = 0;
  for (unsigned i = 0; i < size; ++i) {
    value |= uint64_t{window_read(static_cast<uint32_t>(gpa - kWindowBase + i))} << (8 * i);
  }
  return value;
}

void Vga::mmio_write(uint64_t gpa, unsigned size, uint64_t value) {
  std::lock_guard lock(mutex_);
  for (unsigned i = 0; i < size; ++i) {
    window_write(static_cast<uint32_t>(gpa - kWindowBase + i), static_cast<uint8_t>(value >> (8 * i)));
  }
}

uint8_t Vga::window_read(uint32_t addr) {
  // Graphics controller misc register bits 3:2 select which part of 0xA0000-0xBFFFF the card decodes.
  switch ((gc_[6] >> 2) & 3) {
    case 0: break;
    case 1:
      if (addr >= 0x10000) return 0xFF;
      addr += bank_offset_;
      break;
    case 2:
      addr -= 0x10000;
      if (addr >= 0x8000) return 0xFF;
      break;
    default:
      addr -= 0x18000;
      if (addr >= 0x8000) return 0xFF;
      break;
  }

  if (seq_[4] & 0x08) {  // chain 4: the low two address bits pick the plane
    return addr < vram_size_ ? vram_[addr] : 0xFF;
  }
  if (gc_[5] & 0x10) {  // odd/even: address bit 0 picks the plane within the pair chosen by read map select
    const unsigned plane = (gc_[4] & 2) | (addr & 1);
    const size_t index = ((size_t{addr} & ~size_t{1}) << 1) | plane;
    return index < vram_size_ ? vram_[index] : 0xFF;
  }
  // Planar: a read loads all four planes into the latches.
  if (size_t{addr} * 4 + 4 > vram_size_) return 0xFF;
  std::memcpy(&latch_, vram_ + size_t{addr} * 4, 4);
  if (!(gc_[5] & 0x08)) return plane_byte(latch_, gc_[4] & 3);  // read mode 0
  // Read mode 1: a bit is set where every plane not ignored by colour don't care matches colour compare.
  uint32_t diff = (latch_ ^ expand_planes(gc_[2])) & expand_planes(gc_[7]);
  diff |= diff >> 16;
  diff |= diff >> 8;
  return static_cast<uint8_t>(~diff);
}

void Vga::window_write(uint32_t addr, uint8_t value) {
  switch ((gc_[6] >> 2) & 3) {
    case 0: break;
    case 1:
      if (addr >= 0x10000) return;
      addr += bank_offset_;
      break;
    case 2:
      addr -= 0x10000;
      if (addr >= 0x8000) return;
      break;
    default:
      addr -= 0x18000;
      if (addr >= 0x8000) return;
      break;
  }

  if (seq_[4] & 0x08) {
    if ((seq_[2] & (1u << (addr & 3))) && addr < vram_size_) vram_[addr] = value;
    return;
  }
  if (!(seq_[4] & 0x04)) {  // odd/even write
    const unsigned plane = (gc_[4] & 2) | (addr & 1);
    const size_t index = ((size_t{addr} & ~size_t{1}) << 1) | plane;
    if ((seq_[2] & (1u << plane)) && index < vram_size_) vram_[index] = value;
    return;
  }
  if (size_t{addr} * 4 + 4 > vram_size_) return;

  uint32_t data;
  uint32_t bit_mask = gc_[8];
  const unsigned rotate = gc_[3] & 7;
  const uint8_t rotated = static_cast<uint8_t>((value >> rotate) | (value << (8 - rotate)));
  switch (gc_[5] & 3) {
    case 0: {
      data = rotated * 0x01010101u;
      const uint32_t set_reset_enable = expand_planes(gc_[1]);
      data = (data & ~set_reset_enable) | (expand_planes(gc_[0]) & set_reset_enable);
      break;
    }
    case 1:  // latches straight back to memory; bit mask and logical op do not apply
      data = latch_;
      goto store;
    case 2:
      data = expand_planes(value & 0x0F);
      break;
    default:  // 3: the rotated value ANDed with the bit mask selects set/reset colour bits
      bit_mask &= rotated;
      data = expand_planes(gc_[0]);
      break;
  }
  switch (gc_[3] >> 3) {
    case 1: data &= latch_; break;
    case 2: data |= latch_; break;
    case 3: data ^= latch_; break;
    default: break;
  }
  bit_mask *= 0x01010101u;
  data = (data & bit_mask) | (latch_ & ~bit_mask);

store:
  const uint32_t write_mask = expand_planes(seq_[2]);
  uint32_t old;
  std::memcpy(&old, vram_ + size_t{addr} * 4, 4);
  old = (old & ~write_mask) | (data & write_mask);
  std::memcpy(vram_ + size_t{addr} * 4, &old, 4);
}

uint64_t Vga::lfb_read(uint64_t offset, unsigned size) {
  std::lock_guard lock(mutex_);
  uint64_t value = 0;
  if (size <= 8 && size <= vram_size_ && offset <= vram_size_ - size) std::memcpy(&value, vram_ + offset, size);
  return value;
}

void Vga::lfb_write(uint64_t offset, unsigned size, uint64_t value) {
  std::lock_guard lock(mutex_);
  if (size <= 8 && size <= vram_size_ && offset <= vram_size_ - size) std::memcpy(vram_ + offset, &value, size);
}

// --- Ports --------------------------------------------------------------------------------------

uint32_t Vga::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  if (port >= kVbePortBase && port < kVbePortBase + kVbePortCount) {
    // DISPI registers are 16 bits wide and accessed with 16-bit I/O.
    if (port == kVbePortBase) return vbe_index_;
    return vbe_read();
  }
  uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) value |= uint32_t{port_read(static_cast<uint16_t>(port + i))} << (8 * i);
  return value;
}

void Vga::pio_write(uint16_t port, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  if (port >= kVbePortBase && port < kVbePortBase + kVbePortCount) {
    if (port == kVbePortBase) {
      vbe_index_ = static_cast<uint16_t>(value);
      // A 32-bit write to the index port carries the data in its upper half.
      if (size == 4) vbe_write(static_cast<uint16_t>(value >> 16));
    } else {
      vbe_write(static_cast<uint16_t>(value));
    }
    return;
  }
  // Byte order is the bus order: a 16-bit write to an index port sets the index, then the data.
  for (unsigned i = 0; i < size; ++i) port_write(static_cast<uint16_t>(port + i), static_cast<uint8_t>(value >> (8 * i)));
}

bool Vga::port_ignored(uint16_t port) const {
  // Misc output bit 0 moves the CRTC and input status 1 between 0x3Bx (mono) and 0x3Dx (colour).
  if (misc_ & 1) return port == 0x3B4 || port == 0x3B5 || port == 0x3BA;
  return port == 0x3D4 || port == 0x3D5 || port == 0x3DA;
}

uint8_t Vga::port_read(uint16_t port) {
  if (port_ignored(port)) return 0xFF;
  switch (port) {
    case 0x3C0: return attr_index_;
    case 0x3C1: {
      const unsigned index = attr_index_ & 0x1F;
      return index < attr_.size() ? attr_[index] : 0;
    }
    case 0x3C2: return 0x10;  // input status 0: switch sense
    case 0x3C4: return seq_index_;
    case 0x3C5: return seq_[seq_index_ & 7];
    case 0x3C6: return dac_mask_;
    case 0x3C7: return dac_state_;
    case 0x3C8: return dac_write_index_;
    case 0x3C9: {
      const uint8_t v = palette_[dac_read_index_ * 3 + dac_sub_index_];
      if (++dac_sub_index_ == 3) {
        dac_sub_index_ = 0;
        ++dac_read_index_;
      }
      return v;
    }
    case 0x3CA: return feature_;
    case 0x3CC: return misc_;
    case 0x3CE: return gc_index_;
    case 0x3CF: return gc_[gc_index_ & 0xF];
    case 0x3B4:
    case 0x3D4: return crtc_index_;
    case 0x3B5:
    case 0x3D5: return crtc_index_ < crtc_.size() ? crtc_[crtc_index_] : 0xFF;
    case 0x3BA:
    case 0x3DA:
      // Software waits for retrace edges by polling; toggling both bits on every read lets such
      // loops finish at once instead of tracking real 60 Hz timing.
      attr_flip_flop_ = false;
      status1_ ^= 0x09;  // vertical retrace | display disabled
      return status1_;
    default: return 0xFF;
  }
}

void Vga::port_write(uint16_t port, uint8_t value) {
  if (port_ignored(port)) return;
  switch (port) {
    case 0x3C0:
      if (!attr_flip_flop_) {
        attr_index_ = value & 0x3F;
      } else {
        const unsigned index = attr_index_ & 0x1F;
        if (index < 0x10) attr_[index] = value & 0x3F;
        else if (index == 0x10) attr_[index] = value & ~0x10;
        else if (index == 0x11) attr_[index] = value;
        else if (index == 0x12) attr_[index] = value & 0x3F;
        else if (index == 0x13 || index == 0x14) attr_[index] = value & 0x0F;
      }
      attr_flip_flop_ = !attr_flip_flop_;
      break;
    case 0x3C2: misc_ = value & ~0x10; break;
    case 0x3C4: seq_index_ = value & 7; break;
    case 0x3C5: seq_[seq_index_] = value & kSeqMask[seq_index_]; break;
    case 0x3C6: dac_mask_ = value; break;
    case 0x3C7:
      dac_read_index_ = value;
      dac_sub_index_ = 0;
      dac_state_ = 3;
      break;
    case 0x3C8:
      dac_write_index_ = value;
      dac_sub_index_ = 0;
      dac_state_ = 0;
      break;
    case 0x3C9:
      dac_cache_[dac_sub_index_] = value;
      if (++dac_sub_index_ == 3) {
        std::memcpy(&palette_[dac_write_index_ * 3], dac_cache_.data(), 3);
        dac_sub_index_ = 0;
        ++dac_write_index_;
      }
      break;
    case 0x3CE: gc_index_ = value & 0xF; break;
    case 0x3CF: gc_[gc_index_] = value & kGcMask[gc_index_]; break;
    case 0x3B4:
    case 0x3D4: crtc_index_ = value; break;
    case 0x3B5:
    case 0x3D5:
      if (crtc_index_ >= crtc_.size()) break;
      // CRTC register 0x11 bit 7 write-protects the horizontal and vertical timing registers 0-7,
      // except the line compare bit 8 in the overflow register.
      if ((crtc_[0x11] & 0x80) && crtc_index_ <= 7) {
        if (crtc_index_ == 7) crtc_[7] = static_cast<uint8_t>((crtc_[7] & ~0x10) | (value & 0x10));
        break;
      }
      crtc_[crtc_index_] = value;
      break;
    case 0x3BA:
    case 0x3DA: feature_ = value & 0x10; break;
    default: break;
  }
}

// --- Bochs VBE (DISPI) ---------------------------------------------------------------------------

uint16_t Vga::vbe_read() {
  if (vbe_index_ == kVbeVideoMemory64k) return static_cast<uint16_t>(vram_size_ >> 16);
  if (vbe_index_ >= kVbeRegCount) return 0;
  if (vbe_[kVbeEnable] & kVbeGetCaps) {
    switch (vbe_index_) {
      case kVbeXres: return kVbeMaxXres;
      case kVbeYres: return kVbeMaxYres;
      case kVbeBpp: return kVbeMaxBpp;
      default: break;
    }
  }
  return vbe_[vbe_index_];
}

void Vga::vbe_write(uint16_t value) {
  switch (vbe_index_) {
    case kVbeId:
      if (value >= kVbeId0 && value <= kVbeId5) vbe_[kVbeId] = value;
      break;
    case kVbeXres:
    case kVbeYres:
    case kVbeBpp:
    case kVbeVirtWidth:
    case kVbeXOffset:
    case kVbeYOffset:
      vbe_[vbe_index_] = value;
      vbe_fixup();
      vbe_update_vga_regs();
      break;
    case kVbeBank:
      value &= static_cast<uint16_t>((vram_size_ >> 16) - 1);
      vbe_[kVbeBank] = value;
      bank_offset_ = uint32_t{value} << 16;
      break;
    case kVbeEnable:
      if ((value & kVbeEnabled) && !vbe_enabled()) {
        vbe_[kVbeVirtWidth] = 0;
        vbe_[kVbeXOffset] = 0;
        vbe_[kVbeYOffset] = 0;
        vbe_[kVbeEnable] |= kVbeEnabled;
        vbe_fixup();
        vbe_update_vga_regs();
        if (!(value & kVbeNoClearMem)) {
          std::memset(vram_, 0, std::min<size_t>(vram_size_, size_t{vbe_[kVbeYres]} * vbe_line_offset_));
        }
      } else {
        bank_offset_ = 0;
      }
      dac_8bit_ = (value & kVbe8BitDac) != 0;
      vbe_[kVbeEnable] = value;
      break;
    default: break;
  }
}

// Clamps the mode registers to what VRAM can hold and derives line length and start address.
void Vga::vbe_fixup() {
  if (!vbe_enabled()) return;
  auto& r = vbe_;
  unsigned bits;
  switch (r[kVbeBpp]) {
    case 4:
    case 8:
    case 16:
    case 24:
    case 32: bits = r[kVbeBpp]; break;
    case 15: bits = 16; break;
    default: bits = r[kVbeBpp] = 8; break;
  }
  r[kVbeXres] = static_cast<uint16_t>(std::clamp<unsigned>(r[kVbeXres] & ~7u, 8, kVbeMaxXres));
  r[kVbeVirtWidth] = static_cast<uint16_t>(std::clamp<unsigned>(r[kVbeVirtWidth] & ~7u, r[kVbeXres], kVbeMaxXres));
  const uint32_t line = r[kVbeVirtWidth] * bits / 8;
  const uint32_t max_y = static_cast<uint32_t>(vram_size_ / line);
  r[kVbeYres] = static_cast<uint16_t>(std::clamp<uint32_t>(r[kVbeYres], 1, std::min<uint32_t>(kVbeMaxYres, max_y)));
  r[kVbeXOffset] = std::min(r[kVbeXOffset], kVbeMaxXres);
  r[kVbeYOffset] = std::min(r[kVbeYOffset], kVbeMaxYres);
  uint64_t offset = uint64_t{r[kVbeXOffset]} * bits / 8 + uint64_t{r[kVbeYOffset]} * line;
  if (offset + uint64_t{r[kVbeYres]} * line > vram_size_) {
    r[kVbeYOffset] = 0;
    offset = uint64_t{r[kVbeXOffset]} * bits / 8;
    if (offset + uint64_t{r[kVbeYres]} * line > vram_size_) {
      r[kVbeXOffset] = 0;
      offset = 0;
    }
  }
  r[kVbeVirtHeight] = static_cast<uint16_t>(std::min<uint32_t>(max_y, 0xFFFF));
  vbe_line_offset_ = line;
  vbe_start_ = static_cast<uint32_t>(offset);
}

// Puts the VGA registers in the state a DISPI mode implies, so the 0xA0000 window becomes a linear
// 64 KiB bank (chain 4) and the 4 bpp modes render through the planar path with the right geometry.
void Vga::vbe_update_vga_regs() {
  if (!vbe_enabled()) return;
  gc_[6] = static_cast<uint8_t>((gc_[6] & ~0x0C) | 0x05);  // graphics, memory map 1 (0xA0000, 64 KiB)
  crtc_[0x17] |= 3;                                       // no CGA address interleave
  crtc_[0x13] = static_cast<uint8_t>(vbe_line_offset_ >> 3);
  crtc_[1] = static_cast<uint8_t>((vbe_[kVbeXres] >> 3) - 1);
  const unsigned h = vbe_[kVbeYres] - 1u;
  crtc_[0x12] = static_cast<uint8_t>(h);
  crtc_[7] = static_cast<uint8_t>((crtc_[7] & ~0x42) | ((h >> 7) & 0x02) | ((h >> 3) & 0x40));
  crtc_[0x18] = 0xFF;  // line compare beyond the screen
  crtc_[7] |= 0x10;
  crtc_[9] |= 0x40;
  unsigned shift;
  if (vbe_[kVbeBpp] == 4) {
    shift = 0;
    seq_[1] &= ~0x08;
    seq_[4] &= ~0x08;
  } else {
    shift = 2;
    seq_[4] |= 0x08;
    seq_[2] |= 0x0F;
  }
  gc_[5] = static_cast<uint8_t>((gc_[5] & ~0x60) | (shift << 5));
  crtc_[9] &= ~0x9F;  // no double scan, one scanline per row
}

// --- Rendering ----------------------------------------------------------------------------------

uint32_t Vga::dac_rgb(unsigned index) const {
  const uint8_t* c = &palette_[(index & dac_mask_ & 0xFF) * 3];  // the PEL mask gates the DAC index
  if (dac_8bit_) return rgb(c[0], c[1], c[2]);
  // 6-bit DAC values: replicate the top bits so 0x3F becomes 0xFF.
  auto expand = [](uint8_t v) { return static_cast<uint8_t>(((v & 0x3F) << 2) | ((v >> 4) & 3)); };
  return rgb(expand(c[0]), expand(c[1]), expand(c[2]));
}

uint32_t Vga::attribute_color(unsigned index) const {
  unsigned v = attr_[index & 0xF];
  // Mode control bit 7 takes palette bits 5:4 from the colour select register instead.
  if (attr_[0x10] & 0x80) v = ((attr_[0x14] & 0xFu) << 4) | (v & 0xF);
  else v = ((attr_[0x14] & 0xCu) << 4) | (v & 0x3F);
  return dac_rgb(v);
}

void Vga::render(Frame* frame) {
  std::lock_guard lock(mutex_);
  ++frame_count_;
  // Attribute index bit 5 (palette address source) clear hands the palette to the CPU and blanks the
  // screen; sequencer clocking mode bit 5 turns the screen off.
  const bool blank = !(attr_index_ & 0x20) || (seq_[1] & 0x20);
  // Mode changes in the debug log: which path renders and why, for "the screen stays black" reports.
  const uint32_t mode = (blank ? 1u : 0u) | (uint32_t{gc_[6] & 1u} << 1) | (vbe_enabled() ? 4u : 0u) |
                        (uint32_t{(gc_[5] >> 5) & 3u} << 3) | (uint32_t{vbe_[kVbeBpp]} << 8);
  if (mode != logged_mode_) {
    logged_mode_ = mode;
    SVM_DEBUG("vga: %s, attr index 0x%02x, seq1 0x%02x, gc5 0x%02x gc6 0x%02x, crtc1 %u, vbe %ux%ux%u%s",
              blank ? "blank" : (vbe_enabled() && vbe_[kVbeBpp] != 4) ? "vbe" : (gc_[6] & 1) ? "graphics" : "text",
              attr_index_, seq_[1], gc_[5], gc_[6], crtc_[1], vbe_[kVbeXres], vbe_[kVbeYres], vbe_[kVbeBpp],
              vbe_enabled() ? " (enabled)" : "");
  }
  if (!blank) {
    if (vbe_enabled() && vbe_[kVbeBpp] != 4) return render_vbe(frame);
    if (gc_[6] & 1) return render_graphics(frame);
    return render_text(frame);
  }
  if (frame->width == 0 || frame->height == 0) {
    frame->width = 640;
    frame->height = 480;
  }
  frame->pixels.assign(size_t{frame->width} * frame->height, 0);
}

namespace {
void resize(Vga::Frame* frame, uint32_t width, uint32_t height) {
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(size_t{width} * height);
}

unsigned vertical_display_end(const std::array<uint8_t, 0x19>& cr) {
  return (cr[0x12] | ((cr[7] & 0x02u) << 7) | ((cr[7] & 0x40u) << 3)) + 1;
}
}  // namespace

void Vga::render_text(Frame* frame) {
  unsigned cell_width = (seq_[1] & 0x01) ? 8 : 9;
  const bool wide = (seq_[1] & 0x08) != 0;  // dot clock / 2: 40-column modes
  const unsigned cell_height = (crtc_[9] & 0x1F) + 1u;
  const unsigned cols = crtc_[1] + 1u;
  const unsigned rows = vertical_display_end(crtc_) / cell_height;
  const unsigned dot_width = cell_width * (wide ? 2 : 1);
  if (rows == 0 || cols * dot_width > kMaxDisplay || rows * cell_height > kMaxDisplay) {
    resize(frame, 640, 400);
    std::fill(frame->pixels.begin(), frame->pixels.end(), 0);
    return;
  }
  resize(frame, cols * dot_width, rows * cell_height);

  const size_t dword_mask = vram_size_ / 4 - 1;
  const uint32_t start = (uint32_t{crtc_[0x0C]} << 8) | crtc_[0x0D];
  const uint32_t line = crtc_[0x13] * 2u;  // in character cells (VRAM dwords)
  const uint32_t cursor = (uint32_t{crtc_[0x0E]} << 8) | crtc_[0x0F];
  const unsigned cursor_start = crtc_[0x0A] & 0x1F;
  const unsigned cursor_end = crtc_[0x0B] & 0x1F;
  const bool cursor_on = !(crtc_[0x0A] & 0x20) && (frame_count_ & 16);
  const bool blink_attr = (attr_[0x10] & 0x08) != 0;
  const bool blink_phase = (frame_count_ & 32) != 0;
  const bool line_graphics = (attr_[0x10] & 0x04) != 0;
  // Character map select: map B (attribute bit 3 clear) and map A, each in 8 KiB steps of plane 2.
  const uint8_t sel = seq_[3];
  const uint32_t font_base[2] = {((((sel >> 4) & 1u) | ((sel << 1) & 6u)) * 8192u),
                                 ((((sel >> 5) & 1u) | ((sel >> 1) & 6u)) * 8192u)};

  uint32_t colors[16];
  for (unsigned i = 0; i < 16; ++i) colors[i] = attribute_color(i);

  for (unsigned row = 0; row < rows; ++row) {
    for (unsigned col = 0; col < cols; ++col) {
      const uint32_t cell = start + row * line + col;
      const size_t base = (cell & dword_mask) * 4;
      const uint8_t ch = vram_[base];
      const uint8_t attr = vram_[base + 1];
      unsigned fg = attr & 0x0F;
      unsigned bg = attr >> 4;
      if (blink_attr) {
        bg &= 7;
        if ((attr & 0x80) && blink_phase) fg = bg;
      }
      const uint32_t font = font_base[(attr >> 3) & 1] + ch * 32u;
      const bool is_cursor = cursor_on && cell == cursor;
      for (unsigned y = 0; y < cell_height; ++y) {
        unsigned bits = vram_[(((font + y) & dword_mask) * 4) + 2];
        if (is_cursor && y >= cursor_start && y <= cursor_end) bits = 0xFF;
        // The 9th column repeats column 8 for the box-drawing characters when line graphics is on.
        const bool ninth = cell_width == 9 && line_graphics && ch >= 0xC0 && ch <= 0xDF && (bits & 1);
        uint32_t* out = &frame->pixels[size_t{row * cell_height + y} * frame->width + col * dot_width];
        for (unsigned x = 0; x < cell_width; ++x) {
          const bool on = x < 8 ? (bits >> (7 - x)) & 1 : ninth;
          const uint32_t c = colors[on ? fg : bg];
          *out++ = c;
          if (wide) *out++ = c;
        }
      }
    }
  }
}

void Vga::render_graphics(Frame* frame) {
  const unsigned shift = (gc_[5] >> 5) & 3;
  const unsigned dots = (crtc_[1] + 1u) * 8;
  const unsigned height = vertical_display_end(crtc_);
  const bool pixel_double = shift < 2 && (seq_[1] & 0x08);
  const unsigned width = pixel_double ? dots * 2 : dots;
  if (width > kMaxDisplay || height > kMaxDisplay) {
    resize(frame, 640, 480);
    std::fill(frame->pixels.begin(), frame->pixels.end(), 0);
    return;
  }
  resize(frame, width, height);

  const bool vbe = vbe_enabled();
  uint32_t start = vbe ? vbe_start_ / 4 : (uint32_t{crtc_[0x0C]} << 8) | crtc_[0x0D];  // dwords
  const uint32_t line_offset = vbe ? vbe_line_offset_ : crtc_[0x13] * 8u;           // VRAM bytes
  const unsigned double_scan = crtc_[9] >> 7;
  const unsigned multi_scan = shift == 1 ? double_scan : (((crtc_[9] & 0x1Fu) + 1) << double_scan) - 1;
  const unsigned line_compare = crtc_[0x18] | ((crtc_[7] & 0x10u) << 4) | ((crtc_[9] & 0x40u) << 3);
  const size_t vram_mask = vram_size_ - 1;
  const uint32_t plane_enable = expand_planes(attr_[0x12] & 0xF);

  uint32_t colors[16];
  for (unsigned i = 0; i < 16; ++i) colors[i] = attribute_color(i);

  uint32_t line_addr = start * 4;
  unsigned row = 0;  // display row (scanline / repeat count), for CGA interleave
  unsigned repeat = multi_scan;
  for (unsigned y = 0; y < height; ++y) {
    uint32_t addr = line_addr;
    // CRTC mode bits 0/1 clear substitute row bits 0/1 for CPU address bits 13/14, the CGA and
    // Hercules interleaved layouts. Those modes write in odd/even, which doubles the address in VRAM.
    if (!(crtc_[0x17] & 1)) {
      const unsigned bit = 14 + ((crtc_[0x17] >> 6) & 1);
      addr = (addr & ~(1u << bit)) | ((row & 1u) << bit);
    }
    if (!(crtc_[0x17] & 2)) addr = (addr & ~0x8000u) | ((row & 2u) << 14);

    uint32_t* out = &frame->pixels[size_t{y} * width];
    if (shift >= 2) {
      // 256 colours: one VRAM byte per pixel straight to the DAC, two dots per pixel.
      for (unsigned x = 0; x < dots / 2; ++x) {
        const uint32_t c = dac_rgb(vram_[(addr + x) & vram_mask]);
        *out++ = c;
        *out++ = c;
      }
    } else {
      for (unsigned x = 0; x < dots / 8; ++x) {
        uint32_t data;
        std::memcpy(&data, vram_ + ((addr + x * 4) & vram_mask & ~size_t{3}), 4);
        data &= plane_enable;
        for (unsigned bit = 0; bit < 8; ++bit) {
          unsigned v;
          if (shift == 0) {
            const unsigned b = 7 - bit;
            v = ((plane_byte(data, 0) >> b) & 1) | (((plane_byte(data, 1) >> b) & 1) << 1) |
                (((plane_byte(data, 2) >> b) & 1) << 2) | (((plane_byte(data, 3) >> b) & 1) << 3);
          } else {
            // CGA 4 colours: the even byte (plane 0) holds pixels 0-3, the odd byte (plane 1) 4-7,
            // two bits each, high bits first.
            const uint8_t byte = plane_byte(data, bit >> 2);
            v = (byte >> (6 - 2 * (bit & 3))) & 3;
          }
          *out++ = colors[v];
          if (pixel_double) *out++ = colors[v];
        }
      }
    }

    if (repeat == 0) {
      const unsigned mask = (crtc_[0x17] & 3u) ^ 3u;
      if ((row & mask) == mask) line_addr += line_offset;
      ++row;
      repeat = multi_scan;
    } else {
      --repeat;
    }
    if (y == line_compare) line_addr = 0;  // split screen: the rest shows VRAM from address 0
  }
}

void Vga::render_vbe(Frame* frame) {
  const unsigned width = vbe_[kVbeXres];
  const unsigned height = vbe_[kVbeYres];
  const unsigned bpp = vbe_[kVbeBpp];
  resize(frame, width, height);
  const unsigned bytes = bpp == 15 ? 2 : bpp / 8;
  for (unsigned y = 0; y < height; ++y) {
    const size_t line = vbe_start_ + size_t{y} * vbe_line_offset_;
    uint32_t* out = &frame->pixels[size_t{y} * width];
    if (line + size_t{width} * bytes > vram_size_) {
      std::fill(out, out + width, 0);
      continue;
    }
    const uint8_t* src = vram_ + line;
    switch (bpp) {
      case 8:
        for (unsigned x = 0; x < width; ++x) out[x] = dac_rgb(src[x]);
        break;
      case 15:
        for (unsigned x = 0; x < width; ++x) {
          const unsigned p = src[2 * x] | (src[2 * x + 1] << 8);
          const unsigned r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
          out[x] = rgb(static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 3) | (g >> 2)),
                       static_cast<uint8_t>((b << 3) | (b >> 2)));
        }
        break;
      case 16:
        for (unsigned x = 0; x < width; ++x) {
          const unsigned p = src[2 * x] | (src[2 * x + 1] << 8);
          const unsigned r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
          out[x] = rgb(static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)),
                       static_cast<uint8_t>((b << 3) | (b >> 2)));
        }
        break;
      case 24:
        for (unsigned x = 0; x < width; ++x) out[x] = rgb(src[3 * x + 2], src[3 * x + 1], src[3 * x]);
        break;
      default:  // 32: B, G, R, unused
        for (unsigned x = 0; x < width; ++x) out[x] = rgb(src[4 * x + 2], src[4 * x + 1], src[4 * x]);
        break;
    }
  }
}

}  // namespace sandvm::devices
