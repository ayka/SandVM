#include "core/devices/e1000.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "core/log.h"

namespace sandvm::devices {
namespace {
// Register offsets (8254x SDM section 13), stored as dword indices.
constexpr uint32_t reg(uint32_t offset) { return offset / 4; }
constexpr uint32_t kCtrl = reg(0x0000);
constexpr uint32_t kStatus = reg(0x0008);
constexpr uint32_t kEecd = reg(0x0010);
constexpr uint32_t kEerd = reg(0x0014);
constexpr uint32_t kMdic = reg(0x0020);
constexpr uint32_t kIcr = reg(0x00C0);
constexpr uint32_t kIcs = reg(0x00C8);
constexpr uint32_t kIms = reg(0x00D0);
constexpr uint32_t kImc = reg(0x00D8);
constexpr uint32_t kRctl = reg(0x0100);
constexpr uint32_t kTctl = reg(0x0400);
constexpr uint32_t kPba = reg(0x1000);
constexpr uint32_t kRdbal = reg(0x2800);
constexpr uint32_t kRdbah = reg(0x2804);
constexpr uint32_t kRdlen = reg(0x2808);
constexpr uint32_t kRdh = reg(0x2810);
constexpr uint32_t kRdt = reg(0x2818);
constexpr uint32_t kTdbal = reg(0x3800);
constexpr uint32_t kTdbah = reg(0x3804);
constexpr uint32_t kTdlen = reg(0x3808);
constexpr uint32_t kTdh = reg(0x3810);
constexpr uint32_t kTdt = reg(0x3818);
constexpr uint32_t kStatsFirst = reg(0x4000);
constexpr uint32_t kStatsLast = reg(0x40FC);
constexpr uint32_t kGprc = reg(0x4074);  // good packets received
constexpr uint32_t kGptc = reg(0x4080);  // good packets transmitted
constexpr uint32_t kGorcl = reg(0x4088);
constexpr uint32_t kGotcl = reg(0x4090);
constexpr uint32_t kTorl = reg(0x40C0);
constexpr uint32_t kTotl = reg(0x40C8);
constexpr uint32_t kTpr = reg(0x40D0);
constexpr uint32_t kTpt = reg(0x40D4);
constexpr uint32_t kMta = reg(0x5200);
constexpr uint32_t kRa = reg(0x5400);

constexpr uint32_t kCtrlSlu = 1u << 6;
constexpr uint32_t kCtrlRst = 1u << 26;
constexpr uint32_t kCtrlPhyRst = 1u << 31;
// FD | SLU | 1000 Mb/s | SWDPIN0 | SWDPIN2, what the card shows after reset with the EEPROM below.
constexpr uint32_t kCtrlDefault = 0x00140241;
// Link up, full duplex, 1000 Mb/s, GIO master enabled.
constexpr uint32_t kStatusDefault = 0x80080783;

constexpr uint32_t kEecdSk = 0x01, kEecdCs = 0x02, kEecdDi = 0x04, kEecdDo = 0x08, kEecdReq = 0x40, kEecdGnt = 0x80,
                   kEecdPres = 0x100;
constexpr uint32_t kEerdStart = 0x01, kEerdDone = 0x10;

constexpr uint32_t kMdicReady = 1u << 28, kMdicIntEnable = 1u << 29, kMdicError = 1u << 30;

constexpr uint32_t kIcrTxdw = 0x01, kIcrTxqe = 0x02, kIcrLsc = 0x04, kIcrRxdmt0 = 0x10, kIcrRxt0 = 0x80,
                   kIcrMdac = 0x200;

constexpr uint32_t kRctlEn = 1u << 1, kRctlUpe = 1u << 3, kRctlMpe = 1u << 4, kRctlBam = 1u << 15,
                   kRctlBsex = 1u << 25, kRctlSecrc = 1u << 26;
constexpr uint32_t kTctlEn = 1u << 1;

// Transmit descriptor command bits (legacy CMD / extended DCMD share the layout we use).
constexpr uint8_t kCmdEop = 0x01, kCmdIc = 0x04, kCmdRs = 0x08, kCmdDext = 0x20, kCmdTse = 0x04;
constexpr uint8_t kPoptsIxsm = 0x01, kPoptsTxsm = 0x02;
constexpr uint8_t kTucmdTcp = 0x01, kTucmdIp = 0x02;
constexpr uint8_t kRxDd = 0x01, kRxEop = 0x02, kRxIxsm = 0x04;

constexpr size_t kMaxTxPacket = 256 * 1024;  // TSO bursts are at most 64 KiB plus headers
constexpr uint8_t kPhyAddress = 1;

uint16_t get16le(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32le(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
uint64_t get64le(const uint8_t* p) { return get32le(p) | (uint64_t{get32le(p + 4)} << 32); }
uint16_t get16be(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32be(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
void put16be(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
void put32be(uint8_t* p, uint32_t v) {
  put16be(p, static_cast<uint16_t>(v >> 16));
  put16be(p + 2, static_cast<uint16_t>(v));
}

// Inserts the one's complement of the 16-bit one's complement sum of bytes [css, cse] (cse 0 = end of
// packet) at cso, as the checksum offload engine does. The driver primes the field: 0 for the IP header
// checksum, the pseudo-header sum for TCP/UDP.
void put_checksum(std::vector<uint8_t>& p, unsigned css, unsigned cso, unsigned cse) {
  if (p.empty()) return;
  const size_t end = cse ? std::min<size_t>(cse + 1u, p.size()) : p.size();
  if (css >= end || cso + 2u > p.size()) return;
  uint32_t sum = 0;
  for (size_t i = css; i < end; i += 2) sum += (p[i] << 8) | (i + 1 < end ? p[i + 1] : 0);
  while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
  put16be(&p[cso], static_cast<uint16_t>(~sum));
}

uint32_t crc32_ethernet(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}
}  // namespace

E1000::E1000(const Mac& mac, IrqCallback on_irq, DmaMemory dma)
    : on_irq_(std::move(on_irq)), dma_(std::move(dma)), regs_(kMmioSize / 4) {
  // EEPROM image: MAC address in words 0-2, then the 82540EM defaults the drivers look at (init control
  // words, PCI IDs, PHY/LED setup), unused words 0xFFFF, and word 0x3F making the sum 0xBABA.
  eeprom_.fill(0xFFFF);
  for (unsigned i = 0; i < 3; ++i) eeprom_[i] = static_cast<uint16_t>(mac[2 * i] | (mac[2 * i + 1] << 8));
  static constexpr std::pair<uint8_t, uint16_t> kDefaults[] = {
      {0x03, 0x0000}, {0x05, 0x0000}, {0x06, 0x0000}, {0x07, 0x0000}, {0x08, 0x3000}, {0x09, 0x1000},
      {0x0A, 0x6403},  // init control 1
      {0x0B, 0x001E},  // subsystem ID: PRO/1000 MT Desktop
      {0x0C, 0x8086},  // subsystem vendor
      {0x0D, 0x100E},  // device ID
      {0x0E, 0x8086},  // vendor ID
      {0x0F, 0x3040},  // init control 2
      {0x10, 0x0008}, {0x11, 0x2000}, {0x12, 0x7E14}, {0x13, 0x0048}, {0x14, 0x1000}, {0x15, 0x00D8},
      {0x16, 0x0000}, {0x17, 0x2700}, {0x18, 0x6CC9}, {0x19, 0x3150}, {0x1A, 0x0722}, {0x1B, 0x040B},
      {0x1C, 0x0984}, {0x1D, 0x0000}, {0x1E, 0xC000}, {0x1F, 0x0706}, {0x20, 0x1008}, {0x21, 0x0000},
      {0x22, 0x0F04}, {0x23, 0x7FFF}, {0x24, 0x4D01}, {0x30, 0x0100}, {0x31, 0x4000}, {0x32, 0x121C},
  };
  for (const auto& [word, value] : kDefaults) eeprom_[word] = value;
  uint16_t sum = 0;
  for (unsigned i = 0; i < 0x3F; ++i) sum = static_cast<uint16_t>(sum + eeprom_[i]);
  eeprom_[0x3F] = static_cast<uint16_t>(0xBABA - sum);
  reset_locked();
}

void E1000::set_backend(TxCallback on_tx, Notify on_rx_space) {
  std::lock_guard lock(mutex_);
  on_tx_ = std::move(on_tx);
  on_rx_space_ = std::move(on_rx_space);
}

void E1000::reset_locked() {
  std::fill(regs_.begin(), regs_.end(), 0);
  regs_[kCtrl] = kCtrlDefault;
  regs_[kStatus] = kStatusDefault;
  regs_[kPba] = 0x00100030;  // 48 KiB receive, 16 KiB transmit packet buffer
  // Receive address 0 comes from the EEPROM, marked valid.
  regs_[kRa] = eeprom_[0] | (uint32_t{eeprom_[1]} << 16);
  regs_[kRa + 1] = eeprom_[2] | 0x80000000u;
  // M88E1011 PHY, autonegotiation done, link up at 1000 Mb/s full duplex.
  phy_.fill(0);
  phy_[0x00] = 0x1140;  // control: autoneg enable, full duplex, 1000 Mb/s
  phy_[0x01] = 0x796D;  // status: link up, autoneg complete
  phy_[0x02] = 0x0141;  // PHY ID (Marvell)
  phy_[0x03] = 0x0C20;  // M88E1011
  phy_[0x04] = 0x0DE1;  // autoneg advertisement
  phy_[0x05] = 0x45E1;  // link partner ability
  phy_[0x06] = 0x000F;  // autoneg expansion
  phy_[0x09] = 0x0E00;  // 1000BASE-T control
  phy_[0x0A] = 0x3C00;  // 1000BASE-T status: link partner 1000 FD/HD, receivers OK
  phy_[0x0F] = 0x3000;  // extended status
  phy_[0x10] = 0x0360;  // PHY specific control
  phy_[0x11] = 0xAC00;  // PHY specific status: 1000 Mb/s, full duplex, resolved, link
  phy_[0x14] = 0x0C60;  // extended PHY specific control
  eecd_pins_ = 0;
  ee_in_ = 0;
  ee_bits_in_ = ee_bit_out_ = 0;
  ee_reading_ = false;
  ctx_ = {};
  tx_buf_.clear();
  tx_first_ = true;
  update_irq();
}

bool E1000::dma(uint64_t gpa, void* data, size_t len, bool to_guest) {
  return bus_master_ && dma_ && dma_(gpa, data, len, to_guest);
}

// --- Register access -----------------------------------------------------------------------------

uint32_t E1000::read(uint32_t offset, unsigned size) {
  std::lock_guard lock(mutex_);
  if (offset >= kMmioSize) return 0;
  const uint32_t value = read_reg(offset / 4);
  return size >= 4 ? value : (value >> (8 * (offset & 3))) & ((1u << (8 * size)) - 1);
}

void E1000::write(uint32_t offset, unsigned size, uint32_t value) {
  std::lock_guard lock(mutex_);
  if (offset >= kMmioSize) return;
  if (size < 4) {
    // Narrow writes merge into the stored value (drivers use dword access; this is for robustness).
    const unsigned shift = 8 * (offset & 3);
    const uint32_t mask = ((1u << (8 * size)) - 1) << shift;
    value = (regs_[offset / 4] & ~mask) | ((value << shift) & mask);
  }
  write_reg(offset / 4, value);
}

uint32_t E1000::read_reg(uint32_t index) {
  switch (index) {
    case kEecd: return get_eecd();
    case kIcr: {
      // Reading the cause register acknowledges everything in it.
      const uint32_t value = regs_[kIcr];
      regs_[kIcr] = 0;
      update_irq();
      return value;
    }
    case kIms: return regs_[kIms];
    case kIcs: return regs_[kIcr];
    default: break;
  }
  if (index >= kStatsFirst && index <= kStatsLast) {
    const uint32_t value = regs_[index];  // statistics clear on read
    regs_[index] = 0;
    return value;
  }
  return regs_[index];
}

void E1000::write_reg(uint32_t index, uint32_t value) {
  switch (index) {
    case kCtrl:
      if (value & kCtrlRst) {
        reset_locked();
        return;
      }
      if ((value & kCtrlSlu) && !(regs_[kCtrl] & kCtrlSlu)) set_cause(kIcrLsc);  // "link came up"
      regs_[kCtrl] = value & ~(kCtrlRst | kCtrlPhyRst);
      return;
    case kStatus: return;  // read-only
    case kEecd: set_eecd(value); return;
    case kEerd:
      // EERD reads a word without the bit-banging (not used by 82540 drivers, but harmless to offer).
      if (value & kEerdStart) {
        const unsigned addr = (value >> 8) & 0xFF;
        const uint16_t data = addr < eeprom_.size() ? eeprom_[addr] : 0xFFFF;
        regs_[kEerd] = (uint32_t{data} << 16) | (addr << 8) | kEerdDone;
      }
      return;
    case kMdic: mdic(value); return;
    case kIcr:
      regs_[kIcr] &= ~value;  // write 1 to clear
      update_irq();
      return;
    case kIcs: set_cause(value); return;
    case kIms:
      regs_[kIms] |= value;
      update_irq();
      return;
    case kImc:
      regs_[kIms] &= ~value;
      update_irq();
      return;
    case kRctl: {
      const bool enabling = (value & kRctlEn) && !(regs_[kRctl] & kRctlEn);
      regs_[kRctl] = value;
      if (enabling && on_rx_space_) on_rx_space_();
      return;
    }
    case kRdlen:
    case kTdlen: regs_[index] = value & 0xFFF80; return;  // multiple of 128 bytes
    case kRdh:
    case kTdh: regs_[index] = value & 0xFFFF; return;
    case kRdt:
      regs_[kRdt] = value & 0xFFFF;
      if (on_rx_space_) on_rx_space_();
      return;
    case kTdt:
      regs_[kTdt] = value & 0xFFFF;
      start_tx();
      return;
    case kTctl:
      regs_[kTctl] = value;
      start_tx();
      return;
    default: regs_[index] = value; return;
  }
}

void E1000::set_cause(uint32_t bits) {
  regs_[kIcr] |= bits;
  update_irq();
}

void E1000::update_irq() {
  const bool level = (regs_[kIcr] & regs_[kIms]) != 0;
  if (level == irq_level_) return;
  irq_level_ = level;
  if (on_irq_) on_irq_(level);
}

// Microwire EEPROM: the driver bit-bangs SK/CS/DI and samples DO. A read is start bit + opcode 10b +
// 6 address bits (9 bits in), then the 16 data bits come out MSB first, one per SK cycle, continuing
// into the following words while CS stays asserted.
void E1000::set_eecd(uint32_t value) {
  const uint32_t old = eecd_pins_;
  eecd_pins_ = value & (kEecdSk | kEecdCs | kEecdDi | kEecdReq);
  if (!(value & kEecdCs)) return;
  if ((value ^ old) & kEecdCs) {  // chip select rising edge starts a new command
    ee_in_ = 0;
    ee_bits_in_ = 0;
    ee_bit_out_ = 0;
    ee_reading_ = false;
  }
  if (!((value ^ old) & kEecdSk)) return;
  if (!(value & kEecdSk)) {  // falling clock edge: next output bit
    if (ee_reading_) ++ee_bit_out_;
    return;
  }
  ee_in_ = (ee_in_ << 1) | ((value & kEecdDi) ? 1 : 0);
  if (++ee_bits_in_ == 9 && !ee_reading_) {
    ee_reading_ = ((ee_in_ >> 6) & 7) == 6;  // start bit 1, opcode 10b
    // The first falling edge after the address is the dummy zero bit; data starts after it.
    ee_bit_out_ = ((ee_in_ & 0x3F) << 4) - 1;
  }
}

uint32_t E1000::get_eecd() const {
  uint32_t value = eecd_pins_ | kEecdPres | ((eecd_pins_ & kEecdReq) ? kEecdGnt : 0);
  bool out = true;  // DO idles high
  if (ee_reading_) {
    const unsigned word = (ee_bit_out_ >> 4) & 0x3F;
    out = (eeprom_[word] >> (15 - (ee_bit_out_ & 15))) & 1;
  }
  if (out) value |= kEecdDo;
  return value;
}

void E1000::mdic(uint32_t value) {
  const unsigned op = (value >> 26) & 3;  // 01 write, 10 read
  const unsigned phy = (value >> 21) & 0x1F;
  const unsigned reg_index = (value >> 16) & 0x1F;
  uint32_t result = value & ~(kMdicReady | kMdicError);
  if (phy != kPhyAddress) {
    result |= kMdicError;
  } else if (op == 2) {
    result = (result & ~0xFFFFu) | phy_[reg_index];
  } else if (op == 1) {
    uint16_t data = static_cast<uint16_t>(value);
    if (reg_index == 0) {
      // Reset (bit 15) and restart autonegotiation (bit 9) complete immediately.
      if (data & 0x0200) set_cause(kIcrLsc);
      data &= ~0x8200;
    }
    if (reg_index != 1 && reg_index != 2 && reg_index != 3 && reg_index != 5 && reg_index != 0x0A &&
        reg_index != 0x0F && reg_index != 0x11) {
      phy_[reg_index] = data;  // the others are read-only status/ID registers
    }
  }
  regs_[kMdic] = result | kMdicReady;
  if (value & kMdicIntEnable) set_cause(kIcrMdac);
}

// --- Receive -------------------------------------------------------------------------------------

bool E1000::accept_frame(std::span<const uint8_t> f) const {
  const uint32_t rctl = regs_[kRctl];
  if (f.size() < 14) return false;
  const uint8_t* dst = f.data();
  if (dst[0] & 1) {
    if (std::all_of(dst, dst + 6, [](uint8_t b) { return b == 0xFF; })) return (rctl & kRctlBam) != 0;
    if (rctl & kRctlMpe) return true;
    // Multicast table: 12 bits of the destination address, picked by RCTL.MO, index a 4096-bit table.
    static constexpr unsigned kShift[4] = {4, 3, 2, 0};
    const unsigned mo = (rctl >> 12) & 3;
    const unsigned hash = ((dst[4] >> kShift[mo]) | (dst[5] << (8 - kShift[mo]))) & 0xFFF;
    return (regs_[kMta + (hash >> 5)] >> (hash & 31)) & 1;
  }
  if (rctl & kRctlUpe) return true;
  for (unsigned i = 0; i < 16; ++i) {
    const uint32_t low = regs_[kRa + 2 * i];
    const uint32_t high = regs_[kRa + 2 * i + 1];
    if (!(high & 0x80000000u)) continue;  // address valid
    const uint8_t ra[6] = {static_cast<uint8_t>(low), static_cast<uint8_t>(low >> 8), static_cast<uint8_t>(low >> 16),
                           static_cast<uint8_t>(low >> 24), static_cast<uint8_t>(high),
                           static_cast<uint8_t>(high >> 8)};
    if (std::memcmp(ra, dst, 6) == 0) return true;
  }
  return false;
}

bool E1000::receive(std::span<const uint8_t> frame) {
  std::lock_guard lock(mutex_);
  if (!(regs_[kRctl] & kRctlEn) || !bus_master_) return true;  // nobody listening: the frame is lost
  if (!accept_frame(frame)) return true;

  const uint32_t rctl = regs_[kRctl];
  static constexpr uint32_t kSizes[2][4] = {{2048, 1024, 512, 256}, {2048, 16384, 8192, 4096}};
  const uint32_t buffer_size = kSizes[(rctl & kRctlBsex) ? 1 : 0][(rctl >> 16) & 3];

  // The wire carries at least 60 bytes before the FCS; without SECRC the FCS lands in memory too.
  std::vector<uint8_t> data(frame.begin(), frame.end());
  if (data.size() < 60) data.resize(60, 0);
  if (!(rctl & kRctlSecrc)) {
    const uint32_t fcs = crc32_ethernet(data.data(), data.size());
    for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(fcs >> (8 * i)));
  }

  const uint64_t base = (uint64_t{regs_[kRdbah]} << 32) | regs_[kRdbal];
  const uint32_t count = regs_[kRdlen] / 16;
  uint32_t head = regs_[kRdh];
  const uint32_t tail = regs_[kRdt];
  if (count == 0 || head >= count || tail >= count) return true;  // ring not set up: drop
  const uint32_t free = (tail + count - head) % count;              // head == tail: none available
  const uint32_t needed = static_cast<uint32_t>((data.size() + buffer_size - 1) / buffer_size);
  if (free < needed) return false;  // keep it; the guest will return buffers

  size_t done = 0;
  while (done < data.size()) {
    const uint64_t desc_addr = base + uint64_t{head} * 16;
    uint8_t desc[16];
    if (!dma(desc_addr, desc, sizeof(desc), false)) return true;
    const size_t chunk = std::min<size_t>(buffer_size, data.size() - done);
    const uint64_t buffer = get64le(desc);
    if (buffer && !dma(buffer, data.data() + done, chunk, true)) return true;
    done += chunk;
    desc[8] = static_cast<uint8_t>(chunk);
    desc[9] = static_cast<uint8_t>(chunk >> 8);
    desc[10] = desc[11] = 0;  // packet checksum (not computed)
    desc[12] = static_cast<uint8_t>(kRxDd | kRxIxsm | (done == data.size() ? kRxEop : 0));  // IXSM: no rx csum
    desc[13] = 0;  // errors
    desc[14] = desc[15] = 0;
    if (!dma(desc_addr + 8, desc + 8, 8, true)) return true;
    head = (head + 1) % count;
  }
  regs_[kRdh] = head;

  ++regs_[kGprc];
  ++regs_[kTpr];
  regs_[kGorcl] += static_cast<uint32_t>(data.size());
  regs_[kTorl] += static_cast<uint32_t>(data.size());
  uint32_t cause = kIcrRxt0;
  // Receive descriptor minimum threshold: free descriptors fell to 1/2, 1/4 or 1/8 of the ring.
  const uint32_t left = (tail + count - head) % count;
  if (left <= (count >> (((rctl >> 8) & 3) + 1))) cause |= kIcrRxdmt0;
  set_cause(cause);
  return true;
}

// --- Transmit ------------------------------------------------------------------------------------

void E1000::start_tx() {
  if (!(regs_[kTctl] & kTctlEn)) return;
  const uint64_t base = (uint64_t{regs_[kTdbah]} << 32) | regs_[kTdbal];
  const uint32_t count = regs_[kTdlen] / 16;
  if (count == 0 || regs_[kTdh] >= count || regs_[kTdt] >= count) return;
  bool any = false;
  while (regs_[kTdh] != regs_[kTdt]) {
    const uint64_t addr = base + uint64_t{regs_[kTdh]} * 16;
    uint8_t desc[16];
    if (!dma(addr, desc, sizeof(desc), false)) break;
    tx_descriptor(desc);
    if (desc[11] & kCmdRs) {
      desc[12] |= 0x01;  // descriptor done
      dma(addr + 12, desc + 12, 1, true);
    }
    regs_[kTdh] = (regs_[kTdh] + 1) % count;
    any = true;
  }
  if (any) set_cause(kIcrTxdw | kIcrTxqe);
}

void E1000::tx_descriptor(const uint8_t* desc) {
  const uint8_t cmd = desc[11];
  if (!(cmd & kCmdDext)) {
    // Legacy descriptor: buffer, length, checksum offset/start, command.
    const uint16_t length = get16le(desc + 8);
    const size_t old = tx_buf_.size();
    if (old + length <= kMaxTxPacket) {
      tx_buf_.resize(old + length);
      if (!dma(get64le(desc), tx_buf_.data() + old, length, false)) tx_buf_.resize(old);
    }
    if (cmd & kCmdEop) {
      if (cmd & kCmdIc) put_checksum(tx_buf_, desc[13], desc[10], 0);
      emit(tx_buf_);
      tx_buf_.clear();
    }
    return;
  }
  const unsigned dtyp = (desc[10] >> 4) & 0xF;
  if (dtyp == 0) {
    // Context descriptor: where the checksums go and how to segment.
    ctx_.ipcss = desc[0];
    ctx_.ipcso = desc[1];
    ctx_.ipcse = get16le(desc + 2);
    ctx_.tucss = desc[4];
    ctx_.tucso = desc[5];
    ctx_.tucse = get16le(desc + 6);
    ctx_.tucmd = desc[11];
    ctx_.hdrlen = desc[13];
    ctx_.mss = get16le(desc + 14);
    ctx_.valid = true;
    return;
  }
  if (dtyp != 1) return;
  // Data descriptor. Options come from the first descriptor of the packet.
  if (tx_first_) {
    tx_popts_ = desc[13];
    tx_tse_ = (cmd & kCmdTse) != 0;
    tx_first_ = false;
  }
  const uint32_t length = get32le(desc + 8) & 0xFFFFF;
  const size_t old = tx_buf_.size();
  if (old + length <= kMaxTxPacket) {
    tx_buf_.resize(old + length);
    if (!dma(get64le(desc), tx_buf_.data() + old, length, false)) tx_buf_.resize(old);
  }
  if (cmd & kCmdEop) {
    if (tx_tse_ && ctx_.valid && ctx_.mss && ctx_.hdrlen) tx_tso();
    else tx_packet();
    tx_buf_.clear();
    tx_first_ = true;
  }
}

void E1000::tx_packet() {
  if (tx_popts_ & kPoptsIxsm) put_checksum(tx_buf_, ctx_.ipcss, ctx_.ipcso, ctx_.ipcse);
  if (tx_popts_ & kPoptsTxsm) put_checksum(tx_buf_, ctx_.tucss, ctx_.tucso, ctx_.tucse);
  emit(tx_buf_);
}

// TCP segmentation: the buffer is one header (hdrlen bytes) followed by up to 64 KiB of payload. Each
// segment gets a copy of the header with the IP length / ID and the TCP sequence number adjusted, FIN
// and PSH only on the last one, and checksums completed. The driver primed the TCP checksum field with
// the pseudo-header sum without the length, which we add per segment.
void E1000::tx_tso() {
  const size_t hdrlen = ctx_.hdrlen;
  if (tx_buf_.size() <= hdrlen || ctx_.tucss + 20u > hdrlen) {
    tx_packet();
    return;
  }
  const bool ipv4 = (ctx_.tucmd & kTucmdIp) != 0;
  const bool tcp = (ctx_.tucmd & kTucmdTcp) != 0;
  const size_t payload = tx_buf_.size() - hdrlen;
  const uint16_t ip_id = ipv4 && ctx_.ipcss + 6u <= hdrlen ? get16be(&tx_buf_[ctx_.ipcss + 4]) : 0;
  const uint32_t seq = tcp ? get32be(&tx_buf_[ctx_.tucss + 4]) : 0;
  unsigned index = 0;
  for (size_t offset = 0; offset < payload; offset += ctx_.mss, ++index) {
    const size_t len = std::min<size_t>(ctx_.mss, payload - offset);
    const bool last = offset + len >= payload;
    std::vector<uint8_t> seg(tx_buf_.begin(), tx_buf_.begin() + hdrlen);
    seg.insert(seg.end(), tx_buf_.begin() + hdrlen + offset, tx_buf_.begin() + hdrlen + offset + len);
    if (ipv4) {
      put16be(&seg[ctx_.ipcss + 2], static_cast<uint16_t>(seg.size() - ctx_.ipcss));
      put16be(&seg[ctx_.ipcss + 4], static_cast<uint16_t>(ip_id + index));
    } else if (ctx_.ipcss + 6u <= hdrlen) {
      put16be(&seg[ctx_.ipcss + 4], static_cast<uint16_t>(seg.size() - ctx_.ipcss - 40));  // IPv6 payload
    }
    const uint32_t l4_len = static_cast<uint32_t>(seg.size() - ctx_.tucss);
    if (tcp) {
      put32be(&seg[ctx_.tucss + 4], static_cast<uint32_t>(seq + offset));
      if (!last) seg[ctx_.tucss + 13] &= ~0x09;  // FIN, PSH
    } else {
      put16be(&seg[ctx_.tucss + 4], static_cast<uint16_t>(l4_len));  // UDP length
    }
    if (ctx_.tucso + 2u <= hdrlen) {
      uint32_t sum = get16be(&seg[ctx_.tucso]) + l4_len;
      while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
      put16be(&seg[ctx_.tucso], static_cast<uint16_t>(sum));
    }
    if (tx_popts_ & kPoptsIxsm) put_checksum(seg, ctx_.ipcss, ctx_.ipcso, ctx_.ipcse);
    if (tx_popts_ & kPoptsTxsm) put_checksum(seg, ctx_.tucss, ctx_.tucso, 0);
    emit(seg);
  }
}

void E1000::emit(std::vector<uint8_t>& frame) {
  if (frame.size() < 14) return;
  ++regs_[kGptc];
  ++regs_[kTpt];
  regs_[kGotcl] += static_cast<uint32_t>(frame.size());
  regs_[kTotl] += static_cast<uint32_t>(frame.size());
  if (on_tx_) on_tx_(frame);
}

}  // namespace sandvm::devices
