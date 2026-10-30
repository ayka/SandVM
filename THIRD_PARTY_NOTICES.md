# Third-party notices

SandVM itself is licensed under the MIT License (see `LICENSE`). It uses or
interoperates with the following components. None of their source code is
included in this repository unless stated otherwise.

## Windows Hypervisor Platform (WHPX)

Part of Windows. SandVM loads `WinHvPlatform.dll` and `WinHvEmulation.dll` at
runtime. No Microsoft binaries are redistributed.

## SeaBIOS / SeaVGABIOS

- Files: `bios-256k.bin`, `vgabios-stdvga.bin` (SeaBIOS 1.17.0,
  `rel-1.17.0-0-gb52ca86e094d`, unmodified QEMU prebuilt images)
- License: GNU LGPL v3
- Upstream: https://www.seabios.org/
- Distribution: not in this repository. The release package bundles them in
  `firmware\` with a `NOTICE.txt`; the corresponding source code is attached to
  the same GitHub release as `seabios-1.17.0.tar.gz` (also at
  https://www.seabios.org/downloads/seabios-1.17.0.tar.gz). If you redistribute
  the package, keep the notice and provide that source code.

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
- Usage: the SandVM.Manager GUI targets `net8.0-windows`. The default release
  package is self-contained: the .NET 8 and WPF runtime files are redistributed
  under their own license terms (MIT). `tools/package.ps1 -FrameworkDependent`
  produces a package that needs an installed .NET 8 Desktop Runtime instead.

## Guest operating system images

ISO and disk images under `images/` are not part of this project and are not
distributed with it. Their licenses are those of the respective distributions.
