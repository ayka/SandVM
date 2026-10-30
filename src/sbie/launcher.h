#pragma once
// Sandboxie-Plus integration, host side. The VMM process itself can be confined in a
// Sandboxie box so that its file/registry/network side effects (disk images, logs,
// device backends) stay inside the box. We drive Sandboxie through its public
// Start.exe front end; see docs/ARCHITECTURE.md for the planned SbieApi integration.

#include <optional>
#include <string>
#include <vector>

namespace sandvm::sbie {

// True when SbieDll.dll is injected into this process, i.e. we already run sandboxed.
bool running_in_sandbox();

// Locates Start.exe: %SANDVM_SBIE_DIR%, then the default Sandboxie-Plus / Sandboxie folders.
std::optional<std::wstring> find_start_exe();

// Sandboxie box names: 1-32 chars of [A-Za-z0-9_].
bool is_valid_box_name(const std::wstring& box);

// Re-runs the current executable inside `box` with `args` (argv[1..]) and waits.
// Returns the exit code reported by Start.exe, or throws std::runtime_error.
int relaunch_in_box(const std::wstring& box, const std::vector<std::wstring>& args);

}  // namespace sandvm::sbie
