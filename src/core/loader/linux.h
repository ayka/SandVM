#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace sandvm {

class Vm;

namespace loader {

struct LinuxBootInfo {
  uint16_t protocol_version = 0;
  uint64_t kernel_addr = 0;
  uint64_t kernel_size = 0;
  uint64_t initrd_addr = 0;
  uint64_t initrd_size = 0;
};

// Direct Linux boot through the x86 boot protocol's 64-bit entry point (Documentation/arch/x86/boot.rst),
// without firmware: the protected-mode kernel goes to 1 MiB, boot_params ("zero page") carries the setup
// header, command line, initrd location and an e820 map, and the BSP starts in long mode at
// kernel + 0x200 with RSI = &boot_params, identity paging over the first 4 GiB and the flat
// __BOOT_CS (0x10) / __BOOT_DS (0x18) segments the protocol requires. Call before Vm::start().
// Throws std::invalid_argument for images that are not 64-bit-bootable bzImages or do not fit.
LinuxBootInfo load_linux(Vm& vm, std::span<const uint8_t> bzimage, std::span<const uint8_t> initrd,
                         const std::string& cmdline);

}  // namespace loader
}  // namespace sandvm
