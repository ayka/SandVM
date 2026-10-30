#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/devices/usb.h"
#include "core/io_bus.h"

namespace sandvm::devices {

// Intel UHCI USB 1.1 host controller (the PIIX3's function 2, "Universal Host Controller Interface
// Design Guide" rev 1.1) with its two root hub ports. The 32-byte register block is decoded at any
// 32-byte aligned I/O base (offset = port & 31).
//
// A host thread runs the schedule once per 1 ms frame while the controller runs and may master the
// bus: frame list entry -> queue heads / transfer descriptors in guest memory, executed synchronously
// against the attached devices (no isochronous transfers, no hubs). Queue heads follow the depth-first
// bit like real hardware; a QH seen twice in a frame (Linux's bandwidth reclamation loop) ends it.
//
// Thread-safe. The IRQ callback is called with the controller lock held (device -> PIC order).
class Uhci : public PioDevice {
 public:
  static constexpr unsigned kPorts = 2;
  static constexpr uint16_t kIoSize = 32;

  using IrqCallback = std::function<void(bool level)>;
  using DmaMemory = std::function<bool(uint64_t gpa, void* data, size_t len, bool to_guest)>;

  Uhci(IrqCallback on_irq, DmaMemory dma);
  ~Uhci() override;
  Uhci(const Uhci&) = delete;
  Uhci& operator=(const Uhci&) = delete;

  // Plugs a full-speed device into root port 0 or 1. Call before the VM runs.
  void attach(unsigned port, std::shared_ptr<UsbDevice> device);
  // PCI command register bit 2: without it the controller cannot read its schedule.
  void set_bus_master(bool enabled);
  // Stops the frame thread; no IRQ callback runs from it after this returns. Idempotent.
  void stop();

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  enum class TdResult { Ok, Short, Nak, Error, Inactive };
  struct Port {
    uint16_t sc = 0x0080;  // PORTSC; bit 7 reads as one
    std::shared_ptr<UsbDevice> device;
  };

  uint16_t read16(unsigned off);
  void write16(unsigned off, uint16_t value);
  void reset_locked();
  void update_irq_locked();
  void frame_thread();
  void run_frame_locked();
  void run_queue_locked(uint32_t qh, uint32_t element, int* budget);
  TdResult run_td_locked(uint32_t addr);
  UsbDevice* find_device_locked(uint8_t address);
  bool read_dword(uint32_t gpa, uint32_t* value);
  bool write_dword(uint32_t gpa, uint32_t value);

  const IrqCallback on_irq_;
  const DmaMemory dma_;

  std::mutex mutex_;
  uint16_t cmd_ = 0;
  uint16_t status_ = 0;
  uint8_t status2_ = 0;  // bit 0: a TD with IOC completed, bit 1: short packet (QEMU's split of USBINT)
  uint16_t intr_ = 0;
  uint16_t frnum_ = 0;
  uint32_t flbase_ = 0;
  uint8_t sofmod_ = 64;
  std::array<Port, kPorts> ports_{};
  bool bus_master_ = false;
  bool irq_line_ = false;
  std::vector<uint8_t> data_;  // TD data bounce buffer

  bool quit_ = false;
  HANDLE timer_ = nullptr;
  HANDLE wake_ = nullptr;  // auto-reset: started, or stop()
  std::thread thread_;
};

}  // namespace sandvm::devices
