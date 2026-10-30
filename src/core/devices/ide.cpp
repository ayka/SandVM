#include "core/devices/ide.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "core/devices/ide_regs.h"
#include "core/log.h"

namespace sandvm::devices {
using namespace ide;

IdeChannel::IdeChannel(uint16_t base, uint16_t ctrl, IrqCallback on_irq, DmaMemory dma_memory)
    : base_(base), ctrl_(ctrl), on_irq_(std::move(on_irq)), dma_memory_(std::move(dma_memory)) {
  set_signature();
  status_ = kStatusReady;
}

void IdeChannel::attach(unsigned index, std::shared_ptr<block::Disk> disk) {
  if (index > 1 || !disk) throw std::invalid_argument("IdeChannel::attach: bad drive index or null disk");
  std::lock_guard lock(mutex_);
  Drive& d = drives_[index];
  d = Drive{};
  d.present = true;
  d.disk = std::move(disk);
  d.sectors = d.disk->size_bytes() / kSectorSize;
  // The usual BIOS-compatible translation: 16 heads, 63 sectors per track, at most 16383 cylinders.
  d.heads = 16;
  d.spt = 63;
  d.cylinders = static_cast<uint16_t>(std::clamp<uint64_t>(d.sectors / (16 * 63), 1, 16383));
  d.cur_heads = d.heads;
  d.cur_spt = d.spt;
  d.multiple = 0;
  set_signature();
}

void IdeChannel::attach_cdrom(unsigned index, std::shared_ptr<block::Disk> media) {
  if (index > 1) throw std::invalid_argument("IdeChannel::attach_cdrom: bad drive index");
  std::lock_guard lock(mutex_);
  Drive& d = drives_[index];
  d = Drive{};
  d.present = true;
  d.atapi = true;
  d.disk = std::move(media);
  d.sectors = d.disk ? d.disk->size_bytes() / kCdSectorSize : 0;
  set_signature();
}

bool IdeChannel::change_media(unsigned index, std::shared_ptr<block::Disk> media) {
  if (index > 1) return false;
  std::lock_guard lock(mutex_);
  Drive& d = drives_[index];
  if (!d.present || !d.atapi) return false;
  d.disk = std::move(media);
  d.sectors = d.disk ? d.disk->size_bytes() / kCdSectorSize : 0;
  d.media_event = d.disk ? 2 : 3;  // NewMedia / MediaRemoval
  d.prevent_removal = false;
  d.unit_attention = true;
  // A transfer in flight from the old disc (PIO data phase, or DMA parked until the engine runs) ends
  // now with the unit attention, so one command never returns data from two discs.
  if (active_ == &d && (transfer_ == Transfer::AtapiIn || dma_kind_ == DmaKind::AtapiIn)) {
    if (dma_kind_ == DmaKind::AtapiIn) {
      dma_kind_ = DmaKind::None;
      dma_bytes_ = 0;
      bm_status_ &= static_cast<uint8_t>(~0x01);  // bus master no longer active
    }
    buffer_.clear();
    buffer_pos_ = 0;
    total_left_ = chunk_left_ = 0;
    cd_sectors_left_ = 0;
    d.unit_attention = false;  // reported right here
    atapi_error(0x06, 0x28);   // UNIT ATTENTION, medium may have changed
  }
  return true;
}

bool IdeChannel::is_cdrom(unsigned index) {
  if (index > 1) return false;
  std::lock_guard lock(mutex_);
  return drives_[index].present && drives_[index].atapi;
}

IdeChannel::Drive* IdeChannel::selected() {
  Drive& d = drives_[(select_ >> 4) & 1];
  return d.present ? &d : nullptr;
}

uint32_t IdeChannel::pio_read(uint16_t port, unsigned size) {
  std::lock_guard lock(mutex_);
  // No device at all: nothing drives the bus and the pull-ups read all-ones, which drivers take as
  // "no devices on this channel" (a status of 0xFF can never be valid).
  if (empty()) return 0xFFFFFFFF >> (32 - 8 * std::min(size, 4u));
  if (port == ctrl_) return selected() ? status_ : 0;  // alternate status: does not acknowledge the IRQ
  const unsigned reg = port - base_;
  uint8_t bytes[4] = {};
  if (reg == 0) {
    data_in(bytes, std::min(size, 4u));
  } else {
    for (unsigned i = 0; i < size && i < 4 && reg + i < kCommandPorts; ++i) bytes[i] = read_register(reg + i);
  }
  uint32_t value = 0;
  for (unsigned i = 0; i < size && i < 4; ++i) value |= uint32_t{bytes[i]} << (8 * i);
  return value;
}

void IdeChannel::pio_write(uint16_t port, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  if (port == ctrl_) {
    write_control(static_cast<uint8_t>(value));
    return;
  }
  const unsigned reg = port - base_;
  uint8_t bytes[4];
  for (unsigned i = 0; i < 4; ++i) bytes[i] = static_cast<uint8_t>(value >> (8 * i));
  if (reg == 0) {
    data_out(bytes, std::min(size, 4u));
    return;
  }
  for (unsigned i = 0; i < size && i < 4 && reg + i < kCommandPorts; ++i) write_register(reg + i, bytes[i]);
}

uint8_t IdeChannel::read_register(unsigned reg) {
  // An absent device does not drive the bus; with its partner present the lines read as 0. That is
  // how BIOSes tell a missing slave from a present one.
  if (reg != 6 && !selected()) return 0;
  const bool hob = (control_ & kCtrlHob) != 0;
  switch (reg) {
    case 1: return error_;
    case 2: return hob ? hob_nsector_ : nsector_;
    case 3: return hob ? hob_sector_ : sector_;
    case 4: return hob ? hob_lcyl_ : lcyl_;
    case 5: return hob ? hob_hcyl_ : hcyl_;
    case 6: return select_;
    case 7:  // reading the status register acknowledges INTRQ
      irq_pending_ = false;
      set_irq(false);
      return status_;
  }
  return 0xFF;
}

void IdeChannel::write_register(unsigned reg, uint8_t value) {
  if (reg >= 1 && reg <= 5) {
    control_ &= ~kCtrlHob;  // any task file write clears HOB
    // The write lands in the selected device's registers; the other one keeps showing its signature.
    if (Drive* d = selected()) d->signature = false;
  }
  switch (reg) {
    case 1: features_ = value; break;
    case 2: hob_nsector_ = nsector_; nsector_ = value; break;
    case 3: hob_sector_ = sector_; sector_ = value; break;
    case 4: hob_lcyl_ = lcyl_; lcyl_ = value; break;
    case 5: hob_hcyl_ = hcyl_; hcyl_ = value; break;
    case 6:
      select_ = value | 0xA0;  // bits 7 and 5 are obsolete and read as 1
      // Each device has its own task file; ours is shared, so present the newly selected device's
      // signature while it is still showing one.
      if (const Drive* d = selected(); d && d->signature) load_signature(*d);
      break;
    case 7: execute(value); break;
  }
}

void IdeChannel::write_control(uint8_t value) {
  const bool was_reset = (control_ & kCtrlReset) != 0;
  control_ = value;
  if ((value & kCtrlReset) && !was_reset) {
    // SRST asserted: both devices are busy until it is released.
    transfer_ = Transfer::None;
    active_ = nullptr;
    dma_kind_ = DmaKind::None;
    status_ = kStatusBsy;
    irq_pending_ = false;
  } else if (!(value & kCtrlReset) && was_reset) {
    select_ = 0xA0;
    set_signature();
    error_ = 0x01;  // diagnostic code: device 0 passed, device 1 passed or absent
    status_ = kStatusReady;
  }
  set_irq(irq_pending_ && !(control_ & kCtrlNoIrq));
}

void IdeChannel::set_signature() {
  for (auto& d : drives_) d.signature = d.present;
  load_signature(drives_[(select_ >> 4) & 1]);
}

void IdeChannel::load_signature(const Drive& d) {
  // ATA: 01 01 00 00; packet devices: 01 01 14 EB.
  nsector_ = 1;
  sector_ = 1;
  lcyl_ = d.atapi ? 0x14 : 0x00;
  hcyl_ = d.atapi ? 0xEB : 0x00;
  hob_nsector_ = hob_sector_ = hob_lcyl_ = hob_hcyl_ = 0;
}

void IdeChannel::execute(uint8_t command) {
  Drive* d = selected();
  if (!d) return;  // nobody is listening on the bus
  if (status_ & kStatusBsy) return;
  transfer_ = Transfer::None;
  dma_kind_ = DmaKind::None;
  active_ = d;
  error_ = 0;
  d->signature = false;
  if (d->atapi) return execute_atapi(*d, command);

  switch (command) {
    case 0xEC:  // IDENTIFY DEVICE
      build_identify(*d);
      transfer_ = Transfer::Identify;
      status_ = kStatusReady | kStatusDrq;
      raise_irq();
      return;

    case 0x20: case 0x21: case 0x24: case 0xC4: case 0x29: {  // READ SECTORS (EXT) / READ MULTIPLE (EXT)
      const bool ext = command == 0x24 || command == 0x29;
      const bool multiple = command == 0xC4 || command == 0x29;
      if (multiple && !d->multiple) return abort_command();
      uint64_t lba;
      if (!decode_lba(ext, &lba)) return abort_command(kErrIdNotFound);
      return start_read(lba, sector_count(ext), multiple ? d->multiple : 1, ext);
    }

    case 0x30: case 0x31: case 0x34: case 0xC5: case 0x39: {  // WRITE SECTORS (EXT) / WRITE MULTIPLE (EXT)
      const bool ext = command == 0x34 || command == 0x39;
      const bool multiple = command == 0xC5 || command == 0x39;
      if (multiple && !d->multiple) return abort_command();
      uint64_t lba;
      if (!decode_lba(ext, &lba)) return abort_command(kErrIdNotFound);
      return start_write(lba, sector_count(ext), multiple ? d->multiple : 1, ext);
    }

    case 0xC8: case 0xC9: case 0x25:    // READ DMA (EXT)
    case 0xCA: case 0xCB: case 0x35: {  // WRITE DMA (EXT)
      const bool ext = command == 0x25 || command == 0x35;
      const bool write = command == 0xCA || command == 0xCB || command == 0x35;
      if (!dma_memory_ || (write && d->disk->read_only())) return abort_command();
      uint64_t lba;
      const uint32_t count = sector_count(ext);
      if (!decode_lba(ext, &lba) || lba + count > d->sectors) return abort_command(kErrIdNotFound);
      lba_ = lba;
      remaining_ = count;
      transfer_lba48_ = ext;
      return start_dma(write ? DmaKind::AtaWrite : DmaKind::AtaRead, lba * kSectorSize, uint64_t{count} * kSectorSize);
    }

    case 0x40: case 0x41: case 0x42: {  // READ VERIFY SECTORS (EXT): reads the media, returns no data
      const bool ext = command == 0x42;
      uint64_t lba;
      if (!decode_lba(ext, &lba) || lba + sector_count(ext) > d->sectors) return abort_command(kErrIdNotFound);
      transfer_lba48_ = ext;
      if (!verify(*d, lba, sector_count(ext))) return;
      return complete();
    }

    case 0xC6: {  // SET MULTIPLE MODE: 0 disables, otherwise a power of two up to our maximum
      const unsigned n = nsector_;
      if (n > kMaxMultiple || (n & (n - 1))) return abort_command();
      d->multiple = static_cast<uint8_t>(n);
      return complete();
    }

    case 0x91:  // INITIALIZE DEVICE PARAMETERS: CHS translation used by non-LBA commands
      if (nsector_ == 0) return abort_command();
      d->cur_heads = static_cast<uint16_t>((select_ & 0x0F) + 1);
      d->cur_spt = nsector_;
      return complete();

    case 0xF8:  // READ NATIVE MAX ADDRESS
    case 0x27:  // READ NATIVE MAX ADDRESS EXT
      encode_lba(command == 0x27, d->sectors ? d->sectors - 1 : 0);
      return complete();

    case 0xE7:  // FLUSH CACHE
    case 0xEA:  // FLUSH CACHE EXT
      if (!d->disk->flush()) return abort_command();
      return complete();

    case 0xE5:  // CHECK POWER MODE: always active
      nsector_ = 0xFF;
      return complete();

    case 0x90:  // EXECUTE DEVICE DIAGNOSTIC
      set_signature();
      error_ = 0x01;
      return complete();

    case 0xEF:  // SET FEATURES: the transfer mode is recorded for IDENTIFY; write cache etc. are accepted
      if (features_ == 0x03 && dma_memory_) return set_transfer_mode(*d);
      return complete();
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:  // standby / idle / sleep
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x98: case 0x99:  // the same, legacy opcodes
    case 0x70:  // SEEK
      return complete();

    default:
      if (command >= 0x10 && command <= 0x1F) return complete();  // RECALIBRATE
      // Includes ATAPI (0xA0, 0xA1), DMA commands, SMART, security: this is a PIO-only ATA disk.
      SVM_DEBUG("ide: aborting unsupported command 0x%02x", command);
      return abort_command();
  }
}

bool IdeChannel::decode_lba(bool lba48, uint64_t* lba) {
  if (lba48) {
    *lba = uint64_t{sector_} | uint64_t{lcyl_} << 8 | uint64_t{hcyl_} << 16 | uint64_t{hob_sector_} << 24 |
           uint64_t{hob_lcyl_} << 32 | uint64_t{hob_hcyl_} << 40;
    return true;
  }
  if (select_ & kSelectLba) {
    *lba = uint64_t{select_ & 0x0Fu} << 24 | uint64_t{hcyl_} << 16 | uint64_t{lcyl_} << 8 | sector_;
    return true;
  }
  const Drive* d = selected();
  const unsigned head = select_ & 0x0F;
  const unsigned cylinder = lcyl_ | (hcyl_ << 8);
  if (!d || sector_ == 0 || sector_ > d->cur_spt || head >= d->cur_heads) return false;
  *lba = (uint64_t{cylinder} * d->cur_heads + head) * d->cur_spt + (sector_ - 1);
  return true;
}

void IdeChannel::encode_lba(bool lba48, uint64_t lba) {
  if (lba48) {
    sector_ = static_cast<uint8_t>(lba);
    lcyl_ = static_cast<uint8_t>(lba >> 8);
    hcyl_ = static_cast<uint8_t>(lba >> 16);
    hob_sector_ = static_cast<uint8_t>(lba >> 24);
    hob_lcyl_ = static_cast<uint8_t>(lba >> 32);
    hob_hcyl_ = static_cast<uint8_t>(lba >> 40);
  } else if (select_ & kSelectLba) {
    sector_ = static_cast<uint8_t>(lba);
    lcyl_ = static_cast<uint8_t>(lba >> 8);
    hcyl_ = static_cast<uint8_t>(lba >> 16);
    select_ = static_cast<uint8_t>((select_ & 0xF0) | ((lba >> 24) & 0x0F));
  } else if (const Drive* d = active_ ? active_ : selected(); d && d->cur_heads && d->cur_spt) {
    const uint64_t per_cylinder = uint64_t{d->cur_heads} * d->cur_spt;
    const uint64_t cylinder = lba / per_cylinder;
    sector_ = static_cast<uint8_t>(lba % d->cur_spt + 1);
    lcyl_ = static_cast<uint8_t>(cylinder);
    hcyl_ = static_cast<uint8_t>(cylinder >> 8);
    select_ = static_cast<uint8_t>((select_ & 0xF0) | ((lba / d->cur_spt) % d->cur_heads));
  }
}

void IdeChannel::report_progress() {
  encode_lba(transfer_lba48_, lba_);
  nsector_ = static_cast<uint8_t>(remaining_);
  if (transfer_lba48_) hob_nsector_ = static_cast<uint8_t>(remaining_ >> 8);
}

bool IdeChannel::verify(Drive& d, uint64_t lba, uint32_t count) {
  // Bounded scratch buffer: a 65536-sector EXT verify would otherwise need 32 MiB at once.
  constexpr uint32_t kChunk = 128;
  std::vector<uint8_t> scratch(size_t{std::min(count, kChunk)} * kSectorSize);
  lba_ = lba;
  remaining_ = count;
  while (remaining_) {
    const uint32_t n = std::min(remaining_, kChunk);
    if (!d.disk->read(lba_ * kSectorSize, scratch.data(), size_t{n} * kSectorSize)) {
      report_progress();
      abort_command(kErrUncorrectable);
      return false;
    }
    lba_ += n;
    remaining_ -= n;
  }
  report_progress();
  return true;
}

uint32_t IdeChannel::sector_count(bool lba48) const {
  if (lba48) {
    const uint32_t n = nsector_ | (uint32_t{hob_nsector_} << 8);
    return n ? n : 65536;
  }
  return nsector_ ? nsector_ : 256;
}

void IdeChannel::start_read(uint64_t lba, uint32_t count, unsigned block, bool lba48) {
  if (lba + count > active_->sectors) return abort_command(kErrIdNotFound);
  lba_ = lba;
  remaining_ = count;
  block_ = block;
  transfer_lba48_ = lba48;
  transfer_ = Transfer::Read;
  load_read_block();
}

bool IdeChannel::load_read_block() {
  const unsigned n = std::min<uint32_t>(block_, remaining_);
  buffer_.resize(size_t{n} * kSectorSize);
  if (!active_->disk->read(lba_ * kSectorSize, buffer_.data(), buffer_.size())) {
    report_progress();
    abort_command(kErrUncorrectable);
    return false;
  }
  block_sectors_ = n;
  buffer_pos_ = 0;
  status_ = kStatusReady | kStatusDrq;
  raise_irq();  // PIO in: an interrupt for every block that becomes ready
  return true;
}

void IdeChannel::start_write(uint64_t lba, uint32_t count, unsigned block, bool lba48) {
  if (active_->disk->read_only()) return abort_command();
  if (lba + count > active_->sectors) return abort_command(kErrIdNotFound);
  lba_ = lba;
  remaining_ = count;
  block_ = block;
  transfer_lba48_ = lba48;
  transfer_ = Transfer::Write;
  block_sectors_ = std::min<uint32_t>(block_, remaining_);
  buffer_.assign(size_t{block_sectors_} * kSectorSize, 0);
  buffer_pos_ = 0;
  status_ = kStatusReady | kStatusDrq;  // PIO out: no interrupt before the first block
}

void IdeChannel::finish_write_block() {
  if (!active_->disk->write(lba_ * kSectorSize, buffer_.data(), buffer_.size())) {
    report_progress();
    return abort_command(kErrUncorrectable);
  }
  lba_ += block_sectors_;
  remaining_ -= block_sectors_;
  report_progress();
  if (remaining_ == 0) return complete();
  block_sectors_ = std::min<uint32_t>(block_, remaining_);
  buffer_.assign(size_t{block_sectors_} * kSectorSize, 0);
  buffer_pos_ = 0;
  status_ = kStatusReady | kStatusDrq;
  raise_irq();
}

void IdeChannel::data_in(uint8_t* out, unsigned len) {
  if (transfer_ == Transfer::AtapiIn) return atapi_data_in(out, len);
  for (unsigned i = 0; i < len; ++i) {
    if ((transfer_ != Transfer::Read && transfer_ != Transfer::Identify) || buffer_pos_ >= buffer_.size()) {
      out[i] = 0xFF;
      continue;
    }
    out[i] = buffer_[buffer_pos_++];
    if (buffer_pos_ < buffer_.size()) continue;

    if (transfer_ == Transfer::Identify) {
      transfer_ = Transfer::None;
      status_ = kStatusReady;
      continue;
    }
    lba_ += block_sectors_;
    remaining_ -= block_sectors_;
    report_progress();
    if (remaining_ > 0) {
      load_read_block();
    } else {
      // PIO in ends without a final interrupt.
      transfer_ = Transfer::None;
      status_ = kStatusReady;
    }
  }
}

void IdeChannel::data_out(const uint8_t* in, unsigned len) {
  if (transfer_ == Transfer::Packet) {
    for (unsigned i = 0; i < len && transfer_ == Transfer::Packet; ++i) {
      cdb_[buffer_pos_++] = in[i];
      if (buffer_pos_ == sizeof(cdb_)) run_packet();
    }
    return;
  }
  for (unsigned i = 0; i < len; ++i) {
    if (transfer_ != Transfer::Write || buffer_pos_ >= buffer_.size()) return;
    buffer_[buffer_pos_++] = in[i];
    if (buffer_pos_ == buffer_.size()) finish_write_block();
  }
}

void IdeChannel::build_identify(const Drive& d) {
  uint16_t w[256] = {};
  const uint64_t lba28 = std::min<uint64_t>(d.sectors, 0x0FFFFFFF);
  const uint64_t per_cylinder = std::max<uint64_t>(uint64_t{d.cur_heads} * d.cur_spt, 1);
  const uint64_t cur_cylinders = std::min<uint64_t>(d.sectors / per_cylinder, 65535);
  const uint64_t cur_capacity = cur_cylinders * per_cylinder;

  w[0] = 0x0040;  // fixed, non-removable ATA device
  w[1] = d.cylinders;
  w[3] = d.heads;
  w[6] = d.spt;
  put_string(&w[10], 10, "SANDVM0000000001");
  put_string(&w[23], 4, "0.1");
  put_string(&w[27], 20, "SandVM HARDDISK");
  w[47] = 0x8000 | kMaxMultiple;
  w[49] = 0x0200;  // LBA supported
  w[51] = 0x0200;  // PIO timing mode 2
  w[53] = 0x0003;  // words 54-58 and 64-70 are valid
  w[54] = static_cast<uint16_t>(cur_cylinders);
  w[55] = d.cur_heads;
  w[56] = d.cur_spt;
  w[57] = static_cast<uint16_t>(cur_capacity);
  w[58] = static_cast<uint16_t>(cur_capacity >> 16);
  w[59] = d.multiple ? static_cast<uint16_t>(0x0100 | d.multiple) : 0;
  w[60] = static_cast<uint16_t>(lba28);
  w[61] = static_cast<uint16_t>(lba28 >> 16);
  w[64] = 0x0003;  // PIO modes 3 and 4
  w[65] = w[66] = w[67] = w[68] = 120;  // cycle times in ns
  w[80] = 0x007E;                       // ATA-1 .. ATA-6
  w[82] = 0x4000;                       // NOP supported
  w[83] = 0x7400;                       // bit 14 valid, FLUSH CACHE EXT, FLUSH CACHE, 48-bit addressing
  w[84] = 0x4000;
  w[85] = 0x4000;
  w[86] = 0x3400;  // the same features enabled
  w[87] = 0x4000;
  w[100] = static_cast<uint16_t>(d.sectors);
  w[101] = static_cast<uint16_t>(d.sectors >> 16);
  w[102] = static_cast<uint16_t>(d.sectors >> 32);
  w[103] = static_cast<uint16_t>(d.sectors >> 48);
  if (dma_memory_) identify_dma_words(w, d);

  buffer_.resize(512);
  for (unsigned i = 0; i < 256; ++i) {
    buffer_[2 * i] = static_cast<uint8_t>(w[i]);
    buffer_[2 * i + 1] = static_cast<uint8_t>(w[i] >> 8);
  }
  buffer_pos_ = 0;
}

void IdeChannel::complete(bool raise) {
  transfer_ = Transfer::None;
  status_ = kStatusReady;
  if (raise) raise_irq();
}

void IdeChannel::abort_command(uint8_t error) {
  transfer_ = Transfer::None;
  error_ = error;
  status_ = kStatusReady | kStatusErr;
  raise_irq();
}

void IdeChannel::raise_irq() {
  irq_pending_ = true;
  set_irq(!(control_ & kCtrlNoIrq));
}

void IdeChannel::set_irq(bool level) {
  if (level == irq_line_) return;
  irq_line_ = level;
  if (level) bm_status_ |= 0x04;  // the PIIX latches each INTRQ rising edge in the bus-master interrupt bit
  if (on_irq_) on_irq_(level);
}

}  // namespace sandvm::devices
