#pragma once

#include <windows.h>
#include <WinHvPlatform.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/acpi/tables.h"
#include "core/devices/ioapic.h"
#include "core/guest_memory.h"
#include "core/io_bus.h"
#include "core/vcpu.h"

namespace sandvm {

namespace devices {
class CmosRtc;
class E1000;
class FwCfg;
class I8042;
class IdeChannel;
class Pic8259;
class Pit8254;
class Uhci;
class UsbTablet;
class Vga;
}  // namespace devices
namespace pci {
class PciBus;
class Piix3Ide;
class Piix3Isa;
class Piix4Pm;
class StdVga;
}  // namespace pci

struct VmConfig {
  uint32_t cpu_count = 1;
  // More than one needs local_apic. Mapped from GPA 0, minus the VGA window. Limited to 3.5 GiB until
  // RAM is split around the PCI hole.
  uint64_t ram_bytes = 64ull << 20;
  // Let the hypervisor emulate an xAPIC per vCPU (MMIO at 0xFEE00000). Without it the CPUID APIC bit
  // is still set but nothing answers at that address.
  bool local_apic = false;
  // A PCI standard VGA (QEMU -vga std model) at 00:02.0 with this much VRAM; 0 = no display adapter.
  // Firmware needs its option ROM, see Vm::set_vga_bios().
  size_t vga_vram_bytes = 0;
  // An Intel 82540EM (e1000) network card at 00:03.0 with this MAC; connect a backend through nic().
  bool nic = false;
  std::array<uint8_t, 6> mac = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
  // A PIIX3 UHCI USB controller (00:01.2) with a HID tablet on port 1: absolute pointer input, so a
  // display window does not need to capture the mouse once the guest drives the tablet.
  bool usb_tablet = false;
  // Firmware boot order, up to three of 'c' (hard disk), 'd' (CD/DVD), 'a' (floppy), 'n' (network),
  // e.g. "dc"; empty leaves it to the firmware (SeaBIOS: disk first).
  std::string boot_order;
};

// Owns one WHPX partition plus its memory, I/O bus, the motherboard devices every PC has
// (8259 PIC pair, 8254 PIT, CMOS/RTC, 8042 keyboard controller), the i440FX/PIIX3 PCI chipset with its
// two legacy IDE channels, optionally a VGA, and the vCPU threads.
// RAM leaves the legacy VGA window 0xA0000-0xBFFFF out, as on every PC, whether or not a VGA exists.
// Destruction order matters: vCPU threads joined -> vCPUs -> memory mappings -> partition.
//
// Lifetime contract:
//  - The destructor stops and joins the vCPU thread itself, so it is safe to destroy a running Vm
//    from any thread except the vCPU thread (i.e. not from inside a device callback).
//  - Host threads that drive devices (e.g. serial input calling set_irq) must be stopped before
//    the Vm is destroyed, because IRQs end up calling into the vCPU.
class Vm {
 public:
  explicit Vm(const VmConfig& config);
  ~Vm();
  Vm(const Vm&) = delete;
  Vm& operator=(const Vm&) = delete;

  WHV_PARTITION_HANDLE partition() const { return partition_; }
  GuestMemory& memory() { return *memory_; }
  IoBus& io() { return io_; }
  devices::Pic8259& pic() { return *pic_; }
  devices::I8042& i8042() { return *i8042_; }  // host keyboard input goes here
  pci::PciBus& pci() { return *pci_; }
  devices::FwCfg& fw_cfg() { return *fw_cfg_; }  // QEMU-style firmware configuration (0x510)
  devices::Vga* vga() { return vga_.get(); }      // null without config.vga_vram_bytes
  devices::E1000* nic() { return nic_.get(); }    // null without config.nic
  devices::CmosRtc& cmos() { return *cmos_; }
  devices::UsbTablet* tablet() { return tablet_.get(); }  // null without config.usb_tablet
  // Hands the VGA option ROM (e.g. SeaBIOS's vgabios-stdvga.bin) to the firmware through fw_cfg, the
  // way QEMU does for devices without a ROM BAR. Needs a VGA. Call before start().
  void set_vga_bios(std::span<const uint8_t> rom);
  // PIIX3 IDE channel 0 (primary, 0x1F0, IRQ14) or 1 (secondary, 0x170, IRQ15). Attach disks before start().
  devices::IdeChannel& ide(unsigned channel);
  Vcpu& vcpu(uint32_t index) { return *vcpus_.at(index); }
  uint32_t cpu_count() const { return static_cast<uint32_t>(vcpus_.size()); }
  // RAM is [0, ram_below_4g) plus [4 GiB, 4 GiB + ram_above_4g).
  uint64_t ram_below_4g() const { return ram_below_4g_; }
  uint64_t ram_above_4g() const { return ram_above_4g_; }
  bool local_apic() const { return local_apic_; }

  // Thread-safe: drives ISA IRQ line 0..15 on the PIC and, with a local APIC, on the IOAPIC.
  void set_irq(unsigned irq, bool level);
  // A local APIC finished a level-triggered interrupt (WHvRunVpExitReasonX64ApicEoi).
  void ioapic_eoi(uint8_t vector);

  // For guests started without firmware: sets up the chipset the way firmware leaves it (PM I/O block
  // enabled) and copies the ACPI tables to 0xE0000. Returns the RSDP address. Call before start().
  uint64_t install_acpi_tables();
  // Presses the ACPI power button; a guest with ACPI support shuts down and wait() returns PowerOff.
  void press_power_button();

  // Installs a PC firmware image (e.g. SeaBIOS bios.bin; size a multiple of 64 KiB, at most 16 MiB):
  // mapped as ROM so that it ends at 4 GiB, its last 256 KiB (at most) also copied to end at 1 MiB, i.e.
  // the legacy BIOS alias plus the shadow copy firmware would make itself, and the BSP set to the
  // reset vector. Call before start().
  // The low copy is plain RAM, so firmware that unlocks shadow RAM through the chipset PAM registers
  // (which we do not model yet) still finds it writable.
  void load_firmware(std::span<const uint8_t> image);

  // Starts every vCPU on its own thread (APs wait for INIT/SIPI). A Vm runs at most once.
  void start();
  // Blocks until the vCPU thread exits and returns why it stopped (StopReason::Reset if the guest
  // asked for a CPU reset; the Vm cannot be restarted, the caller builds a new one).
  StopReason wait();
  // start() + wait().
  StopReason run();
  // Thread-safe; wait() returns StopReason::Canceled soon after.
  void request_stop();
  // Thread-safe: like the reset button, wait() returns StopReason::Reset.
  void request_reset();
  // Thread-safe: freezes / continues every vCPU (device timers keep running; their interrupts are
  // delivered after resume).
  void set_paused(bool paused);
  bool paused() const { return paused_; }

  // Physical access path used by the instruction emulator: RAM/ROM first, then MMIO devices.
  // Writes to ROM are silently dropped, like on real hardware.
  void access_physical(uint64_t gpa, bool is_write, unsigned size, uint8_t* data);

 private:
  bool send_msi(const devices::IoApic::Message& m);
  // Drives the ISA IRQ that INTx pin `pin` (0 = INTA#) of PCI slot `slot` is routed to (PIIX PIRQ swizzle).
  void set_pci_irq(unsigned slot, unsigned pin, bool level);
  acpi::MachineInfo machine_info(uint16_t pm_base) const;

  WHV_PARTITION_HANDLE partition_ = nullptr;
  uint64_t ram_below_4g_ = 0;
  uint64_t ram_above_4g_ = 0;
  bool local_apic_ = false;
  std::unique_ptr<GuestMemory> memory_;
  IoBus io_;
  std::shared_ptr<devices::Pic8259> pic_;
  std::shared_ptr<devices::Pit8254> pit_;  // has a timer thread; stopped explicitly before vcpus_ go
  std::shared_ptr<devices::CmosRtc> cmos_;  // likewise (IRQ8)
  std::shared_ptr<devices::I8042> i8042_;
  std::shared_ptr<devices::IoApic> ioapic_;  // only with config.local_apic
  std::shared_ptr<pci::PciBus> pci_;
  std::shared_ptr<pci::Piix3Isa> piix3_isa_;
  std::shared_ptr<pci::Piix3Ide> piix3_ide_;
  std::shared_ptr<pci::Piix4Pm> piix4_pm_;
  std::shared_ptr<devices::FwCfg> fw_cfg_;
  // Destroyed after the partition (members go after the destructor body), which still maps its VRAM.
  std::shared_ptr<devices::Vga> vga_;
  std::shared_ptr<devices::E1000> nic_;
  std::shared_ptr<devices::Uhci> uhci_;  // frame thread; stopped explicitly like the PIT
  std::shared_ptr<devices::UsbTablet> tablet_;
  std::mutex pci_irq_mutex_;
  std::array<std::bitset<128>, 4> pirq_sources_{};  // per PIRQ line: which slot*4+pin asserts it
  std::atomic<bool> power_off_requested_{false};
  std::atomic<bool> reset_requested_{false};
  std::atomic<bool> paused_{false};
  std::vector<std::unique_ptr<Vcpu>> vcpus_;

  std::mutex run_mutex_;  // guards started_ and threads_ against concurrent start()/wait()
  bool started_ = false;
  std::vector<std::thread> threads_;      // one per vCPU
  std::vector<StopReason> stop_reasons_;  // written by each vCPU thread before it exits
};

}  // namespace sandvm
