#include "cli/selftest.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/block/disk.h"
#include "core/devices/ide.h"
#include "core/devices/serial16550.h"
#include "core/log.h"
#include "core/vm.h"

namespace sandvm::cli {
namespace {

constexpr uint16_t kLoadAddr = 0x7C00;

// Polled output, then CLI; HLT. Loaded at 0000:7C00 (hand-assembled):
//   7C00  BA F8 03     mov  dx, 0x3F8
//   7C03  BE 0F 7C     mov  si, msg        ; 0x7C0F
//   7C06  AC           lodsb
//   7C07  84 C0        test al, al
//   7C09  74 03        jz   7C0E
//   7C0B  EE           out  dx, al
//   7C0C  EB F8        jmp  7C06
//   7C0E  F4           hlt
//   7C0F  msg: "Hello from SandVM guest!\n", 0
constexpr char kHelloExpected[] = "Hello from SandVM guest!\n";

std::string build_hello_guest() {
  std::string image = "\xBA\xF8\x03\xBE\x0F\x7C\xAC\x84\xC0\x74\x03\xEE\xEB\xF8\xF4";
  image += kHelloExpected;
  image.push_back('\0');
  return image;
}

// Interrupt-driven output: programs both 8259s (master base 0x08), unmasks IRQ4, enables the UART
// THRE interrupt with OUT2, then idles in STI; HLT. Each IRQ4 sends one byte; the THR write raises
// THRE again, which the PIC delivers after the EOI. Exercises edge latching, ISR/EOI, injection
// with IF/STI-shadow checks, the interrupt window exit and HLT wakeup. Hand-assembled at 0000:7C00:
//   7C00  FA                 cli
//   7C01  C7 06 30 00 4B 7C  mov  word [0x30], isr      ; IVT[0x0C] offset
//   7C07  C7 06 32 00 00 00  mov  word [0x32], 0        ; IVT[0x0C] segment
//   7C0D  B0 11 E6 20 E6 A0  ICW1 (edge, cascade, ICW4) to both PICs
//   7C13  B0 08 E6 21        ICW2 master = 0x08
//   7C17  B0 70 E6 A1        ICW2 slave  = 0x70
//   7C1B  B0 04 E6 21        ICW3 master
//   7C1F  B0 02 E6 A1        ICW3 slave
//   7C23  B0 01 E6 21 E6 A1  ICW4 8086 mode to both
//   7C29  B0 EF E6 21        OCW1 master: only IRQ4 unmasked
//   7C2D  B0 FF E6 A1        OCW1 slave: all masked
//   7C31  BA FC 03 B0 08 EE  MCR = OUT2
//   7C37  BE 69 7C           mov  si, msg
//   7C3A  BA F9 03 B0 02 EE  IER = ETBEI (THR is empty, so IRQ4 is raised at once)
//   7C40  FB                 sti
//   7C41  F4                 idle: hlt
//   7C42  80 3E 68 7C 00     cmp  byte [done], 0
//   7C47  74 F8              je   idle
//   7C49  FA F4              cli; hlt
//   7C4B  50 52              isr: push ax; push dx
//   7C4D  BA FA 03 EC        in   al, 0x3FA            ; IIR read acknowledges THRE
//   7C51  AC                 lodsb
//   7C52  84 C0              test al, al
//   7C54  74 06              jz   finished
//   7C56  BA F8 03 EE        out  0x3F8, al
//   7C5A  EB 05              jmp  eoi
//   7C5C  C6 06 68 7C 01     finished: mov byte [done], 1
//   7C61  B0 20 E6 20        eoi: non-specific EOI to master
//   7C65  5A 58 CF           pop dx; pop ax; iret
//   7C68  done: 0
//   7C69  msg: "IRQ OK\n", 0
constexpr char kIrqExpected[] = "IRQ OK\n";

std::string build_irq_guest() {
  static const unsigned char kCode[] = {
      0xFA, 0xC7, 0x06, 0x30, 0x00, 0x4B, 0x7C, 0xC7, 0x06, 0x32, 0x00, 0x00, 0x00,  // 7C00
      0xB0, 0x11, 0xE6, 0x20, 0xE6, 0xA0,                                            // 7C0D
      0xB0, 0x08, 0xE6, 0x21, 0xB0, 0x70, 0xE6, 0xA1,                                // 7C13
      0xB0, 0x04, 0xE6, 0x21, 0xB0, 0x02, 0xE6, 0xA1,                                // 7C1B
      0xB0, 0x01, 0xE6, 0x21, 0xE6, 0xA1,                                            // 7C23
      0xB0, 0xEF, 0xE6, 0x21, 0xB0, 0xFF, 0xE6, 0xA1,                                // 7C29
      0xBA, 0xFC, 0x03, 0xB0, 0x08, 0xEE,                                            // 7C31
      0xBE, 0x69, 0x7C,                                                              // 7C37
      0xBA, 0xF9, 0x03, 0xB0, 0x02, 0xEE,                                            // 7C3A
      0xFB, 0xF4, 0x80, 0x3E, 0x68, 0x7C, 0x00, 0x74, 0xF8, 0xFA, 0xF4,              // 7C40
      0x50, 0x52, 0xBA, 0xFA, 0x03, 0xEC, 0xAC, 0x84, 0xC0, 0x74, 0x06,              // 7C4B
      0xBA, 0xF8, 0x03, 0xEE, 0xEB, 0x05,                                            // 7C56
      0xC6, 0x06, 0x68, 0x7C, 0x01,                                                  // 7C5C
      0xB0, 0x20, 0xE6, 0x20, 0x5A, 0x58, 0xCF,                                      // 7C61
      0x00,                                                                          // 7C68 done
  };
  static_assert(sizeof(kCode) == 0x69, "msg must start at 0x7C69");
  std::string image(reinterpret_cast<const char*>(kCode), sizeof(kCode));
  image += kIrqExpected;
  image.push_back('\0');
  return image;
}

// Timer interrupts: master PIC base 0x08, only IRQ0 unmasked, PIT channel 0 in mode 2 with count
// 11932 (~100 Hz). The ISR prints 'T' per tick; after 5 ticks the guest does CLI; HLT.
// Hand-assembled at 0000:7C00:
//   7C00  FA                 cli
//   7C01  C7 06 20 00 38 7C  mov  word [0x20], isr      ; IVT[0x08] offset
//   7C07  C7 06 22 00 00 00  mov  word [0x22], 0        ; IVT[0x08] segment
//   7C0D  B0 11 E6 20        ICW1 master
//   7C11  B0 08 E6 21        ICW2 master = 0x08
//   7C15  B0 04 E6 21        ICW3 master
//   7C19  B0 01 E6 21        ICW4 8086 mode
//   7C1D  B0 FE E6 21        OCW1: only IRQ0 unmasked
//   7C21  B0 34 E6 43        PIT control: channel 0, LSB+MSB, mode 2, binary
//   7C25  B0 9C E6 40        count LSB (0x2E9C = 11932)
//   7C29  B0 2E E6 40        count MSB
//   7C2D  FB                 sti
//   7C2E  F4                 idle: hlt
//   7C2F  80 3E 4B 7C 05     cmp  byte [ticks], 5
//   7C34  72 F8              jb   idle
//   7C36  FA F4              cli; hlt
//   7C38  50 52              isr: push ax; push dx
//   7C3A  FE 06 4B 7C        inc  byte [ticks]
//   7C3E  BA F8 03 B0 54 EE  out  0x3F8, 'T'
//   7C44  B0 20 E6 20        non-specific EOI
//   7C48  5A 58 CF           pop dx; pop ax; iret
//   7C4B  ticks: 0
constexpr char kPitExpected[] = "TTTTT";

std::string build_pit_guest() {
  static const unsigned char kCode[] = {
      0xFA, 0xC7, 0x06, 0x20, 0x00, 0x38, 0x7C, 0xC7, 0x06, 0x22, 0x00, 0x00, 0x00,  // 7C00
      0xB0, 0x11, 0xE6, 0x20, 0xB0, 0x08, 0xE6, 0x21,                                // 7C0D
      0xB0, 0x04, 0xE6, 0x21, 0xB0, 0x01, 0xE6, 0x21,                                // 7C15
      0xB0, 0xFE, 0xE6, 0x21,                                                        // 7C1D
      0xB0, 0x34, 0xE6, 0x43, 0xB0, 0x9C, 0xE6, 0x40, 0xB0, 0x2E, 0xE6, 0x40,        // 7C21
      0xFB, 0xF4, 0x80, 0x3E, 0x4B, 0x7C, 0x05, 0x72, 0xF8, 0xFA, 0xF4,              // 7C2D
      0x50, 0x52, 0xFE, 0x06, 0x4B, 0x7C,                                            // 7C38
      0xBA, 0xF8, 0x03, 0xB0, 0x54, 0xEE,                                            // 7C3E
      0xB0, 0x20, 0xE6, 0x20, 0x5A, 0x58, 0xCF,                                      // 7C44
      0x00,                                                                          // 7C4B ticks
  };
  static_assert(sizeof(kCode) == 0x4C, "ticks must be at 0x7C4B");
  return std::string(reinterpret_cast<const char*>(kCode), sizeof(kCode));
}

// Boot sector for the firmware test (hand-assembled, loaded by the BIOS at 0000:7C00):
//   7C00  FA FC              cli; cld        ; CLI so that the final HLT stops the VM
//   7C02  31 C0 8E D8        xor ax, ax; mov ds, ax
//   7C06  BA F8 03           mov  dx, 0x3F8
//   7C09  BE 15 7C           mov  si, msg
//   7C0C  AC                 lodsb
//   7C0D  84 C0              test al, al
//   7C0F  74 03              jz   7C14
//   7C11  EE                 out  dx, al
//   7C12  EB F8              jmp  7C0C
//   7C14  F4                 hlt
//   7C15  msg: "Booted from disk!\n", 0
//   01FE  55 AA              boot signature
constexpr char kBootExpected[] = "Booted from disk!\n";

std::vector<uint8_t> build_boot_disk() {
  static const unsigned char kCode[] = {
      0xFA, 0xFC, 0x31, 0xC0, 0x8E, 0xD8, 0xBA, 0xF8, 0x03, 0xBE, 0x15, 0x7C,  // 7C00
      0xAC, 0x84, 0xC0, 0x74, 0x03, 0xEE, 0xEB, 0xF8, 0xF4,                    // 7C0C
  };
  static_assert(sizeof(kCode) == 0x15, "msg must start at 0x7C15");
  std::vector<uint8_t> disk(1u << 20);  // 1 MiB: 2048 sectors
  std::memcpy(disk.data(), kCode, sizeof(kCode));
  std::memcpy(disk.data() + sizeof(kCode), kBootExpected, sizeof(kBootExpected));
  disk[510] = 0x55;
  disk[511] = 0xAA;
  return disk;
}

// Prepares a fresh VM (memory contents, entry point, extra devices). Returns false on failure.
using GuestSetup = std::function<bool(Vm&)>;

bool run_vm(const char* name, uint64_t ram_bytes, const GuestSetup& setup, const char* expected, int timeout_s);

bool run_guest(const char* name, const std::string& image, const char* expected) {
  return run_vm(
      name, 1ull << 20,
      [&](Vm& vm) {
        if (!vm.memory().write(kLoadAddr, image.data(), image.size())) return false;
        vm.vcpu(0).set_real_mode_entry(0, kLoadAddr, kLoadAddr);
        return true;
      },
      expected, 5);
}

bool run_bios_disk(const std::vector<uint8_t>& firmware) {
  return run_vm(
      "bios-disk", 16ull << 20,
      [&](Vm& vm) {
        vm.ide(0).attach(0, std::make_shared<block::MemoryDisk>(build_boot_disk()));
        vm.load_firmware(firmware);
        return true;
      },
      kBootExpected, 30);  // SeaBIOS waits ~2.5 s at its boot menu prompt
}

bool run_vm(const char* name, uint64_t ram_bytes, const GuestSetup& setup, const char* expected, int timeout_s) {
  VmConfig config;
  config.ram_bytes = ram_bytes;
  Vm vm(config);

  std::string serial_out;  // written by the vCPU thread, read after wait() joins it
  auto com1 = std::make_shared<devices::Serial16550>(
      devices::Serial16550::kCom1Base,
      [&](uint8_t byte) {
        serial_out.push_back(static_cast<char>(byte));
        std::fputc(byte, stdout);
      },
      [&vm](bool level) { vm.set_irq(4, level); });
  vm.io().register_pio(devices::Serial16550::kCom1Base, devices::Serial16550::kPortCount, com1);

  if (!setup(vm)) {
    SVM_ERROR("selftest %s: could not set up the guest", name);
    return false;
  }

  // A guest waiting for an interrupt that never comes would idle in HLT forever.
  std::mutex mutex;
  std::condition_variable finished_cv;
  bool finished = false;
  bool timed_out = false;
  std::thread watchdog([&] {
    std::unique_lock lock(mutex);
    if (!finished_cv.wait_for(lock, std::chrono::seconds(timeout_s), [&] { return finished; })) {
      timed_out = true;
      vm.request_stop();
    }
  });
  StopReason reason = vm.run();
  {
    std::lock_guard lock(mutex);
    finished = true;
  }
  finished_cv.notify_one();
  watchdog.join();
  std::fflush(stdout);
  if (timed_out) SVM_ERROR("selftest %s: timed out after %d s", name, timeout_s);

  const bool ok = reason == StopReason::Halted && serial_out == expected;
  SVM_INFO("selftest %s: stop=%s, serial=%zu bytes -> %s", name, to_string(reason), serial_out.size(),
           ok ? "PASS" : "FAIL");
  return ok;
}

}  // namespace

int run_selftest(const std::vector<uint8_t>& firmware) {
  bool ok = run_guest("polled", build_hello_guest(), kHelloExpected);
  ok = run_guest("irq", build_irq_guest(), kIrqExpected) && ok;
  ok = run_guest("pit", build_pit_guest(), kPitExpected) && ok;

  // Same guest with the idle HLT at 7C2E replaced by NOP: it now spins without any VM exit, so each
  // IRQ0 can only arrive through Vcpu::kick() cancelling a running WHvRunVirtualProcessor from the
  // PIT thread. This is the path a lost cancel would hang (the watchdog turns that into a FAIL).
  std::string spin = build_pit_guest();
  spin[0x2E] = '\x90';
  ok = run_guest("pit-spin", spin, kPitExpected) && ok;

  // End to end through real firmware: SeaBIOS finds the IDE disk, loads its boot sector and runs it.
  // Optional because the firmware is a third-party file the user supplies.
  if (!firmware.empty()) ok = run_bios_disk(firmware) && ok;
  return ok ? 0 : 1;
}

}  // namespace sandvm::cli
