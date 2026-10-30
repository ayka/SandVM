#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace sandvm::acpi {

// Just enough of an AML encoder (ACPI 6.x, chapter 20) for a static DSDT: names, integers, strings,
// packages, buffers, scopes, devices and processors. Every function returns the encoded bytes of one
// term; containers take their already-encoded children.
using Bytes = std::vector<uint8_t>;

Bytes integer(uint64_t value);  // shortest of Zero/One/Byte/Word/DWord/QWord constant
Bytes string(const std::string& text);
// EISA ID such as "PNP0A03", compressed into the 32-bit form _HID uses.
Bytes eisa_id(const char* id);
// A NameString: "\\_SB.PCI0", "_HID", "^FOO". Segments shorter than four characters are padded with '_'.
Bytes name_string(const std::string& path);

Bytes name(const std::string& path, const Bytes& value);  // Name(path, value)
Bytes package(std::initializer_list<Bytes> elements);
Bytes package(const std::vector<Bytes>& elements);
Bytes buffer(const Bytes& data);
Bytes scope(const std::string& path, std::initializer_list<Bytes> terms);
Bytes device(const std::string& name, std::initializer_list<Bytes> terms);
Bytes device(const std::string& name, const std::vector<Bytes>& terms);
// Processor() is deprecated since ACPI 6.0 but is what Windows 7 and older kernels look for.
Bytes processor(const std::string& name, uint8_t id, uint32_t pblk_addr, uint8_t pblk_len);

// Resource descriptors (ACPI 6.x, 6.4), concatenated and wrapped by resource_template().
Bytes io(uint16_t base, uint8_t length, uint8_t align = 1);
Bytes irq_no_flags(unsigned irq);
Bytes word_bus_number(uint16_t min, uint16_t max);
Bytes word_io(uint16_t min, uint16_t max);
Bytes dword_memory(uint32_t min, uint32_t max);
Bytes resource_template(std::initializer_list<Bytes> descriptors);

Bytes concat(std::initializer_list<Bytes> parts);

}  // namespace sandvm::acpi
