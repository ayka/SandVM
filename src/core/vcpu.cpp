#include "core/vcpu.h"

#include <cstdio>
#include <exception>
#include <iterator>

#include "core/devices/pic8259.h"
#include "core/log.h"
#include "core/vm.h"
#include "core/whpx.h"

namespace sandvm {
namespace {
constexpr uint64_t kRflagsIf = 0x200;
}  // namespace

const char* to_string(StopReason reason) {
  switch (reason) {
    case StopReason::Halted: return "halted";
    case StopReason::Shutdown: return "shutdown";
    case StopReason::Canceled: return "canceled";
    case StopReason::Reset: return "reset";
    case StopReason::PowerOff: return "power off";
    case StopReason::Error: return "error";
  }
  return "?";
}

Vcpu::Vcpu(Vm& vm, uint32_t index) : vm_(vm), index_(index) {
  const auto& w = whpx::api();
  whpx::check(w.WHvCreateVirtualProcessor(vm_.partition(), index_, 0), "WHvCreateVirtualProcessor");

  WHV_EMULATOR_CALLBACKS callbacks{};
  callbacks.Size = sizeof(callbacks);
  callbacks.WHvEmulatorIoPortCallback = &Vcpu::emu_io_port;
  callbacks.WHvEmulatorMemoryCallback = &Vcpu::emu_memory;
  callbacks.WHvEmulatorGetVirtualProcessorRegisters = &Vcpu::emu_get_registers;
  callbacks.WHvEmulatorSetVirtualProcessorRegisters = &Vcpu::emu_set_registers;
  callbacks.WHvEmulatorTranslateGvaPage = &Vcpu::emu_translate_gva;
  HRESULT hr = w.WHvEmulatorCreateEmulator(&callbacks, &emulator_);
  if (FAILED(hr)) {
    w.WHvDeleteVirtualProcessor(vm_.partition(), index_);
    throw whpx::Error("WHvEmulatorCreateEmulator", hr);
  }

  wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!wake_event_) {
    hr = HRESULT_FROM_WIN32(GetLastError());
    w.WHvEmulatorDestroyEmulator(emulator_);
    w.WHvDeleteVirtualProcessor(vm_.partition(), index_);
    throw whpx::Error("CreateEventW", hr);
  }
}

Vcpu::~Vcpu() {
  const auto& w = whpx::api();
  if (wake_event_) CloseHandle(wake_event_);
  if (emulator_) w.WHvEmulatorDestroyEmulator(emulator_);
  w.WHvDeleteVirtualProcessor(vm_.partition(), index_);
}

void Vcpu::get_registers(const WHV_REGISTER_NAME* names, uint32_t count, WHV_REGISTER_VALUE* values) {
  whpx::check(whpx::api().WHvGetVirtualProcessorRegisters(vm_.partition(), index_, names, count, values),
              "WHvGetVirtualProcessorRegisters");
}

void Vcpu::set_registers(const WHV_REGISTER_NAME* names, uint32_t count, const WHV_REGISTER_VALUE* values) {
  whpx::check(whpx::api().WHvSetVirtualProcessorRegisters(vm_.partition(), index_, names, count, values),
              "WHvSetVirtualProcessorRegisters");
}

void Vcpu::set_real_mode_entry(uint16_t seg, uint16_t ip, uint16_t sp) {
  static constexpr WHV_REGISTER_NAME kNames[] = {
      WHvX64RegisterCs, WHvX64RegisterDs, WHvX64RegisterEs, WHvX64RegisterSs,
      WHvX64RegisterFs, WHvX64RegisterGs, WHvX64RegisterRip, WHvX64RegisterRsp,
  };
  constexpr uint32_t kSegCount = 6;
  WHV_REGISTER_VALUE values[std::size(kNames)] = {};
  // Read first so the reset-state segment attributes (code 0x9B / data 0x93) are kept.
  get_registers(kNames, static_cast<uint32_t>(std::size(kNames)), values);
  for (uint32_t i = 0; i < kSegCount; ++i) {
    values[i].Segment.Selector = seg;
    values[i].Segment.Base = uint64_t{seg} << 4;
    values[i].Segment.Limit = 0xFFFF;
  }
  values[6].Reg64 = ip;
  values[7].Reg64 = sp;
  set_registers(kNames, static_cast<uint32_t>(std::size(kNames)), values);
}

void Vcpu::set_reset_entry() {
  static constexpr WHV_REGISTER_NAME kNames[] = {WHvX64RegisterCs, WHvX64RegisterRip};
  WHV_REGISTER_VALUE values[std::size(kNames)] = {};
  get_registers(kNames, static_cast<uint32_t>(std::size(kNames)), values);  // keep the CS attributes
  values[0].Segment.Selector = 0xF000;
  values[0].Segment.Base = 0xFFFF0000;
  values[0].Segment.Limit = 0xFFFF;
  values[1].Reg64 = 0xFFF0;
  set_registers(kNames, static_cast<uint32_t>(std::size(kNames)), values);
}

void Vcpu::set_wait_for_sipi() {
  WHV_REGISTER_NAME name = WHvRegisterInternalActivityState;
  WHV_REGISTER_VALUE value{};
  value.InternalActivity.StartupSuspend = 1;
  set_registers(&name, 1, &value);
}

StopReason Vcpu::run() {
  run_thread_id_ = GetCurrentThreadId();
  struct ClearThreadId {
    std::atomic<DWORD>& id;
    ~ClearThreadId() { id = 0; }
  } clear_thread_id{run_thread_id_};

  const auto& w = whpx::api();
  for (;;) {
    // Order matters for the kick protocol (vcpu.h): in_run_, then the generation snapshot, then the
    // checks for work. A kick after the snapshot is retried by the kicker until the run call returns.
    in_run_ = true;
    seen_gen_ = kick_gen_.load();
    if (stop_requested_.load()) break;
    if (paused_.load()) {
      in_run_ = false;
      while (paused_.load() && !stop_requested_.load()) WaitForSingleObject(wake_event_, INFINITE);
      continue;
    }
    try {
      deliver_interrupt();
    } catch (const std::exception& e) {
      in_run_ = false;
      SVM_ERROR("vcpu%u: interrupt delivery failed: %s", index_, e.what());
      return StopReason::Error;
    }

    WHV_RUN_VP_EXIT_CONTEXT exit{};
    HRESULT hr = w.WHvRunVirtualProcessor(vm_.partition(), index_, &exit, sizeof(exit));
    in_run_ = false;
    if (FAILED(hr)) {
      SVM_ERROR("vcpu%u: WHvRunVirtualProcessor failed (0x%08lx)", index_, static_cast<unsigned long>(hr));
      return StopReason::Error;
    }

    switch (exit.ExitReason) {
      case WHvRunVpExitReasonX64IoPortAccess:
        if (!emulate_io(exit)) return StopReason::Error;
        break;

      case WHvRunVpExitReasonMemoryAccess:
        if (!emulate_mmio(exit)) return StopReason::Error;
        break;

      case WHvRunVpExitReasonX64InterruptWindow:
        // The notification is one-shot; deliver_interrupt() re-arms it if still needed.
        window_requested_ = false;
        break;

      case WHvRunVpExitReasonX64Halt:
        // RIP already points past HLT, which is where an interrupt handler must return to.
        // With IF clear only NMI/SMI/INIT could resume the CPU, and none of them are modeled yet.
        if ((exit.VpContext.Rflags & kRflagsIf) == 0) return StopReason::Halted;
        if (!wait_for_interrupt()) return StopReason::Canceled;
        break;

      case WHvRunVpExitReasonX64ApicEoi:
        vm_.ioapic_eoi(static_cast<uint8_t>(exit.ApicEoi.InterruptVector));
        break;

      case WHvRunVpExitReasonCanceled:
        if (log_level() <= LogLevel::Debug) {
          static constexpr WHV_REGISTER_NAME kDbg[] = {WHvX64RegisterRflags, WHvRegisterInterruptState,
                                                       WHvRegisterPendingEvent, WHvRegisterPendingInterruption,
                                                       WHvRegisterInternalActivityState};
          WHV_REGISTER_VALUE v[std::size(kDbg)] = {};
          get_registers(kDbg, static_cast<uint32_t>(std::size(kDbg)), v);
          SVM_DEBUG("vcpu%u: canceled rip=%llx rflags=%llx shadow=%d extint=%d pendint=%d activity=%llx", index_,
                    static_cast<unsigned long long>(exit.VpContext.Rip), static_cast<unsigned long long>(v[0].Reg64),
                    int(v[1].InterruptState.InterruptShadow), int(v[2].ExtIntEvent.EventPending),
                    int(v[3].PendingInterruption.InterruptionPending),
                    static_cast<unsigned long long>(v[4].Reg64));
          vm_.pic().log_state();
        }
        // Either request_stop() or a kick() for a new interrupt; both are checked at the top of the loop.
        // Repeated cancels from the kick protocol can also leave a stale one behind; that is harmless.
        break;

      case WHvRunVpExitReasonUnrecoverableException:
        SVM_ERROR("vcpu%u: unrecoverable exception (triple fault) at rip=0x%llx", index_,
                  static_cast<unsigned long long>(exit.VpContext.Rip));
        return StopReason::Shutdown;

      case WHvRunVpExitReasonInvalidVpRegisterValue:
        SVM_ERROR("vcpu%u: invalid register state", index_);
        return StopReason::Error;

      case WHvRunVpExitReasonUnsupportedFeature:
        SVM_ERROR("vcpu%u: unsupported feature code=%u", index_,
                  static_cast<unsigned>(exit.UnsupportedFeature.FeatureCode));
        return StopReason::Error;

      default:
        SVM_ERROR("vcpu%u: unhandled exit reason 0x%x at rip=0x%llx", index_,
                  static_cast<unsigned>(exit.ExitReason), static_cast<unsigned long long>(exit.VpContext.Rip));
        return StopReason::Error;
    }
  }
  in_run_ = false;
  return StopReason::Canceled;
}

void Vcpu::cancel() {
  stop_requested_ = true;
  kick();
}

void Vcpu::set_paused(bool paused) {
  paused_ = paused;
  kick();
}

void Vcpu::kick() {
  const uint64_t gen = ++kick_gen_;  // after the caller made its new state (PIC, stop flag) visible
  SetEvent(wake_event_);
  // On the vCPU thread itself (a device raising an IRQ from an emulation callback) the loop re-checks
  // before the next entry anyway, and spinning here would wait for ourselves.
  if (run_thread_id_.load() != GetCurrentThreadId()) force_exit(gen);
}

void Vcpu::force_exit(uint64_t gen) {
  const auto& w = whpx::api();
  bool warned = false;
  while (in_run_.load() && seen_gen_.load() < gen) {
    HRESULT hr = w.WHvCancelRunVirtualProcessor(vm_.partition(), index_, 0);
    if (FAILED(hr) && !warned) {
      warned = true;
      SVM_WARN("vcpu%u: WHvCancelRunVirtualProcessor failed (0x%08lx), retrying", index_,
               static_cast<unsigned long>(hr));
    }
    SwitchToThread();  // let the vCPU thread reach WHvRunVirtualProcessor so the next cancel lands
  }
}

void Vcpu::deliver_interrupt() {
  devices::Pic8259& pic = vm_.pic();
  // The PIC's INTR output is wired to the bootstrap processor only (its LINT0 in virtual wire mode).
  if (index_ != 0 || !pic.intr_pending()) return;

  // The INTR pin is sampled only at an instruction boundary with IF=1, outside an STI / MOV SS
  // shadow, and when no other event is already queued for injection.
  // With the hypervisor's local APIC the PIC output is the APIC's LINT0 (virtual wire mode), so it goes in
  // as an ExtINT event, which the APIC accepts or holds per its LINT0 setup. That path also wakes a vCPU
  // the hypervisor keeps halted internally: with APIC emulation on, HLT does not exit to us.
  const bool apic = vm_.local_apic();
  const WHV_REGISTER_NAME kState[] = {WHvX64RegisterRflags, WHvRegisterInterruptState,
                                      apic ? WHvRegisterPendingEvent : WHvRegisterPendingInterruption,
                                      WHvRegisterInternalActivityState};
  WHV_REGISTER_VALUE state[std::size(kState)] = {};
  get_registers(kState, static_cast<uint32_t>(std::size(kState)), state);
  const bool busy = apic ? state[2].ExtIntEvent.EventPending : state[2].PendingInterruption.InterruptionPending;
  const bool can_inject =
      (state[0].Reg64 & kRflagsIf) && !state[1].InterruptState.InterruptShadow && !busy;

  WHV_REGISTER_NAME names[3];
  WHV_REGISTER_VALUE values[3] = {};
  uint32_t count = 0;
  if (can_inject) {
    const uint8_t vector = pic.acknowledge();  // INTA: moves the request to in-service
    if (apic && state[3].InternalActivity.HaltSuspend) {
      // Observed on WHPX: a VP halted inside the hypervisor is not woken by a pending ExtINT event, so
      // the interrupt (already acknowledged at the PIC) would never be taken. Leave the halt ourselves;
      // RIP is past the HLT, where the handler returns to.
      names[count] = WHvRegisterInternalActivityState;
      values[count] = state[3];
      values[count].InternalActivity.HaltSuspend = 0;
      ++count;
    }
    if (apic) {
      names[count] = WHvRegisterPendingEvent;
      values[count].ExtIntEvent.EventPending = 1;
      values[count].ExtIntEvent.EventType = WHvX64PendingEventExtInt;
      values[count].ExtIntEvent.Vector = vector;
    } else {
      names[count] = WHvRegisterPendingInterruption;
      values[count].PendingInterruption.InterruptionPending = 1;
      values[count].PendingInterruption.InterruptionType = WHvX64PendingInterrupt;
      values[count].PendingInterruption.InterruptionVector = vector;
    }
    ++count;
    SVM_DEBUG("vcpu%u: inject vector 0x%02x", index_, vector);
  }
  // Ask for an exit as soon as the guest can take an interrupt. Also needed right after an injection
  // when another request is still pending (auto-EOI, special mask mode), or it would wait for an
  // unrelated exit.
  if (!window_requested_ && (!can_inject || pic.intr_pending())) {
    names[count] = WHvX64RegisterDeliverabilityNotifications;
    values[count].DeliverabilityNotifications.InterruptNotification = 1;
    ++count;
    window_requested_ = true;
  }
  if (count) set_registers(names, count, values);
}

bool Vcpu::wait_for_interrupt() {
  devices::Pic8259& pic = vm_.pic();
  while (!stop_requested_.load()) {
    if (pic.intr_pending()) return true;
    WaitForSingleObject(wake_event_, INFINITE);
  }
  return false;
}

bool Vcpu::emulate_io(const WHV_RUN_VP_EXIT_CONTEXT& exit) {
  WHV_EMULATOR_STATUS status{};
  HRESULT hr = whpx::api().WHvEmulatorTryIoEmulation(emulator_, this, &exit.VpContext, &exit.IoPortAccess, &status);
  if (FAILED(hr) || !status.EmulationSuccessful) {
    SVM_ERROR("vcpu%u: I/O emulation failed port=0x%04x hr=0x%08lx status=0x%x", index_,
              exit.IoPortAccess.PortNumber, static_cast<unsigned long>(hr), status.AsUINT32);
    return false;
  }
  return true;
}

bool Vcpu::emulate_mmio(const WHV_RUN_VP_EXIT_CONTEXT& exit) {
  WHV_EMULATOR_STATUS status{};
  HRESULT hr =
      whpx::api().WHvEmulatorTryMmioEmulation(emulator_, this, &exit.VpContext, &exit.MemoryAccess, &status);
  if (FAILED(hr) || !status.EmulationSuccessful) {
    const auto& m = exit.MemoryAccess;
    char bytes[16 * 3 + 1] = {};
    for (unsigned i = 0; i < m.InstructionByteCount && i < 16; ++i) {
      std::snprintf(bytes + i * 3, 4, "%02x ", m.InstructionBytes[i]);
    }
    static constexpr const char* kAccess[] = {"read", "write", "execute"};
    SVM_ERROR("vcpu%u: MMIO emulation failed gpa=0x%llx gva=0x%llx %s hr=0x%08lx status=0x%x", index_,
              static_cast<unsigned long long>(m.Gpa), static_cast<unsigned long long>(m.Gva),
              m.AccessInfo.AccessType < 3 ? kAccess[m.AccessInfo.AccessType] : "?", static_cast<unsigned long>(hr),
              status.AsUINT32);
    SVM_ERROR("vcpu%u:   cs=0x%04x rip=0x%llx mode=%s insn: %s", index_, exit.VpContext.Cs.Selector,
              static_cast<unsigned long long>(exit.VpContext.Rip),
              exit.VpContext.ExecutionState.EferLma  ? "long"
              : exit.VpContext.ExecutionState.Cr0Pe ? "protected"
                                                    : "real",
              bytes);
    return false;
  }
  return true;
}

// Device code may throw; exceptions must not unwind through WinHvEmulation.dll.
// Returning a failure makes the emulator report EmulationSuccessful = 0, which the
// run loop turns into StopReason::Error.
HRESULT CALLBACK Vcpu::emu_io_port(VOID* context, WHV_EMULATOR_IO_ACCESS_INFO* io) {
  auto* self = static_cast<Vcpu*>(context);
  try {
    if (io->Direction == 0) {
      io->Data = self->vm_.io().pio_read(io->Port, io->AccessSize);
    } else {
      self->vm_.io().pio_write(io->Port, io->AccessSize, io->Data);
    }
    return S_OK;
  } catch (const std::exception& e) {
    SVM_ERROR("vcpu%u: device exception on port 0x%04x: %s", self->index_, io->Port, e.what());
  } catch (...) {
    SVM_ERROR("vcpu%u: unknown device exception on port 0x%04x", self->index_, io->Port);
  }
  return E_FAIL;
}

HRESULT CALLBACK Vcpu::emu_memory(VOID* context, WHV_EMULATOR_MEMORY_ACCESS_INFO* mem) {
  auto* self = static_cast<Vcpu*>(context);
  try {
    self->vm_.access_physical(mem->GpaAddress, mem->Direction != 0, mem->AccessSize, mem->Data);
    return S_OK;
  } catch (const std::exception& e) {
    SVM_ERROR("vcpu%u: device exception at gpa 0x%llx: %s", self->index_,
              static_cast<unsigned long long>(mem->GpaAddress), e.what());
  } catch (...) {
    SVM_ERROR("vcpu%u: unknown device exception at gpa 0x%llx", self->index_,
              static_cast<unsigned long long>(mem->GpaAddress));
  }
  return E_FAIL;
}

HRESULT CALLBACK Vcpu::emu_get_registers(VOID* context, const WHV_REGISTER_NAME* names, UINT32 count,
                                         WHV_REGISTER_VALUE* values) {
  auto* self = static_cast<Vcpu*>(context);
  return whpx::api().WHvGetVirtualProcessorRegisters(self->vm_.partition(), self->index_, names, count, values);
}

HRESULT CALLBACK Vcpu::emu_set_registers(VOID* context, const WHV_REGISTER_NAME* names, UINT32 count,
                                         const WHV_REGISTER_VALUE* values) {
  auto* self = static_cast<Vcpu*>(context);
  return whpx::api().WHvSetVirtualProcessorRegisters(self->vm_.partition(), self->index_, names, count, values);
}

HRESULT CALLBACK Vcpu::emu_translate_gva(VOID* context, WHV_GUEST_VIRTUAL_ADDRESS gva, WHV_TRANSLATE_GVA_FLAGS flags,
                                         WHV_TRANSLATE_GVA_RESULT_CODE* result, WHV_GUEST_PHYSICAL_ADDRESS* gpa) {
  auto* self = static_cast<Vcpu*>(context);
  WHV_TRANSLATE_GVA_RESULT translation{};
  HRESULT hr = whpx::api().WHvTranslateGva(self->vm_.partition(), self->index_, gva, flags, &translation, gpa);
  if (SUCCEEDED(hr)) *result = translation.ResultCode;
  return hr;
}

}  // namespace sandvm
