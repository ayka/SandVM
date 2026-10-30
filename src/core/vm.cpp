#include "core/vm.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "core/acpi/tables.h"
#include "core/devices/cmos.h"
#include "core/devices/e1000.h"
#include "core/devices/fw_cfg.h"
#include "core/devices/i8042.h"
#include "core/devices/ide.h"
#include "core/devices/ioapic.h"
#include "core/devices/pic8259.h"
#include "core/devices/pit8254.h"
#include "core/devices/uhci.h"
#include "core/devices/usb.h"
#include "core/devices/vga.h"
#include "core/log.h"
#include "core/pci/pci.h"
#include "core/pci/e1000_pci.h"
#include "core/pci/piix.h"
#include "core/pci/std_vga.h"
#include "core/pci/uhci_pci.h"
#include "core/whpx.h"

namespace sandvm {
namespace {
// RAM up to this size fits below the PCI window in one piece. Larger RAM is split like QEMU's i440FX
// machine does: 3 GiB below 4 GiB (leaving a 1 GiB hole for PCI MMIO, the APICs and the firmware ROM)
// and the rest from 4 GiB up.
constexpr uint64_t kRamLimit = 0xE0000000;
constexpr uint64_t kSplitBelow4g = 0xC0000000;
// The end of high RAM must stay within the 36 physical address bits every 64-bit CPU has.
constexpr uint64_t kMaxRamEnd = 1ull << 36;
// CMOS 0x5B-0x5D count high RAM in 64 KiB units; a smaller tail would be mapped but never reported.
constexpr uint64_t kHighRamGranule = 64 * 1024;
constexpr uint64_t kFourGiB = 1ull << 32;
constexpr uint64_t kLegacyBiosEnd = 0x100000;
constexpr uint64_t kVgaWindowEnd = devices::Vga::kWindowBase + devices::Vga::kWindowSize;
constexpr unsigned kVgaDevfn = 0x10;  // 00:02.0, where QEMU puts it
constexpr unsigned kNicSlot = 3;      // 00:03.0, likewise
constexpr unsigned kUsbDevfn = 0x0A;  // 00:01.2, the PIIX3's USB function
// 0xC0000-0xFFFFF. The chipset only aliases the last 128 KiB of the ROM below 1 MiB, but SeaBIOS's
// 256 KiB build also runs code from 0xC0000-0xDFFFF, which it normally copies there itself after
// unlocking shadow RAM through the i440FX PAM registers. Without a northbridge that copy never
// happens, so we provide the post-copy state up front.
constexpr uint64_t kLegacyBiosMaxSize = 256 * 1024;
// The IOAPIC takes the APIC ID after the last CPU, and its ID register has 4 bits.
constexpr uint32_t kMaxCpus = 15;
}  // namespace

Vm::Vm(const VmConfig& config) {
  if (config.cpu_count == 0 || config.cpu_count > kMaxCpus || (config.cpu_count > 1 && !config.local_apic)) {
    // APs are started with INIT/SIPI IPIs, which only the hypervisor's local APIC delivers.
    throw std::invalid_argument("cpu_count must be 1, or 2..15 with local_apic");
  }
  if (config.ram_bytes == 0 || (config.ram_bytes & (GuestMemory::kPageSize - 1)) ||
      (config.ram_bytes > kRamLimit &&
       ((config.ram_bytes & (kHighRamGranule - 1)) || config.ram_bytes - kSplitBelow4g > kMaxRamEnd - kFourGiB))) {
    throw std::invalid_argument(
        "ram_bytes must be non-zero and page aligned; above 3.5 GiB, 64 KiB aligned and at most 63 GiB");
  }

  const auto& w = whpx::api();
  WHV_CAPABILITY cap{};
  UINT32 written = 0;
  whpx::check(w.WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &written),
              "WHvGetCapability(HypervisorPresent)");
  if (!cap.HypervisorPresent) {
    throw std::runtime_error("Hypervisor is not running. Enable 'Windows Hypervisor Platform' and reboot.");
  }

  whpx::check(w.WHvCreatePartition(&partition_), "WHvCreatePartition");
  try {
    WHV_PARTITION_PROPERTY prop{};
    prop.ProcessorCount = config.cpu_count;
    whpx::check(w.WHvSetPartitionProperty(partition_, WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop)),
                "WHvSetPartitionProperty(ProcessorCount)");
    if (config.local_apic) {
      prop = {};
      prop.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;
      whpx::check(w.WHvSetPartitionProperty(partition_, WHvPartitionPropertyCodeLocalApicEmulationMode, &prop,
                                            sizeof(prop)),
                  "WHvSetPartitionProperty(LocalApicEmulationMode)");
    }
    whpx::check(w.WHvSetupPartition(partition_), "WHvSetupPartition");

    memory_ = std::make_unique<GuestMemory>(partition_);
    ram_below_4g_ = config.ram_bytes > kRamLimit ? kSplitBelow4g : config.ram_bytes;
    ram_above_4g_ = config.ram_bytes - ram_below_4g_;
    memory_->add_ram(0, std::min(ram_below_4g_, devices::Vga::kWindowBase));
    if (ram_below_4g_ > kVgaWindowEnd) memory_->add_ram(kVgaWindowEnd, ram_below_4g_ - kVgaWindowEnd);
    if (ram_above_4g_) memory_->add_ram(kFourGiB, ram_above_4g_);
    local_apic_ = config.local_apic;

    for (uint32_t i = 0; i < config.cpu_count; ++i) vcpus_.push_back(std::make_unique<Vcpu>(*this, i));
    // Application processors wait for INIT/SIPI from the BSP, like after a real reset.
    for (uint32_t i = 1; i < config.cpu_count; ++i) vcpus_[i]->set_wait_for_sipi();

    // Without an IOAPIC the PIC INTR pin goes to the bootstrap processor only.
    // vcpus_ is fixed from here on, so the callback may read it from any thread.
    pic_ = std::make_shared<devices::Pic8259>([this] { vcpus_[0]->kick(); });
    io_.register_pio(devices::Pic8259::kMasterBase, 2, pic_);
    io_.register_pio(devices::Pic8259::kSlaveBase, 2, pic_);
    io_.register_pio(devices::Pic8259::kElcrBase, 2, pic_);
    cmos_ = std::make_shared<devices::CmosRtc>(ram_below_4g_, ram_above_4g_, [this](bool level) { set_irq(8, level); });
    io_.register_pio(devices::CmosRtc::kIndexPort, devices::CmosRtc::kPortCount, cmos_);
    if (!config.boot_order.empty()) {
      // QEMU's CMOS boot order, which SeaBIOS built for QEMU reads: one nibble per boot device class
      // (1 floppy, 2 disk, 3 CD, 4 network), first choice in the low nibble of 0x3D, second in its high
      // nibble, third in the high nibble of 0x38.
      uint8_t nibbles[3] = {};
      for (size_t i = 0; i < config.boot_order.size() && i < 3; ++i) {
        switch (config.boot_order[i]) {
          case 'a': nibbles[i] = 1; break;
          case 'c': nibbles[i] = 2; break;
          case 'd': nibbles[i] = 3; break;
          case 'n': nibbles[i] = 4; break;
          default: throw std::invalid_argument("boot order takes the letters a, c, d and n");
        }
      }
      cmos_->set_nvram(0x3D, static_cast<uint8_t>((nibbles[1] << 4) | nibbles[0]));
      cmos_->set_nvram(0x38, static_cast<uint8_t>(nibbles[2] << 4));
    }

    if (local_apic_) {
      ioapic_ = std::make_shared<devices::IoApic>(static_cast<uint8_t>(config.cpu_count),
                                                  [this](const devices::IoApic::Message& m) { return send_msi(m); });
      io_.register_mmio(devices::IoApic::kBase, devices::IoApic::kSize, ioapic_);
    }

    pit_ = std::make_shared<devices::Pit8254>([this](bool level) { set_irq(0, level); });
    io_.register_pio(devices::Pit8254::kBase, devices::Pit8254::kPortCount, pit_);
    io_.register_pio(devices::Pit8254::kPortB, 1, pit_);

    i8042_ = std::make_shared<devices::I8042>([this](unsigned irq, bool level) { set_irq(irq, level); },
                                              [this] {
                                                reset_requested_ = true;
                                                request_stop();
                                              });
    io_.register_pio(devices::I8042::kDataPort, 1, i8042_);
    io_.register_pio(devices::I8042::kCommandPort, 1, i8042_);
    io_.register_pio(devices::I8042::kPortA, 1, i8042_);

    pci_ = std::make_shared<pci::PciBus>([this] {
      reset_requested_ = true;
      request_stop();
    });
    io_.register_pio(pci::PciBus::kConfigAddress, pci::PciBus::kPortCount, pci_);
    pci_->attach(0x00, std::make_shared<pci::I440fxHostBridge>(io_));
    piix3_isa_ = std::make_shared<pci::Piix3Isa>(io_);
    pci_->attach(0x08, piix3_isa_);
    piix3_ide_ = std::make_shared<pci::Piix3Ide>(
        io_, [this](unsigned irq, bool level) { set_irq(irq, level); },
        [this](uint64_t gpa, void* data, size_t len, bool to_guest) {
          // Bus-master DMA reaches memory only (no DMA to MMIO), and cannot write the firmware ROM.
          if (to_guest) return !memory_->is_readonly(gpa) && memory_->write(gpa, data, len);
          return memory_->read(gpa, data, len);
        });
    pci_->attach(0x09, piix3_ide_);
    piix4_pm_ = std::make_shared<pci::Piix4Pm>(
        io_, [this](bool level) { set_irq(acpi::kSciIrq, level); },
        [this] {
          power_off_requested_ = true;
          request_stop();
        });
    pci_->attach(0x0B, piix4_pm_);
    if (config.vga_vram_bytes) {
      vga_ = std::make_shared<devices::Vga>(config.vga_vram_bytes);
      io_.register_pio(devices::Vga::kVgaPortBase, devices::Vga::kVgaPortCount, vga_);
      io_.register_pio(devices::Vga::kVbePortBase, devices::Vga::kVbePortCount, vga_);
      io_.register_mmio(devices::Vga::kWindowBase, devices::Vga::kWindowSize, vga_);
      pci_->attach(kVgaDevfn, std::make_shared<pci::StdVga>(io_, partition_, vga_));
    }
    if (config.nic) {
      nic_ = std::make_shared<devices::E1000>(
          config.mac, [this](bool level) { set_pci_irq(kNicSlot, 0, level); },
          [this](uint64_t gpa, void* data, size_t len, bool to_guest) {
            if (to_guest) return !memory_->is_readonly(gpa) && memory_->write(gpa, data, len);
            return memory_->read(gpa, data, len);
          });
      pci_->attach(kNicSlot << 3, std::make_shared<pci::E1000Pci>(io_, nic_));
    }

    if (config.usb_tablet) {
      uhci_ = std::make_shared<devices::Uhci>(
          [this](bool level) { set_pci_irq(1, 3, level); },  // PIIX3 USB uses INTD#
          [this](uint64_t gpa, void* data, size_t len, bool to_guest) {
            if (to_guest) return !memory_->is_readonly(gpa) && memory_->write(gpa, data, len);
            return memory_->read(gpa, data, len);
          });
      tablet_ = std::make_shared<devices::UsbTablet>();
      uhci_->attach(0, tablet_);
      pci_->attach(kUsbDevfn, std::make_shared<pci::Piix3Usb>(io_, uhci_));
    }

    // Firmware (SeaBIOS) learns the machine through fw_cfg and gets the ACPI tables from it. The table
    // files are rebuilt when selected, because the FADT must point at the PM block where the firmware
    // put it during PCI setup (SeaBIOS: 0xB000), which happens before it reads them.
    fw_cfg_ = std::make_shared<devices::FwCfg>();
    fw_cfg_->add_u64(devices::FwCfg::kRamSize, config.ram_bytes);
    fw_cfg_->add_u16(devices::FwCfg::kNbCpus, static_cast<uint16_t>(config.cpu_count));
    fw_cfg_->add_u16(devices::FwCfg::kMaxCpus, static_cast<uint16_t>(config.cpu_count));
    auto firmware_tables = [this] {
      const uint16_t base = piix4_pm_->pm_base();
      return acpi::build_tables(machine_info(base ? base : acpi::kPmBase));
    };
    const acpi::Tables initial = firmware_tables();
    fw_cfg_->add_file(acpi::kRsdpFile, initial.rsdp, [firmware_tables] { return firmware_tables().rsdp; });
    fw_cfg_->add_file(acpi::kTablesFile, initial.tables, [firmware_tables] { return firmware_tables().tables; });
    fw_cfg_->add_file(acpi::kLoaderFile, acpi::loader_script(initial),
                      [firmware_tables] { return acpi::loader_script(firmware_tables()); });
    io_.register_pio(devices::FwCfg::kSelectorPort, devices::FwCfg::kPortCount, fw_cfg_);
  } catch (...) {
    // io_ shares ownership of the PIT and RTC, so their threads would outlive pic_ unless stopped here.
    if (pit_) pit_->stop();
    if (cmos_) cmos_->stop();
    if (uhci_) uhci_->stop();
    vcpus_.clear();
    memory_.reset();
    w.WHvDeletePartition(partition_);
    throw;
  }
  SVM_INFO("partition ready: %u vCPU, %llu MiB RAM", config.cpu_count,
           static_cast<unsigned long long>(config.ram_bytes >> 20));
}

Vm::~Vm() {
  request_stop();
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  // The PIT and RTC threads raise IRQs, which kick vCPUs; they must be gone before the vCPUs are.
  if (pit_) pit_->stop();
  if (cmos_) cmos_->stop();
  if (uhci_) uhci_->stop();
  vcpus_.clear();
  memory_.reset();
  if (partition_) whpx::api().WHvDeletePartition(partition_);
}

void Vm::set_irq(unsigned irq, bool level) {
  pic_->set_irq(irq, level);
  // ISA IRQs also reach the IOAPIC, identity-mapped except the timer: IRQ0 is wired to pin 2 (the pin
  // the cascade would use), as on PC chipsets; the MADT reports that with an interrupt source override.
  if (ioapic_) ioapic_->set_irq(irq == 0 ? 2 : irq, level);
}

void Vm::set_pci_irq(unsigned slot, unsigned pin, bool level) {
  // Same swizzle as the _PRT: PIRQ = (slot + pin) mod 4. PCI interrupts are open-drain and shared: a
  // PIRQ line is asserted while any device on it asserts its pin (wired-OR).
  const unsigned pirq = (slot + pin) & 3;
  std::lock_guard lock(pci_irq_mutex_);
  pirq_sources_[pirq].set(slot * 4 + pin, level);
  const int irq = piix3_isa_->route(pirq);
  if (irq >= 0) set_irq(static_cast<unsigned>(irq), pirq_sources_[pirq].any());
}

acpi::MachineInfo Vm::machine_info(uint16_t pm_base) const {
  acpi::MachineInfo machine;
  machine.cpu_count = static_cast<uint32_t>(vcpus_.size());
  machine.ram_below_4g = ram_below_4g_;
  machine.ioapic = ioapic_ != nullptr;
  machine.pm_base = pm_base;
  return machine;
}

uint64_t Vm::install_acpi_tables() {
  // RSDP at 0xE0000 where a legacy scan finds it, the rest right behind it, 64-byte aligned.
  constexpr uint32_t kRsdpAddr = 0xE0000;
  constexpr uint32_t kTablesAddr = 0xE0040;
  acpi::Tables tables = acpi::build_tables(machine_info(acpi::kPmBase));
  acpi::link(tables, kTablesAddr);
  if (kTablesAddr + tables.tables.size() > 0x100000 ||
      !memory_->write(kRsdpAddr, tables.rsdp.data(), tables.rsdp.size()) ||
      !memory_->write(kTablesAddr, tables.tables.data(), tables.tables.size())) {
    throw std::runtime_error("ACPI tables do not fit at 0xE0000");
  }
  piix4_pm_->preset_pm_base(acpi::kPmBase);
  return kRsdpAddr;
}

void Vm::set_vga_bios(std::span<const uint8_t> rom) {
  if (!vga_) throw std::logic_error("set_vga_bios: the VM has no VGA");
  // SeaBIOS looks for "pci<vendor>,<device>.rom" before falling back to the device's ROM BAR.
  char name[32];
  std::snprintf(name, sizeof(name), "pci%04x,%04x.rom", pci::StdVga::kVendor, pci::StdVga::kDevice);
  fw_cfg_->add_file(name, std::vector<uint8_t>(rom.begin(), rom.end()));
}

void Vm::press_power_button() { piix4_pm_->press_power_button(); }

void Vm::ioapic_eoi(uint8_t vector) {
  if (ioapic_) ioapic_->eoi(vector);
}

bool Vm::send_msi(const devices::IoApic::Message& m) {
  WHV_INTERRUPT_CONTROL control{};
  switch (m.delivery_mode) {
    case 0: control.Type = WHvX64InterruptTypeFixed; break;
    case 1: control.Type = WHvX64InterruptTypeLowestPriority; break;
    case 4: control.Type = WHvX64InterruptTypeNmi; break;
    case 5: control.Type = WHvX64InterruptTypeInit; break;
    default:
      // SMI has no WHPX equivalent; ExtINT through the IOAPIC is only used for PIC virtual wire
      // mode via pin 0, which we route through LINT0 instead.
      SVM_WARN("ioapic: delivery mode %u not supported, interrupt dropped", m.delivery_mode);
      return true;  // retrying cannot help
  }
  control.DestinationMode = m.logical ? WHvX64InterruptDestinationModeLogical : WHvX64InterruptDestinationModePhysical;
  control.TriggerMode = m.level ? WHvX64InterruptTriggerModeLevel : WHvX64InterruptTriggerModeEdge;
  control.Destination = m.destination;
  control.Vector = m.vector;
  HRESULT hr = whpx::api().WHvRequestInterrupt(partition_, &control, sizeof(control));
  if (FAILED(hr)) {
    SVM_WARN("WHvRequestInterrupt(vector 0x%02x) failed: 0x%08lx", m.vector, static_cast<unsigned long>(hr));
    return false;
  }
  return true;
}

devices::IdeChannel& Vm::ide(unsigned channel) { return piix3_ide_->channel(channel); }

void Vm::load_firmware(std::span<const uint8_t> image) {
  {
    std::lock_guard lock(run_mutex_);
    if (started_) throw std::logic_error("load_firmware: the VM is already running");
  }
  if (image.empty() || image.size() % (64 * 1024) || image.size() > (16u << 20)) {
    throw std::invalid_argument("firmware image size must be a non-zero multiple of 64 KiB, at most 16 MiB");
  }
  memory_->add_rom(kFourGiB - image.size(), image);

  const size_t low_size = std::min<size_t>(image.size(), kLegacyBiosMaxSize);
  const auto low = image.last(low_size);
  if (!memory_->write(kLegacyBiosEnd - low_size, low.data(), low.size())) {
    throw std::invalid_argument("firmware needs at least 1 MiB of RAM for its legacy alias");
  }
  vcpus_.at(0)->set_reset_entry();
  SVM_INFO("firmware: %zu KiB at 0x%llx, legacy alias at 0x%llx", image.size() / 1024,
           static_cast<unsigned long long>(kFourGiB - image.size()),
           static_cast<unsigned long long>(kLegacyBiosEnd - low_size));
}

void Vm::start() {
  std::lock_guard lock(run_mutex_);
  if (started_) throw std::logic_error("Vm::start: a Vm can only be started once");
  started_ = true;
  stop_reasons_.assign(vcpus_.size(), StopReason::Canceled);
  for (uint32_t i = 0; i < vcpus_.size(); ++i) {
    threads_.emplace_back([this, i] {
      StopReason reason;
      try {
        reason = vcpus_[i]->run();
      } catch (const std::exception& e) {
        SVM_ERROR("vcpu%u: %s", i, e.what());
        reason = StopReason::Error;
      } catch (...) {
        SVM_ERROR("vcpu%u: unknown exception", i);
        reason = StopReason::Error;
      }
      stop_reasons_[i] = reason;  // published to wait() by the thread join
      // One processor stopping (error, triple fault, halt) stops the machine.
      if (reason != StopReason::Canceled) request_stop();
    });
  }
}

StopReason Vm::wait() {
  std::lock_guard lock(run_mutex_);
  if (!started_) throw std::logic_error("Vm::wait: Vm was not started");
  for (auto& t : threads_) {
    if (t.joinable()) t.join();
  }
  // The first processor that stopped for a reason of its own decides; otherwise it was a request.
  for (StopReason r : stop_reasons_) {
    if (r != StopReason::Canceled) return r;
  }
  if (reset_requested_) return StopReason::Reset;
  if (power_off_requested_) return StopReason::PowerOff;
  return StopReason::Canceled;
}

StopReason Vm::run() {
  start();
  return wait();
}

void Vm::request_stop() {
  for (auto& vcpu : vcpus_) vcpu->cancel();
}

void Vm::request_reset() {
  reset_requested_ = true;
  request_stop();
}

void Vm::set_paused(bool paused) {
  paused_ = paused;
  for (auto& vcpu : vcpus_) vcpu->set_paused(paused);
}

void Vm::access_physical(uint64_t gpa, bool is_write, unsigned size, uint8_t* data) {
  if (uint8_t* host = memory_->host_ptr(gpa, size)) {
    if (!is_write) std::memcpy(data, host, size);
    else if (!memory_->is_readonly(gpa)) std::memcpy(host, data, size);
    return;
  }
  if (is_write) {
    uint64_t value = 0;
    std::memcpy(&value, data, size);
    io_.mmio_write(gpa, size, value);
  } else {
    uint64_t value = io_.mmio_read(gpa, size);
    std::memcpy(data, &value, size);
  }
}

}  // namespace sandvm
