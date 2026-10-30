# Third-party notices

SandVM itself is licensed under the MIT License (see `LICENSE`). It uses or
interoperates with the following components. None of their source code is
included in this repository unless stated otherwise.

## Windows Hypervisor Platform (WHPX)

Part of Windows. SandVM loads `WinHvPlatform.dll` and `WinHvEmulation.dll` at
runtime. No Microsoft binaries are redistributed.

## SeaBIOS / SeaVGABIOS

- Files: `bios-256k.bin`, `vgabios-stdvga.bin`
- License: GNU LGPL v3
- Upstream: https://www.seabios.org/
- Distribution: **not included** in this repository or in the default release
  package. You supply these images yourself. If you redistribute a package that
  contains them, keep the LGPL notice and provide the corresponding source code.

## Sandboxie-Plus

- License: GNU GPL v3
- Upstream: https://sandboxie-plus.com/
- Usage: optional external program. SandVM invokes the installed `Start.exe`
  interface to place its own process in a sandbox. No Sandboxie source code is
  copied, linked, or modified by this project.

## llvm-mingw

- License: Apache License 2.0 with LLVM exceptions
- Upstream: https://github.com/mstorsjo/llvm-mingw
- Usage: build toolchain only. It is not part of the released binaries.

## .NET 8 / WPF

- License: MIT
- Upstream: https://dotnet.microsoft.com/
- Usage: the SandVM.Manager GUI targets `net8.0-windows`. The default package is
  framework-dependent; the .NET 8 Desktop Runtime is a prerequisite. A
  self-contained package can be produced with `tools/package.ps1 -SelfContained`;
  in that case the Microsoft runtime files are redistributed under their own
  license terms.

## Guest operating system images

ISO and disk images under `images/` are not part of this project and are not
distributed with it. Their licenses are those of the respective distributions.
