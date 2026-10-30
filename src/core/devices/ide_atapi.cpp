// ATAPI half of IdeChannel: a read-only CD/DVD-ROM drive behind the PACKET command (ATA/ATAPI-6,
// clause 8.21; SFF-8020/MMC for the command set). Implements the commands boot firmware (El Torito),
// Linux sr/libata and Windows cdrom.sys/atapi.sys issue on a data disc; anything else reports
// ILLEGAL REQUEST / INVALID COMMAND OPERATION CODE, which all of them tolerate.

#include <algorithm>
#include <cstring>

#include "core/devices/ide.h"
#include "core/devices/ide_regs.h"
#include "core/log.h"

namespace sandvm::devices {
using namespace ide;

namespace {
// Interrupt reason, reported in the sector count register during a packet command.
constexpr uint8_t kReasonCoD = 0x01;  // command (packet) phase / completion
constexpr uint8_t kReasonIo = 0x02;   // data to the host

// Sense keys and additional sense codes.
constexpr uint8_t kNotReady = 0x02;
constexpr uint8_t kMediumError = 0x03;
constexpr uint8_t kIllegalRequest = 0x05;
constexpr uint8_t kUnitAttention = 0x06;
constexpr uint8_t kAscReadError = 0x11;
constexpr uint8_t kAscInvalidOpcode = 0x20;
constexpr uint8_t kAscLbaOutOfRange = 0x21;
constexpr uint8_t kAscInvalidField = 0x24;
constexpr uint8_t kAscMediumChanged = 0x28;  // not ready to ready change, medium may have changed
constexpr uint8_t kAscSavingNotSupported = 0x39;
constexpr uint8_t kAscNoMedium = 0x3A;
constexpr uint8_t kAscRemovalPrevented = 0x53;

// Larger than a CD can hold (80 minutes): report the medium as a DVD.
constexpr uint64_t kCdMaxSectors = 80 * 60 * 75;
constexpr uint32_t kReadBatch = 16;  // sectors per host read while streaming a READ

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
void push_be16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
void push_be32(std::vector<uint8_t>& v, uint32_t x) {
  push_be16(v, x >> 16);
  push_be16(v, x);
}
void put_be16(std::vector<uint8_t>& v, size_t off, uint32_t x) {
  v[off] = static_cast<uint8_t>(x >> 8);
  v[off + 1] = static_cast<uint8_t>(x);
}
void put_text(std::vector<uint8_t>& v, size_t off, size_t len, const char* text) {
  for (size_t i = 0; i < len; ++i) v[off + i] = i < std::strlen(text) ? static_cast<uint8_t>(text[i]) : ' ';
}
}  // namespace

void IdeChannel::execute_atapi(Drive& d, uint8_t command) {
  switch (command) {
    case 0xA0:  // PACKET
      packet_dma_ = (features_ & 0x01) != 0;  // data phase through the bus master
      if (packet_dma_ && !dma_memory_) return abort_command();
      // Byte count limit: the most the host takes per DRQ. 0xFFFF is treated as 0xFFFE; 0 is only valid
      // for commands without data (IDENTIFY PACKET word 125 does not offer a default).
      byte_limit_ = lcyl_ | (uint32_t{hcyl_} << 8);
      if (byte_limit_ == 0xFFFF) byte_limit_ = 0xFFFE;
      buffer_pos_ = 0;
      transfer_ = Transfer::Packet;
      nsector_ = static_cast<uint8_t>((nsector_ & 0xF8) | kReasonCoD);
      // No interrupt: IDENTIFY PACKET DEVICE advertises accelerated DRQ for the command packet.
      status_ = kStatusReady | kStatusDrq;
      return;

    case 0xA1:  // IDENTIFY PACKET DEVICE
      build_identify_packet();
      transfer_ = Transfer::Identify;
      status_ = kStatusReady | kStatusDrq;
      raise_irq();
      return;

    case 0xEC:  // IDENTIFY DEVICE: a packet device aborts and shows its signature; that is how drivers spot it
      load_signature(d);
      return abort_command();

    case 0x08:  // DEVICE RESET
      transfer_ = Transfer::None;
      load_signature(d);
      error_ = 0x01;
      status_ = 0;  // no interrupt, DRDY clear until the next command (as QEMU)
      return;

    case 0x90:  // EXECUTE DEVICE DIAGNOSTIC
      set_signature();
      error_ = 0x01;
      return complete();

    case 0xE5:  // CHECK POWER MODE
      nsector_ = 0xFF;
      return complete();

    case 0xEF:  // SET FEATURES
      if (features_ == 0x03 && dma_memory_) return set_transfer_mode(d);
      return complete();
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6:  // standby / idle / sleep
      return complete();

    default:
      SVM_DEBUG("ide: ATAPI device aborts ATA command 0x%02x", command);
      return abort_command();
  }
}

void IdeChannel::build_identify_packet() {
  uint16_t w[256] = {};
  // ATAPI, CD-ROM (type 5), removable, accelerated DRQ (bits 6:5 = 10), 12-byte packets.
  w[0] = 0x85C0;
  put_string(&w[10], 10, "SANDVM0000000002");
  put_string(&w[23], 4, "0.1");
  put_string(&w[27], 20, "SandVM DVD-ROM");
  w[49] = 0x0200;  // LBA
  w[53] = 0x0003;  // words 64-70 valid
  w[64] = 0x0003;  // PIO 3 and 4
  w[65] = w[66] = w[67] = w[68] = 120;
  w[71] = 30;  // release times
  w[72] = 30;
  w[80] = 0x007E;  // ATA/ATAPI-1..6
  w[82] = 0x4010;  // PACKET feature set, NOP
  w[83] = 0x4000;
  w[84] = 0x4000;
  w[85] = 0x4010;
  w[86] = 0x0000;
  w[87] = 0x4000;
  if (dma_memory_) identify_dma_words(w, *active_);
  buffer_.resize(512);
  for (unsigned i = 0; i < 256; ++i) {
    buffer_[2 * i] = static_cast<uint8_t>(w[i]);
    buffer_[2 * i + 1] = static_cast<uint8_t>(w[i] >> 8);
  }
  buffer_pos_ = 0;
}

bool IdeChannel::atapi_need_media() {
  if (active_->disk) return true;
  atapi_error(kNotReady, kAscNoMedium);
  return false;
}

void IdeChannel::run_packet() {
  transfer_ = Transfer::None;
  Drive& d = *active_;
  const uint8_t* c = cdb_;
  const uint8_t op = c[0];
  // Sense data describes the last command only; REQUEST SENSE is what reads it.
  const uint8_t last_key = d.sense_key, last_asc = d.asc;
  d.sense_key = d.asc = 0;
  SVM_DEBUG("ide: ATAPI packet 0x%02x", op);

  // After a disc change the next command reports UNIT ATTENTION, except the ones MMC lets through
  // (INQUIRY, REQUEST SENSE, GET CONFIGURATION, GET EVENT STATUS NOTIFICATION): drivers learn of
  // the change that way and re-read the capacity and TOC.
  if (d.unit_attention && op != 0x12 && op != 0x03 && op != 0x46 && op != 0x4A) {
    d.unit_attention = false;
    return atapi_error(kUnitAttention, kAscMediumChanged);
  }

  switch (op) {
    case 0x00:  // TEST UNIT READY
      if (!atapi_need_media()) return;
      return atapi_ok();

    case 0x03: {  // REQUEST SENSE (fixed format)
      std::vector<uint8_t> r(18, 0);
      r[0] = 0x70;
      r[2] = last_key;
      r[7] = 10;
      r[12] = last_asc;
      return atapi_reply(std::move(r), c[4]);
    }

    case 0x12: {  // INQUIRY
      if (c[1] & 0x01) {  // vital product data: supported pages and the unit serial number
        std::vector<uint8_t> r = {0x05, c[2], 0x00};
        if (c[2] == 0x00) {
          r.insert(r.end(), {0x02, 0x00, 0x80});
        } else if (c[2] == 0x80) {
          const char serial[] = "SANDVM0000000002";
          r.push_back(static_cast<uint8_t>(sizeof(serial) - 1));
          r.insert(r.end(), serial, serial + sizeof(serial) - 1);
        } else {
          return atapi_error(kIllegalRequest, kAscInvalidField);
        }
        return atapi_reply(std::move(r), be16(c + 3));
      }
      std::vector<uint8_t> r(36, 0);
      r[0] = 0x05;  // CD/DVD device
      r[1] = 0x80;  // removable
      r[3] = 0x21;  // ATAPI, response data format 1
      r[4] = 31;
      put_text(r, 8, 8, "SANDVM");
      put_text(r, 16, 16, "DVD-ROM");
      put_text(r, 32, 4, "0.1");
      return atapi_reply(std::move(r), be16(c + 3));
    }

    case 0x1A:    // MODE SENSE (6)
    case 0x5A: {  // MODE SENSE (10)
      const bool ten = op == 0x5A;
      const unsigned page = c[2] & 0x3F;
      const unsigned control = c[2] >> 6;  // 0 current, 1 changeable, 2 default, 3 saved
      if (control == 3) return atapi_error(kIllegalRequest, kAscSavingNotSupported);
      const std::vector<uint8_t> error_recovery = {0x01, 0x06, 0x00, 0x05, 0x00, 0x00, 0x00, 0x00};
      // Capabilities page as QEMU reports it: reads CD-R/RW and DVD, lockable tray, 4x.
      const std::vector<uint8_t> capabilities = {0x2A, 0x12, 0x3B, 0x00, 0x70, 0x60, 0x29, 0x00, 0x02, 0xC0,
                                                 0x00, 0x02, 0x02, 0x00, 0x02, 0xC0, 0x00, 0x00, 0x00, 0x00};
      std::vector<uint8_t> pages;
      if (page == 0x01 || page == 0x3F) pages.insert(pages.end(), error_recovery.begin(), error_recovery.end());
      if (page == 0x2A || page == 0x3F) pages.insert(pages.end(), capabilities.begin(), capabilities.end());
      if (pages.empty()) return atapi_error(kIllegalRequest, kAscInvalidField);
      if (control == 1) {
        // Nothing is changeable: keep each page's code and length, zero the parameter bytes.
        for (size_t p = 0; p < pages.size(); p += 2 + pages[p + 1]) {
          std::fill(pages.begin() + p + 2, pages.begin() + p + 2 + pages[p + 1], 0);
        }
      }
      std::vector<uint8_t> r(ten ? 8 : 4, 0);
      r.insert(r.end(), pages.begin(), pages.end());
      if (ten) put_be16(r, 0, static_cast<uint32_t>(r.size() - 2));
      else r[0] = static_cast<uint8_t>(r.size() - 1);
      return atapi_reply(std::move(r), ten ? be16(c + 7) : c[4]);
    }

    case 0x1B:  // START STOP UNIT: LoEj with Start clear ejects the disc (the guest's "eject" command)
      if ((c[4] & 0x03) == 0x02) {
        if (d.prevent_removal) return atapi_error(kIllegalRequest, kAscRemovalPrevented);
        if (d.disk) {
          SVM_INFO("ide: the guest ejected the disc");
          d.disk.reset();
          d.sectors = 0;
          d.media_event = 3;
        }
      }
      return atapi_ok();

    case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
      d.prevent_removal = (c[4] & 0x01) != 0;
      return atapi_ok();

    case 0x2B:  // SEEK (10)
    case 0x35:  // SYNCHRONIZE CACHE
    case 0xBB:  // SET CD SPEED
      return atapi_ok();

    case 0x25: {  // READ CAPACITY
      if (!atapi_need_media()) return;
      std::vector<uint8_t> r;
      push_be32(r, static_cast<uint32_t>(d.sectors ? d.sectors - 1 : 0));
      push_be32(r, kCdSectorSize);
      return atapi_reply(std::move(r), 8);
    }

    case 0x28:    // READ (10)
    case 0xA8: {  // READ (12)
      if (!atapi_need_media()) return;
      const uint64_t lba = be32(c + 2);
      const uint32_t count = op == 0x28 ? be16(c + 7) : be32(c + 6);
      if (lba + count > d.sectors) return atapi_error(kIllegalRequest, kAscLbaOutOfRange);
      return atapi_start_read(lba, count);
    }

    case 0x43: {  // READ TOC/PMA/ATIP
      if (!atapi_need_media()) return;
      const bool msf = (c[1] & 0x02) != 0;
      unsigned format = c[2] & 0x0F;
      if (format == 0) format = c[9] >> 6;  // pre-MMC drivers put the format in the control byte
      const unsigned start_track = c[6];
      auto address = [&](std::vector<uint8_t>& r, uint32_t lba) {
        if (!msf) return push_be32(r, lba);
        lba += 150;  // MSF counts the 2-second pregap
        r.push_back(0);
        r.push_back(static_cast<uint8_t>(lba / (60 * 75)));
        r.push_back(static_cast<uint8_t>(lba / 75 % 60));
        r.push_back(static_cast<uint8_t>(lba % 75));
      };
      std::vector<uint8_t> r = {0, 0, 1, 1};  // length, first and last track
      if (format == 0) {  // formatted TOC: one data track plus the lead-out
        if (start_track > 1 && start_track != 0xAA) return atapi_error(kIllegalRequest, kAscInvalidField);
        if (start_track <= 1) {
          r.insert(r.end(), {0x00, 0x14, 0x01, 0x00});  // ADR 1, data track, track 1
          address(r, 0);
        }
        r.insert(r.end(), {0x00, 0x16, 0xAA, 0x00});  // lead-out
        address(r, static_cast<uint32_t>(d.sectors));
      } else if (format == 1) {  // multi-session info: session 1 starts at track 1
        r.insert(r.end(), {0x00, 0x14, 0x01, 0x00});
        address(r, 0);
      } else {
        return atapi_error(kIllegalRequest, kAscInvalidField);
      }
      put_be16(r, 0, static_cast<uint32_t>(r.size() - 2));
      return atapi_reply(std::move(r), be16(c + 7));
    }

    case 0x46: {  // GET CONFIGURATION: header plus the profile list feature (0000h), our only feature
      const unsigned request_type = c[1] & 0x03;  // 0 all, 1 current, 2 one feature
      const unsigned start = be16(c + 2);
      if (request_type == 3) return atapi_error(kIllegalRequest, kAscInvalidField);
      const bool dvd = d.disk && d.sectors > kCdMaxSectors;
      const bool cd = d.disk && !dvd;
      std::vector<uint8_t> r(8, 0);
      put_be16(r, 6, dvd ? 0x0010 : cd ? 0x0008 : 0x0000);  // current profile
      if (start == 0) {
        r.insert(r.end(), {0x00, 0x00, 0x03, 0x08,                       // feature 0, persistent+current
                           0x00, 0x10, static_cast<uint8_t>(dvd), 0x00,  // DVD-ROM
                           0x00, 0x08, static_cast<uint8_t>(cd), 0x00});  // CD-ROM
      }
      const uint32_t len = static_cast<uint32_t>(r.size() - 4);
      r[0] = static_cast<uint8_t>(len >> 24);
      r[1] = static_cast<uint8_t>(len >> 16);
      r[2] = static_cast<uint8_t>(len >> 8);
      r[3] = static_cast<uint8_t>(len);
      return atapi_reply(std::move(r), be16(c + 7));
    }

    case 0x4A: {  // GET EVENT STATUS NOTIFICATION (polled only)
      if (!(c[1] & 0x01)) return atapi_error(kIllegalRequest, kAscInvalidField);
      std::vector<uint8_t> r;
      if (c[4] & 0x10) {  // media class: a pending change (reported once), then presence
        r = {0x00, 0x06, 0x04, 0x10, d.media_event, static_cast<uint8_t>(d.disk ? 0x02 : 0x00), 0x00, 0x00};
        d.media_event = 0;
      } else {
        r = {0x00, 0x02, 0x80, 0x10};  // no event available for the requested classes
      }
      return atapi_reply(std::move(r), be16(c + 7));
    }

    case 0x51: {  // READ DISC INFORMATION: one complete session with one track
      if (!atapi_need_media()) return;
      std::vector<uint8_t> r(34, 0);
      put_be16(r, 0, 32);
      r[2] = 0x0E;  // last session complete, disc complete
      r[3] = 1;     // first track
      r[4] = 1;     // sessions
      r[5] = 1;     // first track in last session
      r[6] = 1;     // last track in last session
      return atapi_reply(std::move(r), be16(c + 7));
    }

    case 0xBD: {  // MECHANISM STATUS: no changer, no slots
      std::vector<uint8_t> r(8, 0);
      return atapi_reply(std::move(r), be16(c + 8));
    }

    default:
      SVM_DEBUG("ide: unsupported ATAPI packet 0x%02x", op);
      return atapi_error(kIllegalRequest, kAscInvalidOpcode);
  }
}

void IdeChannel::atapi_reply(std::vector<uint8_t> data, size_t allocation) {
  if (data.size() > allocation) data.resize(allocation);
  if (data.empty()) return atapi_ok();
  if (packet_dma_) {
    buffer_ = std::move(data);
    dma_buffer_pos_ = 0;
    return start_dma(DmaKind::AtapiIn, 0, buffer_.size());
  }
  if (byte_limit_ == 0) return atapi_abort();
  buffer_ = std::move(data);
  buffer_pos_ = 0;
  total_left_ = buffer_.size();
  cd_sectors_left_ = 0;
  transfer_ = Transfer::AtapiIn;
  atapi_next_chunk();
}

void IdeChannel::atapi_start_read(uint64_t lba, uint32_t count) {
  if (count == 0) return atapi_ok();
  if (packet_dma_) {
    buffer_.clear();
    dma_buffer_pos_ = 0;
    return start_dma(DmaKind::AtapiIn, lba * kCdSectorSize, uint64_t{count} * kCdSectorSize);
  }
  if (byte_limit_ == 0) return atapi_abort();
  buffer_.clear();
  buffer_pos_ = 0;
  lba_ = lba;
  cd_sectors_left_ = count;
  total_left_ = size_t{count} * kCdSectorSize;
  transfer_ = Transfer::AtapiIn;
  atapi_next_chunk();
}

void IdeChannel::atapi_next_chunk() {
  // Every DRQ chunk but the last must be even (the data port is 16 bits wide); an odd final chunk is
  // padded by the 0xFF the data port returns after the transfer ends.
  chunk_left_ = std::min<size_t>(total_left_, byte_limit_);
  if (chunk_left_ < total_left_) chunk_left_ = std::max<size_t>(chunk_left_ & ~size_t{1}, 2);
  lcyl_ = static_cast<uint8_t>(chunk_left_);
  hcyl_ = static_cast<uint8_t>(chunk_left_ >> 8);
  nsector_ = static_cast<uint8_t>((nsector_ & 0xF8) | kReasonIo);
  status_ = kStatusReady | kStatusDrq;
  raise_irq();
}

void IdeChannel::atapi_data_in(uint8_t* out, unsigned len) {
  for (unsigned i = 0; i < len; ++i) {
    if (transfer_ != Transfer::AtapiIn) {
      out[i] = 0xFF;
      continue;
    }
    if (buffer_pos_ >= buffer_.size()) {
      // Stream the next batch of a READ from the image.
      const uint32_t n = std::min(cd_sectors_left_, kReadBatch);
      buffer_.resize(size_t{n} * kCdSectorSize);
      buffer_pos_ = 0;
      if (n == 0 || !active_->disk || !active_->disk->read(lba_ * kCdSectorSize, buffer_.data(), buffer_.size())) {
        atapi_error(kMediumError, kAscReadError);
        out[i] = 0xFF;
        continue;
      }
      lba_ += n;
      cd_sectors_left_ -= n;
    }
    out[i] = buffer_[buffer_pos_++];
    --total_left_;
    if (--chunk_left_ > 0) continue;
    if (total_left_ > 0) atapi_next_chunk();
    else atapi_ok();
  }
}

void IdeChannel::atapi_ok() {
  transfer_ = Transfer::None;
  error_ = 0;
  status_ = kStatusReady;
  nsector_ = static_cast<uint8_t>((nsector_ & 0xF8) | kReasonIo | kReasonCoD);
  raise_irq();
}

void IdeChannel::atapi_abort() {
  // Rejected at the ATA level (e.g. a data command with a zero byte count limit), as QEMU does.
  abort_command();
  nsector_ = static_cast<uint8_t>((nsector_ & 0xF8) | kReasonIo | kReasonCoD);
}

void IdeChannel::atapi_error(uint8_t sense_key, uint8_t asc) {
  active_->sense_key = sense_key;
  active_->asc = asc;
  transfer_ = Transfer::None;
  error_ = static_cast<uint8_t>(sense_key << 4);
  status_ = kStatusReady | kStatusErr;
  nsector_ = static_cast<uint8_t>((nsector_ & 0xF8) | kReasonIo | kReasonCoD);
  raise_irq();
}

}  // namespace sandvm::devices
