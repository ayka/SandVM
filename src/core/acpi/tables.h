#pragma once

#include <cstdint>
#include <vector>

namespace sandvm::acpi {

constexpr uint16_t kPmBase = 0x600;  // PIIX4 PMBA we program for guests started without firmware

// The machine the tables describe. Fixed resources (SCI on IRQ 9, IOAPIC at 0xFEC00000, PCI INTx
// swizzled onto IRQ 10/11) are constants shared with the device models in pci/piix.* and
// devices/ioapic.*. The PM block is wherever PMBA points (SeaBIOS uses 0xB000).
struct MachineInfo {
  uint32_t cpu_count = 1;
  uint64_t ram_below_4g = 0;  // RAM is [0, ram_below_4g); the PCI memory window starts above it
  bool ioapic = false;        // with a local APIC: emit the MADT
  uint16_t pm_base = kPmBase;
};
constexpr unsigned kSciIrq = 9;
constexpr uint16_t kApmControlPort = 0xB2;  // SMI_CMD: ACPI_ENABLE / ACPI_DISABLE are written here
constexpr uint8_t kAcpiEnable = 0xF1;
constexpr uint8_t kAcpiDisable = 0xF0;
constexpr uint16_t kResetPort = 0xCF9;
constexpr uint8_t kResetValue = 0x06;
// PCI INTx routing: PIRQ = (slot + pin) & 3; PIRQA/B -> IRQ 10, PIRQC/D -> IRQ 11. The PIIX3 routing
// registers are preset to this and _PRT reports it.
constexpr uint8_t kPirqIrq[4] = {10, 10, 11, 11};

// Relocatable ACPI tables in the two-blob form QEMU hands to firmware: the RSDP alone (it must sit in
// 0xE0000-0xFFFFF to be found by a scan) and everything else. Pointer fields hold offsets into `tables`
// until link() (or the firmware, following loader_script()) adds the address the blob was placed at.
struct Tables {
  std::vector<uint8_t> rsdp;    // 36 bytes, 16-byte aligned when placed
  std::vector<uint8_t> tables;  // XSDT, RSDT, FADT, FACS, DSDT, MADT; 64-byte aligned when placed
  struct Pointer {
    bool in_rsdp;
    uint32_t offset;
    uint8_t size;
  };
  struct Checksum {
    bool in_rsdp;
    uint32_t offset;  // the checksum byte
    uint32_t start;
    uint32_t length;
  };
  std::vector<Pointer> pointers;
  std::vector<Checksum> checksums;  // to recompute after pointers change, in this order
};

Tables build_tables(const MachineInfo& machine);
// Resolves every pointer for `tables` placed at guest address tables_addr and fixes the checksums.
void link(Tables& t, uint64_t tables_addr);
// QEMU's "etc/table-loader" script performing the same allocation and linking inside the firmware,
// for the fw_cfg files kRsdpFile and kTablesFile.
std::vector<uint8_t> loader_script(const Tables& t);
inline constexpr char kRsdpFile[] = "etc/acpi/rsdp";
inline constexpr char kTablesFile[] = "etc/acpi/tables";
inline constexpr char kLoaderFile[] = "etc/table-loader";

}  // namespace sandvm::acpi
