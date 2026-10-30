#pragma once
// Register bits shared by the ATA (ide.cpp) and ATAPI (ide_atapi.cpp) halves of IdeChannel.

#include <cstdint>
#include <cstring>

namespace sandvm::devices::ide {

// Status register.
constexpr uint8_t kStatusErr = 0x01;
constexpr uint8_t kStatusDrq = 0x08;
constexpr uint8_t kStatusDsc = 0x10;
constexpr uint8_t kStatusDrdy = 0x40;
constexpr uint8_t kStatusBsy = 0x80;
constexpr uint8_t kStatusReady = kStatusDrdy | kStatusDsc;

// Error register.
constexpr uint8_t kErrAbort = 0x04;
constexpr uint8_t kErrIdNotFound = 0x10;
constexpr uint8_t kErrUncorrectable = 0x40;

// Device control register.
constexpr uint8_t kCtrlNoIrq = 0x02;
constexpr uint8_t kCtrlReset = 0x04;
constexpr uint8_t kCtrlHob = 0x80;

constexpr uint8_t kSelectLba = 0x40;

// ATA strings store two characters per word, the first one in the high byte, padded with spaces.
inline void put_string(uint16_t* words, unsigned count, const char* text) {
  const size_t len = std::strlen(text);
  for (unsigned i = 0; i < count; ++i) {
    const char hi = 2 * i < len ? text[2 * i] : ' ';
    const char lo = 2 * i + 1 < len ? text[2 * i + 1] : ' ';
    words[i] = static_cast<uint16_t>((uint8_t(hi) << 8) | uint8_t(lo));
  }
}

}  // namespace sandvm::devices::ide
