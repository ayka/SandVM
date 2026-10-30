#include "core/acpi/tables.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include "core/acpi/aml.h"

namespace sandvm::acpi {
namespace {

constexpr uint32_t kLapicBase = 0xFEE00000;
constexpr uint32_t kIoApicBase = 0xFEC00000;
constexpr uint32_t kPciHoleEnd = 0xFEBFFFFF;  // below the IOAPIC
constexpr uint16_t kPm1EvtLen = 4, kPm1CntLen = 2, kPmTmrLen = 4, kGpe0Len = 4;

class Writer {
 public:
  explicit Writer(Bytes& out) : out_(out) {}
  void u8(uint8_t v) { out_.push_back(v); }
  void u16(uint16_t v) { le(v, 2); }
  void u32(uint32_t v) { le(v, 4); }
  void u64(uint64_t v) { le(v, 8); }
  void str(const char* s, size_t n) {
    for (size_t i = 0; i < n; ++i) out_.push_back(i < std::strlen(s) ? static_cast<uint8_t>(s[i]) : ' ');
  }
  void bytes(const Bytes& b) { out_.insert(out_.end(), b.begin(), b.end()); }
  // Generic Address Structure for a system I/O register block.
  void gas_io(uint16_t port, uint8_t bits) {
    u8(port ? 1 : 0);  // address space: system I/O
    u8(port ? bits : 0);
    u8(0);
    u8(0);  // access size: undefined (legacy)
    u64(port);
  }

 private:
  void le(uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
  Bytes& out_;
};

uint8_t checksum(const uint8_t* p, size_t n) {
  uint8_t sum = 0;
  for (size_t i = 0; i < n; ++i) sum = static_cast<uint8_t>(sum + p[i]);
  return static_cast<uint8_t>(-sum);
}

// Standard 36-byte header; length and checksum are patched by finish().
Bytes header(const char* signature, uint8_t revision) {
  Bytes t;
  Writer w(t);
  w.str(signature, 4);
  w.u32(0);
  w.u8(revision);
  w.u8(0);
  w.str("SANDVM", 6);
  w.str("SANDVM", 8);
  w.u32(1);
  w.str("SVM", 4);
  w.u32(1);
  return t;
}

Bytes finish(Bytes t) {
  const uint32_t len = static_cast<uint32_t>(t.size());
  std::memcpy(&t[4], &len, 4);
  t[9] = 0;
  t[9] = checksum(t.data(), t.size());
  return t;
}

Bytes build_dsdt(const MachineInfo& m) {
  using namespace acpi;
  // _PRT: every slot's INTA-D, swizzled onto the PIRQ lines, each hard-wired to an ISA IRQ (GSI), so the
  // same table works in PIC and APIC mode. Source 0 means "GSI in the last field", level/active-low.
  std::vector<Bytes> prt;
  for (unsigned slot = 1; slot < 32; ++slot) {
    for (unsigned pin = 0; pin < 4; ++pin) {
      prt.push_back(package({integer((uint64_t{slot} << 16) | 0xFFFF), integer(pin), integer(0),
                             integer(kPirqIrq[(slot + pin) & 3])}));
    }
  }
  const uint32_t hole_start = static_cast<uint32_t>(m.ram_below_4g);

  const Bytes isa = device("ISA", {
      name("_ADR", integer(0x00010000)),
      device("KBD", {name("_HID", eisa_id("PNP0303")),
                     name("_CRS", resource_template({io(0x60, 1), io(0x64, 1), irq_no_flags(1)}))}),
      device("MOU", {name("_HID", eisa_id("PNP0F13")), name("_CRS", resource_template({irq_no_flags(12)}))}),
      device("COM1", {name("_HID", eisa_id("PNP0501")), name("_UID", integer(1)),
                      name("_CRS", resource_template({io(0x3F8, 8), irq_no_flags(4)}))}),
      device("RTC", {name("_HID", eisa_id("PNP0B00")),
                     name("_CRS", resource_template({io(0x70, 2), irq_no_flags(8)}))}),
      device("PIC", {name("_HID", eisa_id("PNP0000")),
                     name("_CRS", resource_template({io(0x20, 2), io(0xA0, 2), io(0x4D0, 2), irq_no_flags(2)}))}),
      device("TMR", {name("_HID", eisa_id("PNP0100")),
                     name("_CRS", resource_template({io(0x40, 4), irq_no_flags(0)}))}),
      device("SPKR", {name("_HID", eisa_id("PNP0800")), name("_CRS", resource_template({io(0x61, 1)}))}),
  });

  const Bytes pci0 = device("PCI0", {
      name("_HID", eisa_id("PNP0A03")),
      name("_ADR", integer(0)),
      name("_UID", integer(0)),
      name("_CRS", resource_template({
                       word_bus_number(0, 0xFF),
                       io(0xCF8, 8),
                       word_io(0x0000, 0x0CF7),
                       word_io(0x0D00, 0xFFFF),
                       dword_memory(0x000A0000, 0x000BFFFF),
                       dword_memory(hole_start, kPciHoleEnd),
                   })),
      name("_PRT", package(prt)),
      isa,
  });

  Bytes cpus;  // Processor() objects, one term list
  for (uint32_t i = 0; i < m.cpu_count; ++i) {
    char n[5];
    std::snprintf(n, sizeof(n), "CP%02X", static_cast<unsigned>(i & 0xFF));
    const Bytes cpu = processor(n, static_cast<uint8_t>(i), 0, 0);
    cpus.insert(cpus.end(), cpu.begin(), cpu.end());
  }

  Bytes t = header("DSDT", 1);  // revision 1: integers are 32-bit, which every OS handles
  Writer w(t);
  w.bytes(scope("\\_SB", {pci0}));
  w.bytes(scope("\\_PR", {cpus}));
  // Sleep states: the PIIX4 encodes S5 (soft off) as SLP_TYP 0. Only S0 and S5 are offered.
  w.bytes(name("\\_S0", package({integer(0), integer(0), integer(0), integer(0)})));
  w.bytes(name("\\_S5", package({integer(0), integer(0), integer(0), integer(0)})));
  return finish(t);
}

// FADT with FIRMWARE_CTRL / DSDT / X_DSDT holding table offsets (fixed up by the caller's pointer list).
constexpr uint32_t kFadtFirmwareCtrl = 36, kFadtDsdt = 40, kFadtXDsdt = 140;

Bytes build_fadt(const MachineInfo& m, uint32_t facs, uint32_t dsdt) {
  const uint16_t kPmBase = m.pm_base;
  Bytes t = header("FACP", 3);
  Writer w(t);
  w.u32(facs);
  w.u32(dsdt);
  w.u8(0);  // reserved (INT_MODEL in ACPI 1.0)
  w.u8(0);  // preferred PM profile: unspecified
  w.u16(kSciIrq);
  w.u32(kApmControlPort);
  w.u8(kAcpiEnable);
  w.u8(kAcpiDisable);
  w.u8(0);  // S4BIOS_REQ
  w.u8(0);  // PSTATE_CNT
  w.u32(kPmBase);         // PM1a_EVT_BLK
  w.u32(0);               // PM1b_EVT_BLK
  w.u32(kPmBase + 0x04);  // PM1a_CNT_BLK
  w.u32(0);               // PM1b_CNT_BLK
  w.u32(0);               // PM2_CNT_BLK
  w.u32(kPmBase + 0x08);  // PM_TMR_BLK
  w.u32(kPmBase + 0x0C);  // GPE0_BLK (PIIX4 GPSTS/GPEN)
  w.u32(0);               // GPE1_BLK
  w.u8(kPm1EvtLen);
  w.u8(kPm1CntLen);
  w.u8(0);
  w.u8(kPmTmrLen);
  w.u8(kGpe0Len);
  w.u8(0);
  w.u8(0);
  w.u8(0);       // CST_CNT
  w.u16(0x0FFF);  // P_LVL2_LAT > 100: C2 not supported
  w.u16(0x0FFF);  // P_LVL3_LAT > 1000: C3 not supported
  w.u16(0);
  w.u16(0);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  w.u8(0x32);    // CENTURY: CMOS index of the century byte
  w.u16(0x0003);  // IAPC_BOOT_ARCH: LEGACY_DEVICES | 8042
  w.u8(0);
  // Flags: WBINVD, PROC_C1, SLP_BUTTON (none), RTC_S4, RESET_REG_SUP. PWR_BUTTON stays clear: the power
  // button is the fixed-feature one (PM1 PWRBTN_STS/EN) that Piix4Pm::press_power_button() drives.
  w.u32((1u << 0) | (1u << 2) | (1u << 5) | (1u << 7) | (1u << 10));
  w.gas_io(kResetPort, 8);
  w.u8(kResetValue);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  w.u64(0);  // X_FIRMWARE_CTRL: FIRMWARE_CTRL above already points at the FACS, and only one may be set
  w.u64(dsdt);
  w.gas_io(kPmBase, kPm1EvtLen * 8);
  w.gas_io(0, 0);
  w.gas_io(kPmBase + 0x04, kPm1CntLen * 8);
  w.gas_io(0, 0);
  w.gas_io(0, 0);
  w.gas_io(kPmBase + 0x08, kPmTmrLen * 8);
  w.gas_io(kPmBase + 0x0C, kGpe0Len * 8);
  w.gas_io(0, 0);
  if (t.size() != 244) throw std::logic_error("FADT revision 3 must be 244 bytes");
  return finish(t);
}

Bytes build_facs() {
  Bytes t;
  Writer w(t);
  w.str("FACS", 4);
  w.u32(64);
  w.u32(0);  // hardware signature
  w.u32(0);  // firmware waking vector
  w.u32(0);  // global lock
  w.u32(0);  // flags
  w.u64(0);  // X firmware waking vector
  w.u8(1);   // version
  t.resize(64, 0);
  return t;  // no checksum field
}

Bytes build_madt(const MachineInfo& m) {
  Bytes t = header("APIC", 1);
  Writer w(t);
  w.u32(kLapicBase);
  w.u32(1);  // PCAT_COMPAT: dual 8259s present
  for (uint32_t i = 0; i < m.cpu_count; ++i) {
    w.u8(0);  // processor local APIC
    w.u8(8);
    w.u8(static_cast<uint8_t>(i));  // ACPI processor ID (matches Processor() in the DSDT)
    w.u8(static_cast<uint8_t>(i));  // APIC ID = VP index on WHPX
    w.u32(1);                       // enabled
  }
  w.u8(1);  // I/O APIC
  w.u8(12);
  w.u8(static_cast<uint8_t>(m.cpu_count));  // IOAPIC ID after the local APIC IDs
  w.u8(0);
  w.u32(kIoApicBase);
  w.u32(0);  // GSI base
  auto override_irq = [&](uint8_t irq, uint32_t gsi, uint16_t flags) {
    w.u8(2);
    w.u8(10);
    w.u8(0);  // bus: ISA
    w.u8(irq);
    w.u32(gsi);
    w.u16(flags);
  };
  override_irq(0, 2, 0x0000);        // timer on pin 2, conforming polarity/trigger
  override_irq(kSciIrq, kSciIrq, 0x000D);  // SCI: active high, level
  w.u8(4);  // local APIC NMI: LINT1 of all processors
  w.u8(6);
  w.u8(0xFF);
  w.u16(0);
  w.u8(1);
  return finish(t);
}

void align(Bytes& blob, size_t a) { blob.resize((blob.size() + a - 1) / a * a, 0); }

}  // namespace

Tables build_tables(const MachineInfo& machine) {
  Tables out;
  Bytes& blob = out.tables;
  auto place = [&](const Bytes& table, size_t alignment) {
    align(blob, alignment);
    const uint32_t off = static_cast<uint32_t>(blob.size());
    blob.insert(blob.end(), table.begin(), table.end());
    return off;
  };
  auto pointer = [&](uint32_t off, uint8_t size) { out.pointers.push_back({false, off, size}); };
  auto table_checksum = [&](uint32_t off) {
    uint32_t len;
    std::memcpy(&len, &blob[off + 4], 4);
    out.checksums.push_back({false, off + 9, off, len});
  };

  const uint32_t facs = place(build_facs(), 64);  // the blob itself is 64-byte aligned
  const uint32_t dsdt = place(build_dsdt(machine), 16);
  table_checksum(dsdt);
  const uint32_t fadt = place(build_fadt(machine, facs, dsdt), 16);
  pointer(fadt + kFadtFirmwareCtrl, 4);
  pointer(fadt + kFadtDsdt, 4);
  pointer(fadt + kFadtXDsdt, 8);
  table_checksum(fadt);
  std::vector<uint32_t> entries{fadt};
  if (machine.ioapic) {
    entries.push_back(place(build_madt(machine), 16));
    table_checksum(entries.back());
  }

  Bytes rsdt = header("RSDT", 1);
  Bytes xsdt = header("XSDT", 1);
  {
    Writer r(rsdt), x(xsdt);
    for (uint32_t e : entries) {
      r.u32(e);
      x.u64(e);
    }
  }
  const uint32_t rsdt_off = place(finish(rsdt), 16);
  const uint32_t xsdt_off = place(finish(xsdt), 16);
  for (size_t i = 0; i < entries.size(); ++i) {
    pointer(rsdt_off + 36 + static_cast<uint32_t>(4 * i), 4);
    pointer(xsdt_off + 36 + static_cast<uint32_t>(8 * i), 8);
  }
  table_checksum(rsdt_off);
  table_checksum(xsdt_off);

  Bytes& rsdp = out.rsdp;
  Writer w(rsdp);
  w.str("RSD PTR ", 8);
  w.u8(0);
  w.str("SANDVM", 6);
  w.u8(2);  // ACPI 2.0+ RSDP
  w.u32(rsdt_off);
  w.u32(36);
  w.u64(xsdt_off);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  w.u8(0);
  out.pointers.push_back({true, 16, 4});
  out.pointers.push_back({true, 24, 8});
  out.checksums.push_back({true, 8, 0, 20});   // ACPI 1.0 checksum
  out.checksums.push_back({true, 32, 0, 36});  // extended checksum, covers the first one
  return out;
}

void link(Tables& t, uint64_t tables_addr) {
  for (const auto& p : t.pointers) {
    uint8_t* field = (p.in_rsdp ? t.rsdp.data() : t.tables.data()) + p.offset;
    uint64_t value = 0;
    std::memcpy(&value, field, p.size);
    value += tables_addr;
    std::memcpy(field, &value, p.size);
  }
  for (const auto& c : t.checksums) {
    uint8_t* blob = c.in_rsdp ? t.rsdp.data() : t.tables.data();
    blob[c.offset] = 0;
    blob[c.offset] = checksum(blob + c.start, c.length);
  }
}

std::vector<uint8_t> loader_script(const Tables& t) {
  // QEMU hw/acpi/bios-linker-loader.c: 128-byte commands, names in 56-byte fields.
  constexpr size_t kEntry = 128, kName = 56;
  constexpr uint32_t kAllocate = 1, kAddPointer = 2, kAddChecksum = 3;
  constexpr uint8_t kZoneHigh = 1, kZoneFseg = 2;
  Bytes script;
  auto entry = [&](uint32_t command) {
    script.resize(script.size() + kEntry, 0);
    uint8_t* e = &script[script.size() - kEntry];
    std::memcpy(e, &command, 4);
    return e + 4;
  };
  auto file = [](uint8_t* dst, const char* name) { std::memcpy(dst, name, std::strlen(name) + 1); };
  auto u32 = [](uint8_t* dst, uint32_t v) { std::memcpy(dst, &v, 4); };
  {
    uint8_t* e = entry(kAllocate);
    file(e, kTablesFile);
    u32(e + kName, 64);
    e[kName + 4] = kZoneHigh;
  }
  {
    uint8_t* e = entry(kAllocate);
    file(e, kRsdpFile);
    u32(e + kName, 16);
    e[kName + 4] = kZoneFseg;
  }
  for (const auto& p : t.pointers) {
    uint8_t* e = entry(kAddPointer);
    file(e, p.in_rsdp ? kRsdpFile : kTablesFile);
    file(e + kName, kTablesFile);
    u32(e + 2 * kName, p.offset);
    e[2 * kName + 4] = p.size;
  }
  for (const auto& c : t.checksums) {
    uint8_t* e = entry(kAddChecksum);
    file(e, c.in_rsdp ? kRsdpFile : kTablesFile);
    u32(e + kName, c.offset);
    u32(e + kName + 4, c.start);
    u32(e + kName + 8, c.length);
  }
  return script;
}

}  // namespace sandvm::acpi
