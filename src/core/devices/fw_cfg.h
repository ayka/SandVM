#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/io_bus.h"

namespace sandvm::devices {

// QEMU's firmware configuration interface, I/O variant without DMA (QEMU docs/specs/fw_cfg.rst):
// a 16-bit selector at 0x510 picks an item, 0x511 streams its bytes. SeaBIOS uses it to learn the
// CPU count and to fetch named files such as the ACPI tables and the "etc/table-loader" script that
// tells it where to put and how to link them.
//
// Thread-safe. A file may have a refresh callback, called (with the device lock held) whenever the
// guest selects it, for contents that depend on state the firmware sets up first (e.g. the PM base
// in the FADT). A refresh must return the same size: the directory already announced it.
class FwCfg : public PioDevice {
 public:
  static constexpr uint16_t kSelectorPort = 0x510;
  static constexpr uint16_t kDataPort = 0x511;
  static constexpr uint16_t kPortCount = 2;

  // Well-known keys.
  static constexpr uint16_t kSignature = 0x0000;
  static constexpr uint16_t kId = 0x0001;
  static constexpr uint16_t kRamSize = 0x0003;
  static constexpr uint16_t kNbCpus = 0x0005;
  static constexpr uint16_t kMaxCpus = 0x000F;
  static constexpr uint16_t kFileDir = 0x0019;

  using Refresh = std::function<std::vector<uint8_t>()>;

  FwCfg();

  void add_bytes(uint16_t key, std::vector<uint8_t> data);
  void add_u16(uint16_t key, uint16_t value);
  void add_u64(uint16_t key, uint64_t value);
  void add_file(const std::string& name, std::vector<uint8_t> data, Refresh refresh = {});

  uint32_t pio_read(uint16_t port, unsigned size) override;
  void pio_write(uint16_t port, unsigned size, uint32_t value) override;

 private:
  struct Item {
    std::vector<uint8_t> data;
    Refresh refresh;
  };
  void rebuild_directory();

  std::mutex mutex_;
  std::map<uint16_t, Item> items_;
  std::vector<std::pair<std::string, uint16_t>> files_;  // in directory order
  uint16_t selected_ = 0;
  size_t offset_ = 0;
};

}  // namespace sandvm::devices
