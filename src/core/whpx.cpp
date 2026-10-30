#include "core/whpx.h"

#include <cstdio>
#include <mutex>

namespace sandvm::whpx {
namespace {

Api g_api;
bool g_loaded = false;
std::string g_error;
std::once_flag g_once;

template <typename Fn>
bool resolve(HMODULE module, const char* name, Fn& out) {
  FARPROC proc = GetProcAddress(module, name);
  if (!proc) {
    g_error = std::string("missing export: ") + name;
    return false;
  }
  out = reinterpret_cast<Fn>(reinterpret_cast<void (*)()>(proc));
  return true;
}

void do_load() {
  HMODULE platform = LoadLibraryExW(L"WinHvPlatform.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  HMODULE emulation = LoadLibraryExW(L"WinHvEmulation.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  bool ok = platform && emulation;
  if (!ok) {
    g_error =
        "WinHvPlatform.dll / WinHvEmulation.dll not found. Enable the "
        "'Windows Hypervisor Platform' optional feature and reboot.";
  }
#define SANDVM_RESOLVE_PLATFORM(name) ok = ok && resolve(platform, #name, g_api.name);
#define SANDVM_RESOLVE_EMULATION(name) ok = ok && resolve(emulation, #name, g_api.name);
  SANDVM_WHP_FUNCS(SANDVM_RESOLVE_PLATFORM)
  SANDVM_WHE_FUNCS(SANDVM_RESOLVE_EMULATION)
#undef SANDVM_RESOLVE_PLATFORM
#undef SANDVM_RESOLVE_EMULATION
  if (!ok) {
    // load() fails permanently (call_once), so do not keep half-initialized state around.
    g_api = Api{};
    if (platform) FreeLibrary(platform);
    if (emulation) FreeLibrary(emulation);
    return;
  }
  g_loaded = true;
}

std::string format_error(const std::string& what, HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
  return what + " failed (HRESULT " + buf + ")";
}

}  // namespace

bool load(std::string* error) {
  std::call_once(g_once, do_load);
  if (!g_loaded && error) *error = g_error;
  return g_loaded;
}

const Api& api() {
  std::string error;
  if (!load(&error)) throw Error(error, E_NOINTERFACE);
  return g_api;
}

Error::Error(const std::string& what, HRESULT hr) : std::runtime_error(format_error(what, hr)), hr_(hr) {}

}  // namespace sandvm::whpx
