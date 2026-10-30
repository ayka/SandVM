#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sandvm::cli {

// Scancode set 1 make/break sequences that type `text` on a US keyboard layout (a guest with another
// layout sees the keys at the same positions). Printable ASCII plus the escapes \n (Enter), \t (Tab),
// \e (Escape), \b (Backspace) and \\. Characters without a key are skipped.
std::vector<uint8_t> scancodes_for_text(const std::string& text);

}  // namespace sandvm::cli
