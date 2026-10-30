#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "core/block/disk.h"
#include "core/io_bus.h"

namespace sandvm::devices {

// One legacy IDE channel (command block at base, control block at ctrl) with up to two devices, PIO
// only. ATA hard disks implement what BIOSes and the Linux/Windows legacy IDE drivers use: IDENTIFY,
// READ/WRITE SECTORS (CHS, LBA28, LBA48), READ/WRITE MULTIPLE, READ VERIFY, SET MULTIPLE MODE,
// INITIALIZE DEVICE PARAMETERS, SET FEATURES, FLUSH CACHE, power commands as no-ops, EXECUTE
// DEVICE DIAGNOSTIC and software reset. ATAPI CD/DVD-ROM drives (ide_atapi.cpp) take the PACKET
// command with the MMC subset that boot firmware, Linux sr and Windows cdrom.sys need. Bus-master DMA
// (SFF-8038i, as on the PIIX) for READ/WRITE DMA (EXT) and DMA PACKET commands: the channel owns its
// 8 bus-master registers (bm_read/bm_write, routed by the PCI function's BAR4) and copies data between
// the image and guest memory through the PRD table when both the command and the engine's start bit
// are there.
//
// The task file is shared by both drives, as on the wire, except that each device presents its own
// signature after a reset until the task file is written. Thread-safe; disk I/O happens synchronously
// with the channel lock held, on the vCPU thread (so images should be on local storage: a stalled
// network share stalls the vCPU). The IRQ callback is called with the lock held (device -> PIC lock order).
class IdeChannel : public PioDevice {
 public:
  static constexpr uint16_t kPrimaryBase = 0x1F0;
  static constexpr uint16_t kPrimaryCtrl = 0x3F6;
  static constexpr unsigned kPrimaryIrq = 14;
  static constexpr uint16_t kSecondaryBase = 0x170;
  static constexpr uint16_t kSecondaryCtrl = 0x376;
  static constexpr unsigned kSecondaryIrq = 15;
  static constexpr uint16_t kCommandPorts = 8;

  using IrqCallback = std::function<void(bool level)>;
  // Guest physical memory for DMA: copies len bytes to (to_guest) or from guest physical gpa.
  // Returns false if the range is not RAM.
  using DmaMemory = std::function<bool(uint64_t gpa, void* data, size_t len, bool to_guest)>;

  IdeChannel(uint16_t base, uint16_t ctrl, IrqCallback on_irq, DmaMemory dma_memory = {});

  // index 0 = master, 1 = slave. Call before the VM runs. Disk size is truncated to whole sectors.
  void attach(unsigned index, std::shared_ptr<block::Disk> disk);
  // An ATAPI CD/DVD-ROM drive; `media` is an ISO image (2048-byte sectors) or null for an empty tray.
  void attach_cdrom(unsigned index, std::shared_ptr<block::Disk> media);
  // Thread-safe, while the VM runs: swaps (or with null removes) the disc in an ATAPI drive, like
  // opening and closing the tray. The guest sees UNIT ATTENTION (medium may have changed) on its next
  // command and a media event through GET EVENT STATUS NOTIFICATION. Returns false if `index` is not a
  // CD/DVD drive. A transfer in flight from the old disc ends with an error.
  bool change_media(unsigned index, std::shared_ptr<block::Disk> media);
  bool is_cdrom(unsigned index);

  uint16_t base() const { return base_; }
  uint16_t ctrl() const { return ctrl_; }

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

  // Bus-master IDE registers of this channel, reg 0..7: command, status, PRD table address (4..7).
  uint8_t bm_read(unsigned reg);
  void bm_write(unsigned reg, uint8_t value);

 private:
  static constexpr unsigned kSectorSize = 512;
  static constexpr unsigned kCdSectorSize = 2048;
  static constexpr unsigned kMaxMultiple = 16;

  struct Drive {
    bool present = false;
    bool atapi = false;
    bool signature = false;  // shows its reset signature in the task file when selected
    std::shared_ptr<block::Disk> disk;  // for ATAPI: the medium, null when the tray is empty
    uint64_t sectors = 0;               // in kSectorSize (ATA) or kCdSectorSize (ATAPI) units
    uint8_t sense_key = 0, asc = 0;     // ATAPI sense data for REQUEST SENSE
    bool unit_attention = false;        // ATAPI: medium changed, reported once as UNIT ATTENTION
    uint8_t media_event = 0;            // ATAPI: pending GESN media event code (2 new, 3 removal)
    bool prevent_removal = false;       // ATAPI: PREVENT ALLOW MEDIUM REMOVAL lock (guest eject only)
    uint8_t transfer_mode = 0;          // SET FEATURES 03h value: 0x2n = MWDMA n, 0x4n = UDMA n
    // Default CHS geometry (as reported in IDENTIFY words 1/3/6) and the current translation,
    // which INITIALIZE DEVICE PARAMETERS may change.
    uint16_t cylinders = 0, heads = 0, spt = 0;
    uint16_t cur_heads = 0, cur_spt = 0;
    uint8_t multiple = 0;  // sectors per DRQ block for READ/WRITE MULTIPLE, 0 = disabled
  };

  enum class Transfer { None, Identify, Read, Write, Packet, AtapiIn };

  Drive* selected();
  bool empty() const { return !drives_[0].present && !drives_[1].present; }
  uint8_t read_register(unsigned reg);
  void write_register(unsigned reg, uint8_t value);
  void write_control(uint8_t value);
  void execute(uint8_t command);

  // After reset / diagnostics: every device shows its signature; loads the selected one's.
  void set_signature();
  void load_signature(const Drive& d);
  bool decode_lba(bool lba48, uint64_t* lba);
  void encode_lba(bool lba48, uint64_t lba);
  uint32_t sector_count(bool lba48) const;
  // Task file progress after a block: next LBA and the residual sector count, so error recovery
  // (and software that chains commands from the returned task file) sees where the transfer stopped.
  void report_progress();

  void start_read(uint64_t lba, uint32_t count, unsigned block, bool lba48);
  void start_write(uint64_t lba, uint32_t count, unsigned block, bool lba48);
  bool load_read_block();
  void finish_write_block();
  bool verify(Drive& d, uint64_t lba, uint32_t count);
  void data_in(uint8_t* out, unsigned len);
  void data_out(const uint8_t* in, unsigned len);
  void build_identify(const Drive& d);

  // ATAPI (ide_atapi.cpp).
  void execute_atapi(Drive& d, uint8_t command);
  void build_identify_packet();
  void run_packet();
  // Sends `data` (cut to the CDB's allocation length) in byte-count-limited DRQ chunks, then completes.
  void atapi_reply(std::vector<uint8_t> data, size_t allocation);
  void atapi_start_read(uint64_t lba, uint32_t count);
  void atapi_next_chunk();
  void atapi_data_in(uint8_t* out, unsigned len);
  void atapi_ok();
  void atapi_abort();
  void atapi_error(uint8_t sense_key, uint8_t asc);
  bool atapi_need_media();

  // Bus-master DMA (ide_dma.cpp).
  enum class DmaKind { None, AtaRead, AtaWrite, AtapiIn };
  void start_dma(DmaKind kind, uint64_t offset, uint64_t bytes);  // offset: byte on the medium
  enum class DmaResult { Done, PrdShort, BusError, DiskError };
  void run_dma();
  // Moves up to len bytes of the pending transfer between the medium / reply buffer and guest gpa.
  DmaResult dma_chunk(uint64_t gpa, size_t len);
  void finish_dma(DmaResult result, bool prd_left);
  void set_transfer_mode(Drive& d);
  void identify_dma_words(uint16_t* w, const Drive& d) const;

  void complete(bool raise_irq = true);
  void abort_command(uint8_t error = 0x04);
  void raise_irq();
  void set_irq(bool level);

  const uint16_t base_;
  const uint16_t ctrl_;
  const IrqCallback on_irq_;
  const DmaMemory dma_memory_;

  std::mutex mutex_;
  Drive drives_[2];

  // Task file. hob_* hold the previous ("high order byte") writes for LBA48.
  uint8_t features_ = 0, nsector_ = 0, sector_ = 0, lcyl_ = 0, hcyl_ = 0;
  uint8_t hob_nsector_ = 0, hob_sector_ = 0, hob_lcyl_ = 0, hob_hcyl_ = 0;
  uint8_t select_ = 0xA0;
  uint8_t status_ = 0;
  uint8_t error_ = 0;
  uint8_t control_ = 0;
  bool irq_pending_ = false;  // INTRQ before nIEN gating
  bool irq_line_ = false;

  // PIO data phase. active_ is the drive that accepted the command: the guest may rewrite the device
  // register mid-transfer, but the data phase stays with the drive that started it.
  Transfer transfer_ = Transfer::None;
  Drive* active_ = nullptr;
  bool transfer_lba48_ = false;
  std::vector<uint8_t> buffer_;
  size_t buffer_pos_ = 0;
  uint64_t lba_ = 0;          // next sector to read/write
  uint32_t remaining_ = 0;    // sectors left in the command, including the buffered block
  unsigned block_ = 1;        // sectors per DRQ block
  unsigned block_sectors_ = 0;  // sectors in the current buffer

  // ATAPI packet state.
  uint8_t cdb_[12] = {};
  uint32_t byte_limit_ = 0;     // byte count limit the host gave with PACKET (DRQ chunk size), 0 = none
  size_t total_left_ = 0;       // bytes of the reply not yet read by the host
  size_t chunk_left_ = 0;       // bytes left in the current DRQ chunk
  uint32_t cd_sectors_left_ = 0;  // READ: sectors not yet loaded into buffer_ (lba_ is the next one)
  bool packet_dma_ = false;       // the PACKET command asked for a DMA data phase

  // Bus master.
  uint8_t bm_command_ = 0;  // bit 0 start, bit 3 direction (1 = device to memory)
  uint8_t bm_status_ = 0;   // bit 0 active, 1 error, 2 interrupt, 5/6 drive DMA capable
  uint32_t bm_prdt_ = 0;
  DmaKind dma_kind_ = DmaKind::None;  // a DMA command waiting for (or being served by) the engine
  uint64_t dma_bytes_ = 0;            // bytes the command still has to move
  uint64_t dma_total_ = 0;            // bytes the command asked for (ATA: for task file progress)
  uint64_t dma_offset_ = 0;           // next byte on the medium
  size_t dma_buffer_pos_ = 0;         // AtapiIn replies come from buffer_ first, then the medium
};

}  // namespace sandvm::devices
