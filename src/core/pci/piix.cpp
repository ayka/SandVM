#include "core/pci/piix.h"

#include <windows.h>

#include <stdexcept>
#include <utility>

#include "core/acpi/tables.h"
#include "core/log.h"

namespace sandvm::pci {

I440fxHostBridge::I440fxHostBridge(IoBus& io) : PciFunction(io, 0x8086, 0x1237, 0x02, 0x060000) {
  // PAM0 (0x59) only has the BIOS-area read/write enable pair in bits 5:4; PAM1-6 have two pairs.
  set_wmask(0x59, 1, 0x30);
  for (unsigned off = 0x5A; off <= 0x5F; ++off) set_wmask(off, 1, 0x33);
  set_config(0x72, 1, 0x02);  // SMRAM: C_BASE_SEG = 010 (fixed)
  set_wmask(0x72, 1, 0x78);
}

Piix3Isa::Piix3Isa(IoBus& io) : PciFunction(io, 0x8086, 0x7000, 0x00, 0x060100, /*multifunction=*/true) {
  set_config(0x04, 2, 0x0007);  // a bridge to ISA always decodes; the command bits are fixed on
  set_wmask(0x04, 2, 0);
  // PIRQA-D are routed the way firmware leaves them and the way our _PRT describes them (direct GSIs,
  // no link devices). Read-only, so the chipset can never disagree with the AML; SeaBIOS writes the
  // same values anyway.
  for (unsigned i = 0; i < 4; ++i) set_config(0x60 + i, 1, acpi::kPirqIrq[i]);
}

int Piix3Isa::route(unsigned pirq) {
  const uint32_t value = config_read(0x60 + (pirq & 3), 1);
  const unsigned irq = value & 0x0F;
  // IRQ 0-2, 8 and 13 are reserved encodings on the PIIX3.
  if ((value & 0x80) || irq < 3 || irq == 8 || irq == 13) return -1;
  return static_cast<int>(irq);
}

Piix3Ide::Piix3Ide(IoBus& io, IrqCallback on_irq, devices::IdeChannel::DmaMemory dma_memory)
    : PciFunction(io, 0x8086, 0x7010, 0x00, 0x010180) {  // IDE, prog-if 0x80: compatibility mode, bus master
  set_wmask(0x04, 2, 0x0005);  // I/O space, bus master
  // IDETIM primary/secondary: bit 15 = IDE decode enable. Firmware sets it; preset it so a directly
  // booted kernel (no firmware) finds both channels enabled. Linux's ata_piix checks it.
  set_config(0x40, 2, 0x8000);
  set_config(0x42, 2, 0x8000);
  set_wmask(0x40, 4, 0xFFFFFFFF);
  set_wmask(0x44, 1, 0xFF);  // SIDETIM

  channels_[0] = std::make_shared<devices::IdeChannel>(
      devices::IdeChannel::kPrimaryBase, devices::IdeChannel::kPrimaryCtrl,
      [on_irq](bool level) { on_irq(devices::IdeChannel::kPrimaryIrq, level); }, dma_memory);
  channels_[1] = std::make_shared<devices::IdeChannel>(
      devices::IdeChannel::kSecondaryBase, devices::IdeChannel::kSecondaryCtrl,
      [on_irq](bool level) { on_irq(devices::IdeChannel::kSecondaryIrq, level); }, dma_memory);
  // Compatibility mode: the legacy ports are hard-decoded, independent of the BARs.
  for (auto& ch : channels_) {
    io.register_pio(ch->base(), devices::IdeChannel::kCommandPorts, ch);
    io.register_pio(ch->ctrl(), 1, ch);
  }
  add_io_bar(4, 16, std::make_shared<BusMaster>(channels_));
}

uint32_t Piix3Ide::BusMaster::pio_read(uint16_t port, unsigned size) {
  uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) {
    const unsigned off = (port + i) & 0xF;
    value |= uint32_t{channels_[off >> 3]->bm_read(off & 7)} << (8 * i);
  }
  return value;
}

void Piix3Ide::BusMaster::pio_write(uint16_t port, unsigned size, uint32_t value) {
  // Byte by byte, low address first: a dword write to the PRD register lands completely before a
  // later command write can start the engine.
  for (unsigned i = 0; i < size; ++i) {
    const unsigned off = (port + i) & 0xF;
    channels_[off >> 3]->bm_write(off & 7, static_cast<uint8_t>(value >> (8 * i)));
  }
}

namespace {
constexpr unsigned kPmba = 0x40;
constexpr unsigned kPmRegMisc = 0x80;
constexpr uint16_t kPmBlockSize = 64;
constexpr uint16_t kApmCnt = 0xB2;
constexpr uint16_t kApmSts = 0xB3;
constexpr uint8_t kApmAcpiEnable = 0xF1;
constexpr uint8_t kApmAcpiDisable = 0xF0;

// PM1 status/enable bits that raise SCI: timer carry, global release, power button, RTC alarm.
constexpr uint16_t kPm1Events = 0x0521;
constexpr uint16_t kPwrBtn = 0x0100;
constexpr uint16_t kSciEn = 0x0001;
constexpr uint16_t kSlpEn = 0x2000;
constexpr uint64_t kPmTimerHz = 3579545;

int64_t qpc() {
  LARGE_INTEGER v;
  QueryPerformanceCounter(&v);
  return v.QuadPart;
}

int64_t qpc_frequency() {
  LARGE_INTEGER v;
  QueryPerformanceFrequency(&v);
  return v.QuadPart;
}
}  // namespace

Piix4Pm::Piix4Pm(IoBus& io, LineCallback on_sci, EventCallback on_power_off)
    : PciFunction(io, 0x8086, 0x7113, 0x03, 0x068000), io_(io),
      regs_(std::make_shared<Regs>(std::move(on_sci), std::move(on_power_off))) {
  set_wmask(0x04, 2, 0x0001);
  set_config(kPmba, 4, 0x00000001);  // bit 0: I/O space indicator
  set_wmask(kPmba, 4, 0x0000FFC0);
  set_wmask(kPmRegMisc, 1, 0x01);
  set_wmask(0x58, 4, 0xFFFFFFFF);  // DEVACTB (firmware sets APMC_EN here)
  set_wmask(0x5C, 4, 0xFFFFFFFF);  // DEVRESD
  set_config(0x90, 4, 0x00000001);  // SMBus base (not implemented), I/O indicator
  set_wmask(0x90, 4, 0x0000FFF0);
  set_wmask(0xD2, 1, 0x0F);        // SMBus host configuration
  // The APM ports are decoded by the PIIX4 regardless of PMBA.
  io.register_pio(kApmCnt, 2, regs_);
}

void Piix4Pm::preset_pm_base(uint16_t base) {
  std::lock_guard lock(mutex_);
  set_config(kPmba, 4, (base & 0xFFC0u) | 1u);
  set_config(kPmRegMisc, 1, 0x01);
  remap();
}

void Piix4Pm::press_power_button() { regs_->press_power_button(); }

uint16_t Piix4Pm::pm_base() {
  std::lock_guard lock(mutex_);
  return mapped_;
}

void Piix4Pm::after_config_write(unsigned off, unsigned size) {
  if ((off < kPmba + 4 && off + size > kPmba) || (off <= kPmRegMisc && off + size > kPmRegMisc)) remap();
}

void Piix4Pm::remap() {
  const uint16_t base = static_cast<uint16_t>(get_config(kPmba, 4) & 0xFFC0);
  const uint16_t want = (get_config(kPmRegMisc, 1) & 1) && base ? base : 0;
  if (want == mapped_) return;
  if (mapped_) io_.unregister_pio(mapped_, regs_.get());
  mapped_ = 0;
  regs_->set_base(want);
  if (!want) return;
  try {
    io_.register_pio(want, kPmBlockSize, regs_);
    mapped_ = want;
  } catch (const std::invalid_argument&) {
    SVM_WARN("piix4-pm: PM block at 0x%04x overlaps another device, not decoded", want);
  }
}

Piix4Pm::Regs::Regs(LineCallback on_sci, EventCallback on_power_off)
    : on_sci_(std::move(on_sci)), on_power_off_(std::move(on_power_off)), qpc_start_(qpc()),
      qpc_freq_(qpc_frequency()) {}

void Piix4Pm::Regs::set_base(uint16_t base) {
  std::lock_guard lock(mutex_);
  base_ = base;
}

void Piix4Pm::Regs::press_power_button() {
  std::lock_guard lock(mutex_);
  pm1_sts_ |= kPwrBtn;
  update_sci();
}

uint32_t Piix4Pm::Regs::timer() const {
  // 24-bit free-running counter. Split the scaling so it cannot overflow however long the VM runs.
  const uint64_t d = static_cast<uint64_t>(qpc() - qpc_start_);
  const uint64_t f = static_cast<uint64_t>(qpc_freq_);
  const uint64_t ticks = (d / f) * kPmTimerHz + (d % f) * kPmTimerHz / f;
  return static_cast<uint32_t>(ticks & 0xFFFFFF);
}

uint32_t Piix4Pm::Regs::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  timer_sample_ = timer();  // one sample per access, so a 32-bit read cannot tear across a carry
  uint32_t value = 0;
  for (unsigned i = 0; i < size; ++i) {
    const uint16_t p = static_cast<uint16_t>(port + i);
    uint8_t byte = 0;
    if (p == kApmCnt) byte = apm_cnt_;
    else if (p == kApmSts) byte = apm_sts_;
    else if (base_ && p >= base_ && p < base_ + kPmBlockSize) byte = read_byte(p - base_);
    value |= uint32_t{byte} << (8 * i);
  }
  return value;
}

void Piix4Pm::Regs::pio_write(uint16_t port, unsigned size, uint32_t value) {
  bool power_off = false;
  {
    std::lock_guard lock(mutex_);
    for (unsigned i = 0; i < size; ++i) {
      const uint16_t p = static_cast<uint16_t>(port + i);
      const uint8_t byte = static_cast<uint8_t>(value >> (8 * i));
      if (p == kApmCnt) {
        apm_cnt_ = byte;
        if (byte == kApmAcpiEnable) pm1_cnt_ |= kSciEn;
        else if (byte == kApmAcpiDisable) pm1_cnt_ &= ~kSciEn;
        apm_sts_ = 0;  // the "SMI handler" is done
      } else if (p == kApmSts) {
        apm_sts_ = byte;
      } else if (base_ && p >= base_ && p < base_ + kPmBlockSize) {
        write_byte(p - base_, byte);
        if (pm1_cnt_ & kSlpEn) {
          // SLP_EN is write-only and self-clearing. SLP_TYP 0 is S5 on the PIIX4 and in our \_S5.
          const unsigned slp_typ = (pm1_cnt_ >> 10) & 7;
          pm1_cnt_ &= ~kSlpEn;
          if (slp_typ == 0) power_off = true;
          else SVM_WARN("piix4-pm: sleep type %u requested, not supported", slp_typ);
        }
      }
    }
    update_sci();
  }
  if (power_off && on_power_off_) on_power_off_();
}

uint8_t Piix4Pm::Regs::read_byte(unsigned off) {
  switch (off) {
    case 0x00: return static_cast<uint8_t>(pm1_sts_);
    case 0x01: return static_cast<uint8_t>(pm1_sts_ >> 8);
    case 0x02: return static_cast<uint8_t>(pm1_en_);
    case 0x03: return static_cast<uint8_t>(pm1_en_ >> 8);
    case 0x04: return static_cast<uint8_t>(pm1_cnt_);
    case 0x05: return static_cast<uint8_t>(pm1_cnt_ >> 8);
    case 0x08: case 0x09: case 0x0A: case 0x0B: return static_cast<uint8_t>(timer_sample_ >> (8 * (off - 8)));
    case 0x0C: return static_cast<uint8_t>(gpe_sts_);
    case 0x0D: return static_cast<uint8_t>(gpe_sts_ >> 8);
    case 0x0E: return static_cast<uint8_t>(gpe_en_);
    case 0x0F: return static_cast<uint8_t>(gpe_en_ >> 8);
  }
  return 0;
}

void Piix4Pm::Regs::write_byte(unsigned off, uint8_t value) {
  const unsigned shift = (off & 1) * 8;
  const uint16_t v = static_cast<uint16_t>(uint16_t{value} << shift);
  const uint16_t lane = static_cast<uint16_t>(0xFF << shift);
  switch (off) {
    case 0x00: case 0x01: pm1_sts_ &= ~v; break;  // write 1 to clear
    case 0x02: case 0x03: pm1_en_ = static_cast<uint16_t>((pm1_en_ & ~lane) | (v & kPm1Events)); break;
    case 0x04: case 0x05: pm1_cnt_ = static_cast<uint16_t>((pm1_cnt_ & ~lane) | (v & 0x3C07)); break;
    case 0x0C: case 0x0D: gpe_sts_ &= ~v; break;
    case 0x0E: case 0x0F: gpe_en_ = static_cast<uint16_t>((gpe_en_ & ~lane) | v); break;
    default: break;
  }
}

void Piix4Pm::Regs::update_sci() {
  const bool level = (pm1_cnt_ & kSciEn) && ((pm1_sts_ & pm1_en_ & kPm1Events) || (gpe_sts_ & gpe_en_));
  if (level == sci_) return;
  sci_ = level;
  if (on_sci_) on_sci_(level);
}

}  // namespace sandvm::pci
