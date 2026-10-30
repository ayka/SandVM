#include "core/acpi/aml.h"

#include <cstring>
#include <stdexcept>

namespace sandvm::acpi {
namespace {

constexpr uint8_t kZeroOp = 0x00;
constexpr uint8_t kOneOp = 0x01;
constexpr uint8_t kNameOp = 0x08;
constexpr uint8_t kBytePrefix = 0x0A;
constexpr uint8_t kWordPrefix = 0x0B;
constexpr uint8_t kDWordPrefix = 0x0C;
constexpr uint8_t kStringPrefix = 0x0D;
constexpr uint8_t kQWordPrefix = 0x0E;
constexpr uint8_t kScopeOp = 0x10;
constexpr uint8_t kBufferOp = 0x11;
constexpr uint8_t kPackageOp = 0x12;
constexpr uint8_t kOnesOp = 0xFF;
constexpr uint8_t kExtOpPrefix = 0x5B;
constexpr uint8_t kDeviceOp = 0x82;
constexpr uint8_t kProcessorOp = 0x83;
constexpr uint8_t kDualNamePrefix = 0x2E;
constexpr uint8_t kMultiNamePrefix = 0x2F;

void append(Bytes& out, const Bytes& in) { out.insert(out.end(), in.begin(), in.end()); }

void put_le(Bytes& out, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

// PkgLength (20.2.4) counts itself, so the encoding size depends on the total.
Bytes with_pkg_length(const Bytes& body) {
  const size_t n = body.size();
  Bytes out;
  if (n + 1 <= 0x3F) {
    out.push_back(static_cast<uint8_t>(n + 1));
  } else {
    unsigned extra = 1;
    while (extra < 3 && n + 1 + extra > (size_t{1} << (4 + 8 * extra)) - 1) ++extra;
    const size_t total = n + 1 + extra;
    if (total > (size_t{1} << (4 + 8 * extra)) - 1) throw std::length_error("AML package too large");
    out.push_back(static_cast<uint8_t>((extra << 6) | (total & 0x0F)));
    for (unsigned i = 0; i < extra; ++i) out.push_back(static_cast<uint8_t>(total >> (4 + 8 * i)));
  }
  append(out, body);
  return out;
}

Bytes name_seg(const std::string& seg) {
  if (seg.empty() || seg.size() > 4) throw std::invalid_argument("bad AML name segment: " + seg);
  Bytes out(seg.begin(), seg.end());
  out.resize(4, '_');
  return out;
}

Bytes term_list(std::initializer_list<Bytes> terms) {
  Bytes out;
  for (const auto& t : terms) append(out, t);
  return out;
}

Bytes term_list(const std::vector<Bytes>& terms) {
  Bytes out;
  for (const auto& t : terms) append(out, t);
  return out;
}

}  // namespace

Bytes concat(std::initializer_list<Bytes> parts) { return term_list(parts); }

Bytes integer(uint64_t value) {
  Bytes out;
  if (value == 0) out.push_back(kZeroOp);
  else if (value == 1) out.push_back(kOneOp);
  else if (value == ~0ull) out.push_back(kOnesOp);
  else if (value <= 0xFF) { out.push_back(kBytePrefix); put_le(out, value, 1); }
  else if (value <= 0xFFFF) { out.push_back(kWordPrefix); put_le(out, value, 2); }
  else if (value <= 0xFFFFFFFF) { out.push_back(kDWordPrefix); put_le(out, value, 4); }
  else { out.push_back(kQWordPrefix); put_le(out, value, 8); }
  return out;
}

Bytes string(const std::string& text) {
  Bytes out{kStringPrefix};
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0);
  return out;
}

Bytes eisa_id(const char* id) {
  if (std::strlen(id) != 7) throw std::invalid_argument("EISA ID must be 7 characters");
  auto hex = [](char c) -> uint32_t { return c <= '9' ? c - '0' : (c & ~0x20) - 'A' + 10; };
  const uint32_t c1 = (id[0] - 0x40) & 0x1F, c2 = (id[1] - 0x40) & 0x1F, c3 = (id[2] - 0x40) & 0x1F;
  // Big-endian bit packing of the three letters and four hex digits, stored as a little-endian DWORD.
  const uint32_t b0 = (c1 << 2) | (c2 >> 3);
  const uint32_t b1 = ((c2 & 7) << 5) | c3;
  const uint32_t b2 = (hex(id[3]) << 4) | hex(id[4]);
  const uint32_t b3 = (hex(id[5]) << 4) | hex(id[6]);
  Bytes out{kDWordPrefix};
  put_le(out, b0 | (b1 << 8) | (b2 << 16) | (b3 << 24), 4);
  return out;
}

Bytes name_string(const std::string& path) {
  Bytes out;
  size_t pos = 0;
  if (!path.empty() && path[0] == '\\') {
    out.push_back('\\');
    pos = 1;
  }
  while (pos < path.size() && path[pos] == '^') out.push_back(path[pos++]);
  std::vector<Bytes> segs;
  while (pos < path.size()) {
    const size_t dot = path.find('.', pos);
    segs.push_back(name_seg(path.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos)));
    pos = dot == std::string::npos ? path.size() : dot + 1;
  }
  if (segs.empty()) {
    out.push_back(0x00);  // NullName
  } else if (segs.size() == 2) {
    out.push_back(kDualNamePrefix);
  } else if (segs.size() > 2) {
    out.push_back(kMultiNamePrefix);
    out.push_back(static_cast<uint8_t>(segs.size()));
  }
  for (const auto& s : segs) append(out, s);
  return out;
}

Bytes name(const std::string& path, const Bytes& value) {
  Bytes out{kNameOp};
  append(out, name_string(path));
  append(out, value);
  return out;
}

Bytes package(const std::vector<Bytes>& elements) {
  if (elements.size() > 255) throw std::length_error("AML package with more than 255 elements");
  Bytes body{static_cast<uint8_t>(elements.size())};
  append(body, term_list(elements));
  Bytes out{kPackageOp};
  append(out, with_pkg_length(body));
  return out;
}

Bytes package(std::initializer_list<Bytes> elements) { return package(std::vector<Bytes>(elements)); }

Bytes buffer(const Bytes& data) {
  Bytes body = integer(data.size());
  append(body, data);
  Bytes out{kBufferOp};
  append(out, with_pkg_length(body));
  return out;
}

Bytes scope(const std::string& path, std::initializer_list<Bytes> terms) {
  Bytes body = name_string(path);
  append(body, term_list(terms));
  Bytes out{kScopeOp};
  append(out, with_pkg_length(body));
  return out;
}

Bytes device(const std::string& name, const std::vector<Bytes>& terms) {
  Bytes body = name_string(name);
  append(body, term_list(terms));
  Bytes out{kExtOpPrefix, kDeviceOp};
  append(out, with_pkg_length(body));
  return out;
}

Bytes device(const std::string& name, std::initializer_list<Bytes> terms) {
  return device(name, std::vector<Bytes>(terms));
}

Bytes processor(const std::string& name, uint8_t id, uint32_t pblk_addr, uint8_t pblk_len) {
  Bytes body = name_string(name);
  body.push_back(id);
  put_le(body, pblk_addr, 4);
  body.push_back(pblk_len);
  Bytes out{kExtOpPrefix, kProcessorOp};
  append(out, with_pkg_length(body));
  return out;
}

Bytes io(uint16_t base, uint8_t length, uint8_t align) {
  Bytes out{0x47, 0x01};  // I/O port descriptor, 16-bit decode
  put_le(out, base, 2);
  put_le(out, base, 2);
  out.push_back(align);
  out.push_back(length);
  return out;
}

Bytes irq_no_flags(unsigned irq) {
  Bytes out{0x22};  // IRQ descriptor without the flags byte: edge, active high
  put_le(out, 1u << irq, 2);
  return out;
}

namespace {
// Word/DWord address space descriptors (6.4.3.5.2/3). General flags 0x0C = MinFixed | MaxFixed, producer.
Bytes address_space(uint8_t tag, uint8_t type, uint8_t specific, uint64_t min, uint64_t max, unsigned width) {
  Bytes body{type, 0x0C, specific};
  put_le(body, 0, width);                // granularity
  put_le(body, min, width);
  put_le(body, max, width);
  put_le(body, 0, width);                // translation offset
  put_le(body, max - min + 1, width);    // length
  Bytes out{tag};
  put_le(out, body.size(), 2);
  append(out, body);
  return out;
}
}  // namespace

Bytes word_bus_number(uint16_t min, uint16_t max) { return address_space(0x88, 2, 0x00, min, max, 2); }
Bytes word_io(uint16_t min, uint16_t max) { return address_space(0x88, 1, 0x03, min, max, 2); }  // entire range
Bytes dword_memory(uint32_t min, uint32_t max) {
  return address_space(0x87, 0, 0x03, min, max, 4);  // cacheable, read/write
}

Bytes resource_template(std::initializer_list<Bytes> descriptors) {
  Bytes data = term_list(descriptors);
  data.push_back(0x79);  // end tag
  data.push_back(0x00);  // checksum 0 = "treat as correct"
  return buffer(data);
}

}  // namespace sandvm::acpi
