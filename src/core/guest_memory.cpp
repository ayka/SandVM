#include "core/guest_memory.h"

#include <cstring>
#include <stdexcept>

#include "core/whpx.h"

namespace sandvm {

GuestMemory::GuestMemory(WHV_PARTITION_HANDLE partition) : partition_(partition) {}

GuestMemory::~GuestMemory() {
  const auto& w = whpx::api();
  for (const auto& r : regions_) {
    w.WHvUnmapGpaRange(partition_, r.gpa, r.size);
    VirtualFree(r.host, 0, MEM_RELEASE);
  }
}

uint8_t* GuestMemory::add_ram(uint64_t gpa, uint64_t size) { return add_region(gpa, size, false); }

uint8_t* GuestMemory::add_rom(uint64_t gpa, std::span<const uint8_t> image) {
  const uint64_t size = (image.size() + kPageSize - 1) & ~(kPageSize - 1);
  uint8_t* host = add_region(gpa, size, true);
  std::memcpy(host, image.data(), image.size());
  return host;
}

uint8_t* GuestMemory::add_region(uint64_t gpa, uint64_t size, bool readonly) {
  if (size == 0 || (gpa & (kPageSize - 1)) || (size & (kPageSize - 1))) {
    throw std::invalid_argument("guest memory region must be non-empty and page aligned");
  }
  for (const auto& r : regions_) {
    if (gpa < r.gpa + r.size && r.gpa < gpa + size) {
      throw std::invalid_argument("guest memory region overlaps an existing region");
    }
  }

  void* host = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!host) throw std::runtime_error("VirtualAlloc failed for guest memory");

  int flags = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute;
  if (!readonly) flags |= WHvMapGpaRangeFlagWrite;
  HRESULT hr =
      whpx::api().WHvMapGpaRange(partition_, host, gpa, size, static_cast<WHV_MAP_GPA_RANGE_FLAGS>(flags));
  if (FAILED(hr)) {
    VirtualFree(host, 0, MEM_RELEASE);
    throw whpx::Error("WHvMapGpaRange", hr);
  }

  regions_.push_back({gpa, size, static_cast<uint8_t*>(host), readonly});
  return static_cast<uint8_t*>(host);
}

const GuestMemory::Region* GuestMemory::find(uint64_t gpa, uint64_t len) const {
  for (const auto& r : regions_) {
    if (gpa >= r.gpa && len <= r.size && gpa - r.gpa <= r.size - len) return &r;
  }
  return nullptr;
}

uint8_t* GuestMemory::host_ptr(uint64_t gpa, uint64_t len) const {
  const Region* r = find(gpa, len);
  return r ? r->host + (gpa - r->gpa) : nullptr;
}

bool GuestMemory::is_readonly(uint64_t gpa) const {
  const Region* r = find(gpa, 1);
  return r && r->readonly;
}

bool GuestMemory::read(uint64_t gpa, void* dst, size_t len) const {
  const uint8_t* src = host_ptr(gpa, len);
  if (!src) return false;
  std::memcpy(dst, src, len);
  return true;
}

bool GuestMemory::write(uint64_t gpa, const void* src, size_t len) {
  uint8_t* dst = host_ptr(gpa, len);
  if (!dst) return false;
  std::memcpy(dst, src, len);
  return true;
}

}  // namespace sandvm
