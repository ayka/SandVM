# SandVM: agent instructions (Codex and Claude)

SandVM is a C++20 virtual machine monitor for Windows built on the Windows Hypervisor Platform
(WHPX, `WinHvPlatform.dll` + `WinHvEmulation.dll`). Goal: boot full guest OSes (Linux, Windows 7).
Sandboxie-Plus is used as an outer isolation layer for the VMM process (`src/sbie/`), not as the hypervisor.

Collaboration protocol: `docs/COLLAB.md`. Task board: `collab/TASKS.md`. Handoffs: `collab/HANDOFF.md`.

## Layout
- `src/core/`: partition (`vm`), vCPU run loop + instruction emulator glue (`vcpu`), guest memory, I/O bus, `whpx` runtime loader.
- `src/core/devices/`: device models. Each implements `PioDevice` and/or `MmioDevice` (`src/core/io_bus.h`) and gets absolute port/GPA values.
- `src/sbie/`: Sandboxie-Plus launcher.
- `src/cli/`: `sandvm` executable (`caps`, `selftest`, `run`).

## Conventions
- C++20, namespace `sandvm`, 2-space indent, 120 columns, `snake_case` functions, `trailing_` members.
- WHPX functions are called only through `whpx::api()` (never link WinHvPlatform.lib directly).
- Errors: `whpx::check(hr, "WHvXxx")` in setup paths; the run loop logs and returns a `StopReason` instead of throwing.
- Comments explain *why* (hardware behavior, spec section), not what.
- No new third-party dependencies without the user's approval.
- Do not commit; the user decides when to commit.
