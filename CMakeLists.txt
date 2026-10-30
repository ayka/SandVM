cmake_minimum_required(VERSION 3.20)
project(SandVM VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "SandVM requires Windows (Windows Hypervisor Platform).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "SandVM requires a 64-bit (x86-64) build.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# WinHvPlatform.dll / WinHvEmulation.dll are loaded at runtime (see core/whpx.cpp),
# so only the SDK headers are needed at build time.
add_library(sandvm_core STATIC
  src/core/log.cpp
  src/core/whpx.cpp
  src/core/guest_memory.cpp
  src/core/io_bus.cpp
  src/core/block/disk.cpp
  src/core/block/host_file.cpp
  src/core/block/image.cpp
  src/core/block/vhd.cpp
  src/core/block/vmdk.cpp
  src/core/vcpu.cpp
  src/core/vm.cpp
  src/core/devices/cmos.cpp
  src/core/devices/fw_cfg.cpp
  src/core/devices/i8042.cpp
  src/core/devices/ide.cpp
  src/core/devices/ide_atapi.cpp
  src/core/devices/ide_dma.cpp
  src/core/devices/uhci.cpp
  src/core/devices/usb.cpp
  src/core/devices/ioapic.cpp
  src/core/devices/pic8259.cpp
  src/core/devices/pit8254.cpp
  src/core/devices/serial16550.cpp
  src/core/devices/vga.cpp
  src/core/devices/e1000.cpp
  src/core/net/user_nat.cpp
  src/core/loader/linux.cpp
  src/core/pci/pci.cpp
  src/core/pci/piix.cpp
  src/core/pci/std_vga.cpp
  src/core/pci/e1000_pci.cpp
  src/core/pci/uhci_pci.cpp
  src/core/acpi/aml.cpp
  src/core/acpi/tables.cpp
)
target_include_directories(sandvm_core PUBLIC src)
target_compile_definitions(sandvm_core PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)

add_library(sandvm_sbie STATIC
  src/sbie/launcher.cpp
)
target_link_libraries(sandvm_sbie PUBLIC sandvm_core shell32)

add_executable(sandvm
  src/cli/console_input.cpp
  src/cli/control_events.cpp
  src/cli/control_pipe.cpp
  src/cli/disk_cmd.cpp
  src/cli/display_window.cpp
  src/cli/keymap.cpp
  src/cli/main.cpp
  src/cli/selftest.cpp
)
target_link_libraries(sandvm PRIVATE sandvm_core sandvm_sbie user32 gdi32 ws2_32 iphlpapi comdlg32)

foreach(t sandvm_core sandvm_sbie sandvm)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${t} PRIVATE -Wall -Wextra)
  endif()
endforeach()
