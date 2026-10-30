#include "cli/keymap.h"

#include <cstring>

namespace sandvm::cli {
namespace {
constexpr uint8_t kLeftShift = 0x2A;

struct Key {
  uint8_t code;
  bool shift;
};

// US layout: the unshifted and shifted character on each set 1 key.
bool lookup(char c, Key* key) {
  static constexpr struct {
    uint8_t code;
    const char* chars;  // [unshifted, shifted]
  } kKeys[] = {
      {0x02, "1!"}, {0x03, "2@"}, {0x04, "3#"}, {0x05, "4$"}, {0x06, "5%"}, {0x07, "6^"}, {0x08, "7&"},
      {0x09, "8*"}, {0x0A, "9("}, {0x0B, "0)"}, {0x0C, "-_"}, {0x0D, "=+"}, {0x10, "qQ"}, {0x11, "wW"},
      {0x12, "eE"}, {0x13, "rR"}, {0x14, "tT"}, {0x15, "yY"}, {0x16, "uU"}, {0x17, "iI"}, {0x18, "oO"},
      {0x19, "pP"}, {0x1A, "[{"}, {0x1B, "]}"}, {0x1E, "aA"}, {0x1F, "sS"}, {0x20, "dD"}, {0x21, "fF"},
      {0x22, "gG"}, {0x23, "hH"}, {0x24, "jJ"}, {0x25, "kK"}, {0x26, "lL"}, {0x27, ";:"}, {0x28, "'\""},
      {0x29, "`~"}, {0x2B, "\\|"}, {0x2C, "zZ"}, {0x2D, "xX"}, {0x2E, "cC"}, {0x2F, "vV"}, {0x30, "bB"},
      {0x31, "nN"}, {0x32, "mM"}, {0x33, ",<"}, {0x34, ".>"}, {0x35, "/?"}, {0x39, "  "},
  };
  for (const auto& k : kKeys) {
    if (k.chars[0] == c) {
      *key = {k.code, false};
      return true;
    }
    if (k.chars[1] == c && k.code != 0x39) {
      *key = {k.code, true};
      return true;
    }
  }
  return false;
}
}  // namespace

std::vector<uint8_t> scancodes_for_text(const std::string& text) {
  std::vector<uint8_t> out;
  auto press = [&out](Key k) {
    if (k.shift) out.push_back(kLeftShift);
    out.push_back(k.code);
    out.push_back(static_cast<uint8_t>(k.code | 0x80));
    if (k.shift) out.push_back(kLeftShift | 0x80);
  };
  for (size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\\' && i + 1 < text.size()) {
      const char e = text[++i];
      if (e == 'n') press({0x1C, false});
      else if (e == 't') press({0x0F, false});
      else if (e == 'e') press({0x01, false});
      else if (e == 'b') press({0x0E, false});
      else if (e == '\\') press({0x2B, false});
      continue;
    }
    Key k;
    if (lookup(c, &k)) press(k);
  }
  return out;
}

}  // namespace sandvm::cli
