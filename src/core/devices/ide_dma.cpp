// Bus-master DMA half of IdeChannel (SFF-8038i "Bus Master Programming Interface for IDE ATA
// Controllers", as implemented by the PIIX). Each channel has a command register (start, direction),
// a status register (active, error, interrupt, drive-capable bits) and the address of a PRD table:
// 8-byte entries {u32 physical address, u16 byte count (0 = 64 KiB), u16 flags (bit 15 = last)}.
//
// A DMA command parks the transfer until the driver sets the start bit (drivers do it in either
// order); the whole transfer then runs synchronously on the vCPU thread that made it possible.

#include <algorithm>
#include <cstring>

#include "core/devices/ide.h"
#include "core/devices/ide_regs.h"
#include "core/log.h"

namespace sandvm::devices {
using namespace ide;

namespace {
constexpr uint8_t kBmStart = 0x01;
constexpr uint8_t kBmToMemory = 0x08;
constexpr uint8_t kBmActive = 0x01;
constexpr uint8_t kBmError = 0x02;
constexpr uint8_t kBmInterrupt = 0x04;
constexpr uint8_t kBmCapable = 0x60;
constexpr size_t kChunk = 64 * 1024;
constexpr unsigned kMaxPrdEntries = 8192;  // a 64 KiB table; stops a malformed (looping) one
}  // namespace

uint8_t IdeChannel::bm_read(unsigned reg) {
  std::lock_guard lock(mutex_);
  switch (reg) {
    case 0: return bm_command_;
    case 2: return bm_status_;
    case 4: case 5: case 6: case 7: return static_cast<uint8_t>(bm_prdt_ >> (8 * (reg - 4)));
  }
  return 0;
}

void IdeChannel::bm_write(unsigned reg, uint8_t value) {
  std::lock_guard lock(mutex_);
  switch (reg) {
    case 0: {
      const bool was_started = (bm_command_ & kBmStart) != 0;
      bm_command_ = value & (kBmStart | kBmToMemory);
      if (!(value & kBmStart)) {
        // Stopping the engine ends bus mastering; a command still waiting for data keeps waiting.
        bm_status_ &= ~kBmActive;
      } else if (!was_started) {
        bm_status_ |= kBmActive;
        if (dma_kind_ != DmaKind::None) run_dma();
      }
      break;
    }
    case 2:  // error and interrupt are write-1-to-clear, the drive-capable bits are plain storage
      bm_status_ = static_cast<uint8_t>((bm_status_ & kBmActive) | (bm_status_ & (kBmError | kBmInterrupt) & ~value) |
                                        (value & kBmCapable));
      break;
    case 4: case 5: case 6: case 7: {
      const unsigned shift = 8 * (reg - 4);
      bm_prdt_ = (bm_prdt_ & ~(0xFFu << shift)) | (uint32_t{value} << shift);
      bm_prdt_ &= ~3u;  // dword aligned
      break;
    }
    default: break;
  }
}

void IdeChannel::start_dma(DmaKind kind, uint64_t offset, uint64_t bytes) {
  dma_kind_ = kind;
  dma_offset_ = offset;
  dma_bytes_ = bytes;
  dma_total_ = bytes;
  status_ = kStatusReady | kStatusDrq;  // data phase, owned by the DMA engine
  if (bm_command_ & kBmStart) run_dma();
}

IdeChannel::DmaResult IdeChannel::dma_chunk(uint64_t gpa, size_t len) {
  std::vector<uint8_t> tmp(len);
  block::Disk* disk = active_->disk.get();
  switch (dma_kind_) {
    case DmaKind::AtaRead:
      if (!disk->read(dma_offset_, tmp.data(), len)) return DmaResult::DiskError;
      if (!dma_memory_(gpa, tmp.data(), len, true)) return DmaResult::BusError;
      break;
    case DmaKind::AtaWrite:
      if (!dma_memory_(gpa, tmp.data(), len, false)) return DmaResult::BusError;
      if (!disk->write(dma_offset_, tmp.data(), len)) return DmaResult::DiskError;
      break;
    case DmaKind::AtapiIn: {
      // A reply prepared in buffer_ (INQUIRY, TOC, ...) or a READ straight from the medium.
      size_t done = 0;
      if (dma_buffer_pos_ < buffer_.size()) {
        done = std::min(len, buffer_.size() - dma_buffer_pos_);
        std::memcpy(tmp.data(), buffer_.data() + dma_buffer_pos_, done);
      }
      if (done < len && (!disk || !disk->read(dma_offset_, tmp.data() + done, len - done))) return DmaResult::DiskError;
      if (!dma_memory_(gpa, tmp.data(), len, true)) return DmaResult::BusError;
      dma_buffer_pos_ += done;
      dma_offset_ += len - done;
      return DmaResult::Done;
    }
    case DmaKind::None:
      return DmaResult::BusError;
  }
  dma_offset_ += len;
  return DmaResult::Done;
}

void IdeChannel::run_dma() {
  // The driver programs the direction; a mismatch with the command is a controller programming error.
  const bool to_memory = dma_kind_ != DmaKind::AtaWrite;
  if (((bm_command_ & kBmToMemory) != 0) != to_memory) return finish_dma(DmaResult::BusError, false);

  uint32_t prd = bm_prdt_;
  DmaResult result = DmaResult::PrdShort;
  bool prd_left = false;
  for (unsigned n = 0; n < kMaxPrdEntries; ++n, prd += 8) {
    uint8_t e[8];
    if (!dma_memory_(prd, e, sizeof(e), false)) {
      result = DmaResult::BusError;
      break;
    }
    uint64_t addr = (uint32_t{e[0]} | uint32_t{e[1]} << 8 | uint32_t{e[2]} << 16 | uint32_t{e[3]} << 24) & ~1u;
    uint32_t len = e[4] | (uint32_t{e[5]} << 8);
    if (len == 0) len = 0x10000;
    len &= ~1u;
    const bool last = (e[7] & 0x80) != 0;
    DmaResult step_result = DmaResult::Done;
    while (len && dma_bytes_) {
      const size_t step = static_cast<size_t>(std::min<uint64_t>({len, dma_bytes_, kChunk}));
      step_result = dma_chunk(addr, step);
      if (step_result != DmaResult::Done) break;
      addr += step;
      len -= static_cast<uint32_t>(step);
      dma_bytes_ -= step;
    }
    if (step_result != DmaResult::Done) {
      result = step_result;
      break;
    }
    if (dma_bytes_ == 0) {
      // The table described more memory than the transfer needed: the engine stays active (SFF-8038i).
      result = DmaResult::Done;
      prd_left = len > 0 || !last;
      break;
    }
    if (last) break;  // table exhausted with data left
  }
  finish_dma(result, prd_left);
}

void IdeChannel::finish_dma(DmaResult result, bool prd_left) {
  // SFF-8038i status after a transfer: Active=0/Interrupt=1 normal end, Active=1/Interrupt=1 table longer
  // than the transfer, Active=0/Interrupt=0 table too short (the device keeps waiting for data), Error for
  // a failed bus-master cycle (no device interrupt). Interrupt itself comes from INTRQ via set_irq().
  if (result == DmaResult::PrdShort || result == DmaResult::BusError) {
    bm_status_ &= ~kBmActive;
    if (result == DmaResult::BusError) bm_status_ |= kBmError;
    // The command stays parked with what is left; restarting the engine with a new table continues it,
    // a reset abandons it.
    return;
  }
  const DmaKind kind = dma_kind_;
  dma_kind_ = DmaKind::None;
  if (!prd_left) bm_status_ &= ~kBmActive;
  if (kind == DmaKind::AtapiIn) {
    dma_bytes_ = 0;
    if (result == DmaResult::Done) atapi_ok();
    else atapi_error(0x03, 0x11);  // MEDIUM ERROR, unrecovered read error
    return;
  }
  // ATA: the task file shows where the transfer stopped (next LBA, residual count), as in the PIO path.
  const uint64_t done_sectors = (dma_total_ - dma_bytes_) / kSectorSize;
  lba_ += done_sectors;
  remaining_ -= static_cast<uint32_t>(done_sectors);
  dma_bytes_ = 0;
  report_progress();
  if (result == DmaResult::Done) complete();
  else abort_command(kErrUncorrectable | kErrAbort);
}

void IdeChannel::set_transfer_mode(Drive& d) {
  // SET FEATURES 03h: sector count = 00h/01h PIO default, 08h+n PIO n, 20h+n MWDMA n, 40h+n UDMA n.
  const uint8_t mode = nsector_;
  const bool valid = mode <= 0x01 || (mode >= 0x08 && mode <= 0x0C) || (mode >= 0x20 && mode <= 0x22) ||
                     (mode >= 0x40 && mode <= 0x42);
  if (!valid) return abort_command();
  d.transfer_mode = mode;
  complete();
}

void IdeChannel::identify_dma_words(uint16_t* w, const Drive& d) const {
  const uint8_t mode = d.transfer_mode;
  w[49] |= 0x0100;  // DMA supported
  w[53] |= 0x0004;  // word 88 valid
  // Multiword DMA 0-2 and Ultra DMA 0-2, with the selected mode flagged in the high byte.
  w[63] = static_cast<uint16_t>(0x0007 | ((mode & 0xF0) == 0x20 ? 0x0100 << (mode & 7) : 0));
  w[88] = static_cast<uint16_t>(0x0007 | ((mode & 0xF0) == 0x40 ? 0x0100 << (mode & 7) : 0));
}

}  // namespace sandvm::devices
