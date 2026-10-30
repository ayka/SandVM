#pragma once

#include <windows.h>
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>

#include <atomic>
#include <cstdint>

namespace sandvm {

class Vm;

enum class StopReason {
  Halted,    // HLT with nothing that could wake the vCPU
  Shutdown,  // triple fault / unrecoverable exception
  Canceled,  // Vm::request_stop()
  Reset,     // the guest pulsed the CPU reset line (i8042 / port 0x92 / 0xCF9); restarting is up to the caller
  PowerOff,  // the guest entered ACPI S5 (soft off)
  Error,     // unhandled exit or WHPX failure
};

const char* to_string(StopReason reason);

class Vcpu {
 public:
  Vcpu(Vm& vm, uint32_t index);
  ~Vcpu();
  Vcpu(const Vcpu&) = delete;
  Vcpu& operator=(const Vcpu&) = delete;

  uint32_t index() const { return index_; }

  // Runs the vCPU on the calling thread until it stops.
  StopReason run();
  // Thread-safe: makes run() return StopReason::Canceled.
  void cancel();
  // Thread-safe: holds the vCPU outside the guest until resumed (or canceled). Interrupts raised in
  // the meantime stay pending and are delivered after resume.
  void set_paused(bool paused);
  // Thread-safe: an interrupt may have become pending. Wakes the vCPU from HLT, or forces it out
  // of the guest so the interrupt is considered before the next entry.
  void kick();

  void get_registers(const WHV_REGISTER_NAME* names, uint32_t count, WHV_REGISTER_VALUE* values);
  void set_registers(const WHV_REGISTER_NAME* names, uint32_t count, const WHV_REGISTER_VALUE* values);

  // Real-mode entry like a boot sector: all segment registers = seg, CS:IP = seg:ip, SP = sp.
  void set_real_mode_entry(uint16_t seg, uint16_t ip, uint16_t sp);
  // Architectural reset vector: CS selector F000 with base FFFF0000, IP FFF0, so the first fetch is
  // at 0xFFFFFFF0, at the top of the firmware ROM. Other registers keep their WHPX power-on values.
  void set_reset_entry();
  // Application processor after reset: halted until the local APIC receives INIT/SIPI (needs the
  // hypervisor's APIC emulation, which then starts it).
  void set_wait_for_sipi();

 private:
  // Injects a pending PIC interrupt if the guest can accept one, else requests an interrupt-window exit.
  void deliver_interrupt();
  // HLT with IF=1: blocks until the PIC raises INTR. Returns false if a stop was requested.
  bool wait_for_interrupt();
  // Makes sure the run loop observes kick generation `gen` (see in_run_ below).
  void force_exit(uint64_t gen);

  bool emulate_io(const WHV_RUN_VP_EXIT_CONTEXT& exit);
  bool emulate_mmio(const WHV_RUN_VP_EXIT_CONTEXT& exit);

  static HRESULT CALLBACK emu_io_port(VOID* context, WHV_EMULATOR_IO_ACCESS_INFO* io);
  static HRESULT CALLBACK emu_memory(VOID* context, WHV_EMULATOR_MEMORY_ACCESS_INFO* mem);
  static HRESULT CALLBACK emu_get_registers(VOID* context, const WHV_REGISTER_NAME* names, UINT32 count,
                                            WHV_REGISTER_VALUE* values);
  static HRESULT CALLBACK emu_set_registers(VOID* context, const WHV_REGISTER_NAME* names, UINT32 count,
                                            const WHV_REGISTER_VALUE* values);
  static HRESULT CALLBACK emu_translate_gva(VOID* context, WHV_GUEST_VIRTUAL_ADDRESS gva,
                                            WHV_TRANSLATE_GVA_FLAGS flags, WHV_TRANSLATE_GVA_RESULT_CODE* result,
                                            WHV_GUEST_PHYSICAL_ADDRESS* gpa);

  Vm& vm_;
  uint32_t index_;
  WHV_EMULATOR_HANDLE emulator_ = nullptr;
  HANDLE wake_event_ = nullptr;  // auto-reset; set by kick()/cancel() so a wakeup is never lost
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> paused_{false};
  std::atomic<DWORD> run_thread_id_{0};
  bool window_requested_ = false;  // only touched by the run thread

  // Kick protocol. WHPX does not document whether a cancel issued before WHvRunVirtualProcessor starts
  // is remembered, so we do not rely on it. The run loop sets in_run_, then publishes the kick generation
  // it has seen in seen_gen_, then checks for work and enters the guest. A kicker bumps kick_gen_; if the
  // loop is between that snapshot and the end of the run call with an older generation, the kicker
  // repeats the cancel until the run call returns. That window is short: once the guest is running,
  // a cancel always takes effect.
  std::atomic<uint64_t> kick_gen_{0};
  std::atomic<uint64_t> seen_gen_{0};
  std::atomic<bool> in_run_{false};
};

}  // namespace sandvm
