#pragma once

#include <windows.h>
#include <WinHvPlatform.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace sandvm {

// Host-backed guest physical memory. Each region is a VirtualAlloc block mapped
// into the partition with WHvMapGpaRange. ROM regions are mapped without write
// permission, so guest writes to them trap and are dropped by the MMIO path.
class GuestMemory {
 public:
  static constexpr uint64_t kPageSize = 0x1000;

  explicit GuestMemory(WHV_PARTITION_HANDLE partition);
  ~GuestMemory();
  GuestMemory(const GuestMemory&) = delete;
  GuestMemory& operator=(const GuestMemory&) = delete;

  // gpa and size must be page aligned. Returns the host pointer of the region.
  uint8_t* add_ram(uint64_t gpa, uint64_t size);
  uint8_t* add_rom(uint64_t gpa, std::span<const uint8_t> image);

  // Returns a host pointer for [gpa, gpa+len) if it lies inside one region.
  uint8_t* host_ptr(uint64_t gpa, uint64_t len) const;
  bool is_readonly(uint64_t gpa) const;

  bool read(uint64_t gpa, void* dst, size_t len) const;
  bool write(uint64_t gpa, const void* src, size_t len);  // host-side, so it also writes ROM

 private:
  struct Region {
    uint64_t gpa;
    uint64_t size;
    uint8_t* host;
    bool readonly;
  };

  uint8_t* add_region(uint64_t gpa, uint64_t size, bool readonly);
  const Region* find(uint64_t gpa, uint64_t len) const;

  WHV_PARTITION_HANDLE partition_;
  std::vector<Region> regions_;
};

}  // namespace sandvm
