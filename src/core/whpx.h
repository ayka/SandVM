#pragma once
// Thin, runtime-loaded binding to the Windows Hypervisor Platform (WHPX).
// The DLLs only exist when the "Windows Hypervisor Platform" feature is enabled,
// so we resolve them with LoadLibrary instead of linking, and report a clear error.

#if !defined(_M_X64) && !defined(__x86_64__)
#error "SandVM targets x86-64 Windows only (WHPX emulator types are x64-gated)."
#endif

#include <windows.h>
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>

#include <stdexcept>
#include <string>

namespace sandvm::whpx {

#define SANDVM_WHP_FUNCS(X)          \
  X(WHvGetCapability)                \
  X(WHvCreatePartition)              \
  X(WHvSetupPartition)               \
  X(WHvDeletePartition)              \
  X(WHvGetPartitionProperty)         \
  X(WHvSetPartitionProperty)         \
  X(WHvMapGpaRange)                  \
  X(WHvUnmapGpaRange)                \
  X(WHvTranslateGva)                 \
  X(WHvCreateVirtualProcessor)       \
  X(WHvDeleteVirtualProcessor)       \
  X(WHvRunVirtualProcessor)          \
  X(WHvCancelRunVirtualProcessor)    \
  X(WHvGetVirtualProcessorRegisters) \
  X(WHvSetVirtualProcessorRegisters)   X(WHvRequestInterrupt)

#define SANDVM_WHE_FUNCS(X)    \
  X(WHvEmulatorCreateEmulator)  \
  X(WHvEmulatorDestroyEmulator) \
  X(WHvEmulatorTryIoEmulation)  \
  X(WHvEmulatorTryMmioEmulation)

struct Api {
#define SANDVM_DECLARE_FN(name) decltype(&::name) name = nullptr;
  SANDVM_WHP_FUNCS(SANDVM_DECLARE_FN)
  SANDVM_WHE_FUNCS(SANDVM_DECLARE_FN)
#undef SANDVM_DECLARE_FN
};

// Loads the DLLs once. Returns false and fills `error` if WHPX is unavailable.
bool load(std::string* error = nullptr);

// Returns the loaded API; throws whpx::Error if load() failed.
const Api& api();

class Error : public std::runtime_error {
 public:
  Error(const std::string& what, HRESULT hr);
  HRESULT hresult() const { return hr_; }

 private:
  HRESULT hr_;
};

inline void check(HRESULT hr, const char* what) {
  if (FAILED(hr)) throw Error(what, hr);
}

}  // namespace sandvm::whpx
