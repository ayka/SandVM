#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace sandvm::devices {

// Intel 82540EM Gigabit Ethernet controller (PRO/1000 MT Desktop), the "e1000" every PC hypervisor
// offers because Linux (e1000), Windows (inbox Intel PRO/1000 driver) and ReactOS ship drivers for it.
// Register model from the PCI/PCI-X Family of Gigabit Ethernet Controllers Software Developer's Manual
// (8254x SDM): legacy and extended (context/data) transmit descriptors with IP/TCP/UDP checksum offload
// and TCP segmentation (TSO), legacy receive descriptors with unicast/multicast/broadcast filtering,
// Microwire EEPROM (EECD bit-banging, plus EERD), the M88E1011 PHY behind MDIC, and the interrupt
// cause / mask registers driving INTA#.
//
// The link is always up at 1000 Mb/s full duplex. Interrupt moderation timers (ITR, RDTR, RADV, TIDV,
// TADV) are accepted and ignored: every receive and transmit completion interrupts at once.
//
// This class is the register file and the DMA engines; the PCI function (pci::E1000Pci) decodes BAR0
// (128 KiB MMIO) and BAR1 (IOADDR/IODATA) into read()/write().
//
// Thread-safe. Lock order: device -> IRQ / transmit / rx-space callbacks (all called with the lock held),
// so those callbacks must not call back into the device.
class E1000 {
 public:
  static constexpr uint32_t kMmioSize = 0x20000;
  static constexpr uint16_t kIoSize = 0x40;

  using Mac = std::array<uint8_t, 6>;
  using IrqCallback = std::function<void(bool level)>;
  // Guest memory for descriptor and buffer DMA; false = not RAM.
  using DmaMemory = std::function<bool(uint64_t gpa, void* data, size_t len, bool to_guest)>;
  using TxCallback = std::function<void(std::span<const uint8_t> frame)>;  // without FCS
  using Notify = std::function<void()>;

  E1000(const Mac& mac, IrqCallback on_irq, DmaMemory dma);

  // Backend hookup, before the VM runs. on_tx gets every frame the guest sends; on_rx_space is called
  // when the guest hands the card receive buffers (or enables receiving), the moment to retry
  // frames receive() refused.
  void set_backend(TxCallback on_tx, Notify on_rx_space);

  // A frame from the network (no FCS). Returns false if the receive ring has no room right now (the
  // caller keeps the frame and retries after on_rx_space); true if it was delivered or filtered/dropped.
  bool receive(std::span<const uint8_t> frame);

  // PCI command register bus-master enable; DMA is refused while clear.
  void set_bus_master(bool enabled) { bus_master_ = enabled; }

  uint32_t read(uint32_t offset, unsigned size);
  void write(uint32_t offset, unsigned size, uint32_t value);

 private:
  struct TxContext {
    uint8_t ipcss = 0, ipcso = 0, tucss = 0, tucso = 0;
    uint16_t ipcse = 0, tucse = 0;
    uint8_t tucmd = 0, hdrlen = 0;
    uint16_t mss = 0;
    bool valid = false;
  };

  void reset_locked();
  uint32_t read_reg(uint32_t index);  // index = offset / 4
  void write_reg(uint32_t index, uint32_t value);
  void set_eecd(uint32_t value);
  uint32_t get_eecd() const;
  void mdic(uint32_t value);
  void set_cause(uint32_t bits);
  void update_irq();
  bool accept_frame(std::span<const uint8_t> frame) const;
  void start_tx();
  void tx_descriptor(const uint8_t* desc);
  void tx_packet();
  void tx_tso();
  void emit(std::vector<uint8_t>& frame);
  bool dma(uint64_t gpa, void* data, size_t len, bool to_guest);

  const IrqCallback on_irq_;
  const DmaMemory dma_;
  TxCallback on_tx_;
  Notify on_rx_space_;
  std::atomic<bool> bus_master_{false};

  std::mutex mutex_;
  std::vector<uint32_t> regs_;  // kMmioSize / 4 entries
  std::array<uint16_t, 64> eeprom_{};
  std::array<uint16_t, 32> phy_{};
  bool irq_level_ = false;

  // Microwire state machine (93C46-style EEPROM, 6-bit addresses).
  uint32_t eecd_pins_ = 0;  // SK, CS, DI, REQ as the driver last wrote them
  uint32_t ee_in_ = 0;
  unsigned ee_bits_in_ = 0;
  unsigned ee_bit_out_ = 0;
  bool ee_reading_ = false;

  TxContext ctx_;
  std::vector<uint8_t> tx_buf_;
  uint8_t tx_popts_ = 0;
  bool tx_tse_ = false;
  bool tx_first_ = true;  // next data descriptor starts a packet
};

}  // namespace sandvm::devices
