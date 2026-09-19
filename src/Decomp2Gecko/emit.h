#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Decomp2Gecko/layout.h"

namespace Decomp2Gecko {

inline constexpr int64_t GECKO_BASE_ADDRESS = 0x80000000;
inline constexpr int64_t GECKO_ADDRESS_SPACE = 0x02000000;
inline constexpr int64_t DOL_ADDRESS_LIMIT = 0x81000000; // above this = free region
inline constexpr int64_t GECKO_ADDRESS_MASK = 0x01FFFFFF;
inline constexpr int GECKO_IF_EQUAL = 0x20;
inline constexpr int GECKO_WRITE32 = 0x04;
inline constexpr int GECKO_STRING_WRITE = 0x06;
inline constexpr int GECKO_SERIAL_WRITE = 0x08;
inline constexpr int GECKO_ENDIF = 0xE2;

struct MemoryWrite {
    int64_t address;
    Bytes data;
};

using GuardWord = std::optional<std::pair<int64_t, uint32_t>>; // (address, vanilla word)

struct CodeSet {
    std::string name;
    GuardWord guard;
    std::vector<MemoryWrite> writes;
    std::vector<std::string> lines;
    int64_t bss_zero_bytes = 0;

    int64_t in_dol_bytes() const;
    int64_t relocated_bytes() const;
    std::vector<AddressRange> ranges() const;
};

std::vector<std::string> encode_gecko_lines(const std::vector<MemoryWrite>& writes, const GuardWord& guard);

CodeSet build_code_set(const Layout& layout, const std::string& name);

std::vector<std::string> reserved_conflicts(const Layout& layout, const CodeSet& code_set);

std::string format_ini(const CodeSet& code_set, const std::string& author = "Decomp2Gecko");

void simulate_codes(const std::vector<std::string>& lines, MemImage& image);

} // namespace Decomp2Gecko
