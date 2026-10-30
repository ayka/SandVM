#include "core/loader/linux.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "core/log.h"
#include "core/vm.h"

namespace sandvm::loader {
namespace {

// Offsets into the bzImage and into boot_params (Documentation/arch/x86/boot.rst and zero-page.rst).
constexpr size_t kSetupSects = 0x1F1;
constexpr size_t kHeaderMagic = 0x202;  // "HdrS"
constexpr size_t kVersion = 0x206;
constexpr size_t kTypeOfLoader = 0x210;
constexpr size_t kLoadFlags = 0x211;
constexpr size_t kRamdiskImage = 0x218;
constexpr size_t kRamdiskSize = 0x21C;
constexpr size_t kCmdLinePtr = 0x228;
constexpr size_t kInitrdAddrMax = 0x22C;
constexpr size_t kXLoadFlags = 0x236;
constexpr size_t kCmdlineSize = 0x238;
constexpr size_t kInitSize = 0x260;
constexpr size_t kAcpiRsdpAddr = 0x070;
constexpr size_t kExtRamdiskImage = 0x0C0;
constexpr size_t kExtRamdiskSize = 0x0C4;
constexpr size_t kExtCmdLinePtr = 0x0C8;
constexpr size_t kE820Entries = 0x1E8;
constexpr size_t kE820Table = 0x2D0;
constexpr size_t kE820Max = 128;

constexpr uint8_t kLoadedHigh = 0x01;
constexpr uint16_t kXlfKernel64 = 0x0001;
constexpr uint16_t kMinProtocol = 0x020C;  // first version with xloadflags (64-bit entry advertised)

// Guest physical layout below 1 MiB. Everything lies in conventional memory that the e820 map hands
// to the kernel as RAM; the kernel is done with these structures before it reuses that memory
// (boot_params and the command line are copied early in startup_64 / x86_64_start_kernel).
constexpr uint64_t kGdtAddr = 0x0500;
constexpr uint64_t kBootParamsAddr = 0x7000;
constexpr uint64_t kPml4Addr = 0x9000;
constexpr uint64_t kPdptAddr = 0xA000;
constexpr uint64_t kPdAddr = 0xB000;  // 4 page directories, 0xB000-0xEFFF: 4 GiB of 2 MiB pages
constexpr uint64_t kCmdlineAddr = 0x20000;
constexpr uint64_t kCmdlineMax = 0x10000;
constexpr uint64_t kEbdaStart = 0x9FC00;  // reported reserved, like a BIOS would
constexpr uint64_t kKernelAddr = 0x100000;

constexpr uint64_t kPageSize = 0x1000;

struct E820Entry {
  uint64_t addr;
  uint64_t size;
  uint32_t type;
};
static_assert(sizeof(uint64_t) * 2 + sizeof(uint32_t) == 20);
constexpr uint32_t kE820Ram = 1;
constexpr uint32_t kE820Reserved = 2;

template <typename T>
T get(std::span<const uint8_t> buf, size_t off) {
  T v{};
  std::memcpy(&v, buf.data() + off, sizeof(T));
  return v;
}

template <typename T>
void put(std::vector<uint8_t>& buf, size_t off, T v) {
  std::memcpy(buf.data() + off, &v, sizeof(T));
}

void write_guest(Vm& vm, uint64_t gpa, const void* data, size_t len, const char* what) {
  if (!vm.memory().write(gpa, data, len)) {
    throw std::invalid_argument(std::string(what) + " does not fit in guest RAM");
  }
}

void build_page_tables(Vm& vm) {
  // Identity map of the first 4 GiB with 2 MiB pages. The protocol only requires the kernel, boot_params
  // and command line to be mapped, but covering the whole 32-bit space costs four pages and also
  // maps any MMIO the decompressor might touch.
  std::vector<uint64_t> pml4(512, 0), pdpt(512, 0), pd(512 * 4, 0);
  constexpr uint64_t kPresentRw = 0x3;
  constexpr uint64_t kLargePage = 0x80;
  pml4[0] = kPdptAddr | kPresentRw;
  for (uint64_t i = 0; i < 4; ++i) pdpt[i] = (kPdAddr + i * kPageSize) | kPresentRw;
  for (uint64_t i = 0; i < pd.size(); ++i) pd[i] = (i << 21) | kPresentRw | kLargePage;
  write_guest(vm, kPml4Addr, pml4.data(), pml4.size() * 8, "page tables");
  write_guest(vm, kPdptAddr, pdpt.data(), pdpt.size() * 8, "page tables");
  write_guest(vm, kPdAddr, pd.data(), pd.size() * 8, "page tables");
}

void build_gdt(Vm& vm) {
  // Selectors the 64-bit boot protocol requires: __BOOT_CS = 0x10 (64-bit code), __BOOT_DS = 0x18.
  const uint64_t gdt[4] = {0, 0, 0x00AF9A000000FFFFull, 0x00CF92000000FFFFull};
  write_guest(vm, kGdtAddr, gdt, sizeof(gdt), "GDT");
}

void set_long_mode_registers(Vm& vm, uint64_t entry) {
  WHV_REGISTER_NAME names[32];
  WHV_REGISTER_VALUE values[32] = {};
  uint32_t n = 0;
  auto reg = [&](WHV_REGISTER_NAME name) -> WHV_REGISTER_VALUE& {
    names[n] = name;
    return values[n++];
  };

  // Attribute layout of WHV_X64_SEGMENT_REGISTER: type[3:0] S[4] DPL[6:5] P[7] AVL[12] L[13] D[14] G[15].
  auto& cs = reg(WHvX64RegisterCs).Segment;
  cs.Selector = 0x10;
  cs.Limit = 0xFFFFFFFF;
  cs.Attributes = 0xA09B;  // execute/read accessed, present, L=1, G=1
  for (WHV_REGISTER_NAME seg : {WHvX64RegisterDs, WHvX64RegisterEs, WHvX64RegisterSs, WHvX64RegisterFs,
                                WHvX64RegisterGs}) {
    auto& s = reg(seg).Segment;
    s.Selector = 0x18;
    s.Limit = 0xFFFFFFFF;
    s.Attributes = 0xC093;  // read/write accessed, present, D=1, G=1
  }
  // VM entry checks want a usable TR (busy TSS) and an LDTR that is either unusable or a valid LDT.
  auto& tr = reg(WHvX64RegisterTr).Segment;
  tr.Limit = 0xFFFF;
  tr.Attributes = 0x008B;
  auto& ldtr = reg(WHvX64RegisterLdtr).Segment;
  ldtr.Limit = 0xFFFF;
  ldtr.Attributes = 0x0082;

  auto& gdtr = reg(WHvX64RegisterGdtr).Table;
  gdtr.Base = kGdtAddr;
  gdtr.Limit = 4 * 8 - 1;
  reg(WHvX64RegisterIdtr).Table = {};

  reg(WHvX64RegisterCr0).Reg64 = 0x80000033;  // PG | NE | ET | MP | PE
  reg(WHvX64RegisterCr3).Reg64 = kPml4Addr;
  reg(WHvX64RegisterCr4).Reg64 = 0x20;   // PAE
  reg(WHvX64RegisterEfer).Reg64 = 0x500;  // LME | LMA
  reg(WHvX64RegisterRflags).Reg64 = 0x2;  // IF=0: the protocol requires interrupts disabled
  reg(WHvX64RegisterRip).Reg64 = entry;
  reg(WHvX64RegisterRsi).Reg64 = kBootParamsAddr;
  // No stack is promised by the protocol; startup_64 sets its own. A sane value helps debugging.
  reg(WHvX64RegisterRsp).Reg64 = kBootParamsAddr - 0x10;

  vm.vcpu(0).set_registers(names, n, values);
}

}  // namespace

LinuxBootInfo load_linux(Vm& vm, std::span<const uint8_t> bzimage, std::span<const uint8_t> initrd,
                         const std::string& cmdline) {
  if (bzimage.size() < 0x1000 || get<uint32_t>(bzimage, kHeaderMagic) != 0x53726448) {
    throw std::invalid_argument("not a bzImage: setup header magic \"HdrS\" missing");
  }
  LinuxBootInfo info;
  info.protocol_version = get<uint16_t>(bzimage, kVersion);
  if (info.protocol_version < kMinProtocol) {
    throw std::invalid_argument("boot protocol too old for the 64-bit entry point (need 2.12+)");
  }
  if (!(get<uint16_t>(bzimage, kXLoadFlags) & kXlfKernel64)) {
    throw std::invalid_argument("kernel does not advertise a 64-bit entry point (XLF_KERNEL_64)");
  }
  if (!(bzimage[kLoadFlags] & kLoadedHigh)) {
    throw std::invalid_argument("zImage (low-loaded) kernels are not supported");
  }

  unsigned setup_sects = bzimage[kSetupSects];
  if (setup_sects == 0) setup_sects = 4;  // the protocol's historical default
  const size_t setup_size = (setup_sects + 1) * 512;
  if (setup_size >= bzimage.size()) throw std::invalid_argument("bzImage is truncated");
  const auto kernel = bzimage.subspan(setup_size);

  // The decompressor runs in place and needs init_size bytes from the load address (it relocates to a
  // kernel_alignment boundary within that budget when the kernel is relocatable).
  const uint32_t init_size = get<uint32_t>(bzimage, kInitSize);
  const uint64_t ram = vm.ram_below_4g();
  const uint64_t kernel_end = kKernelAddr + std::max<uint64_t>(init_size, kernel.size());
  if (kernel_end > ram) {
    throw std::invalid_argument("not enough guest RAM for the kernel (needs " +
                                std::to_string((kernel_end + (1 << 20) - 1) >> 20) + " MiB)");
  }
  write_guest(vm, kKernelAddr, kernel.data(), kernel.size(), "kernel");
  info.kernel_addr = kKernelAddr;
  info.kernel_size = kernel.size();

  // boot_params: zeroed page plus the setup header copied verbatim (0x1F1 up to the end the header
  // declares at 0x201), then the loader-owned fields filled in.
  std::vector<uint8_t> bp(kPageSize, 0);
  const size_t header_end = std::min<size_t>(0x202 + bzimage[0x201], kPageSize);
  std::memcpy(bp.data() + kSetupSects, bzimage.data() + kSetupSects, header_end - kSetupSects);
  bp[kTypeOfLoader] = 0xFF;  // undefined loader
  bp[kLoadFlags] = kLoadedHigh;  // no CAN_USE_HEAP: the real-mode setup code is not run

  const uint32_t cmdline_size = get<uint32_t>(bzimage, kCmdlineSize);
  if (cmdline.size() >= std::min<uint64_t>(cmdline_size ? cmdline_size + 1ull : 256, kCmdlineMax)) {
    throw std::invalid_argument("kernel command line is too long");
  }
  write_guest(vm, kCmdlineAddr, cmdline.c_str(), cmdline.size() + 1, "command line");
  put<uint32_t>(bp, kCmdLinePtr, static_cast<uint32_t>(kCmdlineAddr));
  put<uint32_t>(bp, kExtCmdLinePtr, 0);

  if (!initrd.empty()) {
    // Highest page-aligned spot below both the end of RAM and initrd_addr_max, like bootloaders do,
    // so the decompressed kernel growing upward from 1 MiB does not run into it.
    const uint64_t addr_max = get<uint32_t>(bzimage, kInitrdAddrMax);
    const uint64_t top = std::min<uint64_t>(ram, addr_max ? addr_max + 1 : ram);
    if (initrd.size() > top || ((top - initrd.size()) & ~(kPageSize - 1)) < kernel_end) {
      throw std::invalid_argument("not enough guest RAM for the kernel and the initrd");
    }
    const uint64_t addr = (top - initrd.size()) & ~(kPageSize - 1);
    write_guest(vm, addr, initrd.data(), initrd.size(), "initrd");
    put<uint32_t>(bp, kRamdiskImage, static_cast<uint32_t>(addr));
    put<uint32_t>(bp, kRamdiskSize, static_cast<uint32_t>(initrd.size()));
    put<uint32_t>(bp, kExtRamdiskImage, static_cast<uint32_t>(addr >> 32));
    put<uint32_t>(bp, kExtRamdiskSize, static_cast<uint32_t>(uint64_t{initrd.size()} >> 32));
    info.initrd_addr = addr;
    info.initrd_size = initrd.size();
  }

  // ACPI: tables at 0xE0000, where a legacy RSDP scan also finds them; boot_params saves the kernel the scan.
  put<uint64_t>(bp, kAcpiRsdpAddr, vm.install_acpi_tables());

  // The memory map a PC BIOS would report for our flat RAM. 0xA0000-0xFFFFF is RAM in this VM but is
  // reserved here so the kernel treats it as the legacy video/ROM hole and never allocates from it.
  const E820Entry map[] = {
      {0, kEbdaStart, kE820Ram},
      {kEbdaStart, 0x100000 - kEbdaStart, kE820Reserved},
      {0x100000, ram - 0x100000, kE820Ram},
      {uint64_t{1} << 32, vm.ram_above_4g(), kE820Ram},
  };
  static_assert(std::size(map) <= kE820Max);
  const size_t entries = vm.ram_above_4g() ? std::size(map) : std::size(map) - 1;
  bp[kE820Entries] = static_cast<uint8_t>(entries);
  for (size_t i = 0; i < entries; ++i) {
    const size_t off = kE820Table + i * 20;
    put<uint64_t>(bp, off, map[i].addr);
    put<uint64_t>(bp, off + 8, map[i].size);
    put<uint32_t>(bp, off + 16, map[i].type);
  }
  write_guest(vm, kBootParamsAddr, bp.data(), bp.size(), "boot_params");

  build_gdt(vm);
  build_page_tables(vm);
  // The 64-bit entry point is at a fixed offset 0x200 from the start of the protected-mode kernel.
  set_long_mode_registers(vm, kKernelAddr + 0x200);

  SVM_INFO("linux: boot protocol %u.%02u, kernel %zu KiB at 0x%llx (init_size %u KiB), initrd %zu KiB at 0x%llx",
           info.protocol_version >> 8, info.protocol_version & 0xFF, kernel.size() / 1024,
           static_cast<unsigned long long>(kKernelAddr), init_size / 1024, initrd.size() / 1024,
           static_cast<unsigned long long>(info.initrd_addr));
  return info;
}

}  // namespace sandvm::loader
