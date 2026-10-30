#pragma once

#include <cstdint>
#include <vector>

namespace sandvm::cli {

// Boots tiny built-in real-mode guests (polled COM1, IRQ4, PIT IRQ0, IRQ0 into an exit-free loop).
// If `firmware` (e.g. SeaBIOS bios.bin) is given, also boots it from an in-memory IDE disk.
// Returns 0 on success.
int run_selftest(const std::vector<uint8_t>& firmware);

}  // namespace sandvm::cli
