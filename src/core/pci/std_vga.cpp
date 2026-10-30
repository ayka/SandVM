#include "core/pci/std_vga.h"

#include <utility>

#include "core/log.h"
#include "core/whpx.h"

namespace sandvm::pci {

StdVga::StdVga(IoBus& io, WHV_PARTITION_HANDLE partition, std::shared_ptr<devices::Vga> vga)
    : PciFunction(io, kVendor, kDevice, 0x02, 0x030000), io_(io), partition_(partition), vga_(std::move(vga)) {
  set_wmask(0x04, 2, 0x0003);  // I/O and memory space
  set_config(0x2C, 2, 0x1AF4);  // subsystem vendor / device as QEMU reports them
  set_config(0x2E, 2, 0x1100);
  add_mem_bar(0, static_cast<uint32_t>(vga_->vram_size()), /*prefetchable=*/true,
              [this](uint64_t base) { map_lfb(base); });
}

void StdVga::map_lfb(uint64_t base) {
  if (stuck_) return;
  const auto& w = whpx::api();
  const uint64_t size = vga_->vram_size();
  if (lfb_) {
    if (hv_mapped_) {
      HRESULT hr = w.WHvUnmapGpaRange(partition_, base_of_current_, size);
      if (FAILED(hr)) {
        // The old GPA would keep aliasing VRAM whatever the BAR says; keep everything as it is rather
        // than add a second alias.
        SVM_ERROR("vga: WHvUnmapGpaRange(0x%llx) failed: 0x%08lx, framebuffer BAR frozen",
                  (unsigned long long)base_of_current_, hr);
        stuck_ = true;
        return;
      }
      hv_mapped_ = false;
    }
    io_.unregister_mmio(base_of_current_, lfb_.get());
    lfb_.reset();
  }
  if (!base) return;

  // Claim the range on the bus first: if another device has it, the partition must not map over it.
  auto lfb = std::make_shared<Lfb>(vga_, base);
  try {
    io_.register_mmio(base, size, lfb);
  } catch (const std::invalid_argument&) {
    SVM_WARN("vga: framebuffer BAR at 0x%llx overlaps another device, not decoded", (unsigned long long)base);
    return;
  }
  lfb_ = std::move(lfb);
  base_of_current_ = base;
  // Mapped VRAM lets the guest draw at memory speed. If the hypervisor refuses (overlap with RAM), the
  // MMIO registration alone still works, one exit per access.
  const auto flags = static_cast<WHV_MAP_GPA_RANGE_FLAGS>(WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite |
                                                          WHvMapGpaRangeFlagExecute);
  HRESULT hr = w.WHvMapGpaRange(partition_, vga_->vram(), base, size, flags);
  hv_mapped_ = SUCCEEDED(hr);
  if (!hv_mapped_) SVM_WARN("vga: WHvMapGpaRange(0x%llx) failed: 0x%08lx", (unsigned long long)base, hr);
  SVM_DEBUG("vga: framebuffer at 0x%llx", (unsigned long long)base);
}

}  // namespace sandvm::pci
