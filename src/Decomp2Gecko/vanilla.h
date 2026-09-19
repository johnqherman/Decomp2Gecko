#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Decomp2Gecko/bytes.h"
#include "Decomp2Gecko/ordered_map.h"

namespace Decomp2Gecko {

using AddressRange = std::pair<int64_t, int64_t>; // [start, end)

class MemImage {
public:
    void add_region(int64_t address, int64_t size, std::string_view data = {});

    bool contains(int64_t address, int64_t length = 1) const;
    std::optional<Bytes> try_read(int64_t address, int64_t length) const;
    std::optional<uint32_t> try_word(int64_t address) const;
    Bytes read(int64_t address, int64_t length) const;
    uint32_t word(int64_t address) const;
    void write(int64_t address, std::string_view data);

private:
    const Bytes* region_holding(int64_t address, int64_t length, int64_t* region_start) const;

    std::vector<int64_t> region_starts_;
    std::vector<Bytes> region_buffers_;
};

// GameCube cached MEM1 address space
inline constexpr int64_t GC_MEM1_START = 0x80000000;
inline constexpr int64_t GC_MEM1_END = 0x81800000;

// DOL header: 7 text + 11 data slots, each w/ offset, load address & size tables
inline constexpr int DOL_SECTION_SLOTS = 18;
inline constexpr int64_t DOL_OFFSET_TABLE = 0x00;
inline constexpr int64_t DOL_ADDRESS_TABLE = 0x48;
inline constexpr int64_t DOL_SIZE_TABLE = 0x90;
inline constexpr int64_t DOL_ENTRY_FIELD = 0xE0;

MemImage read_dol(const std::filesystem::path& path);

struct VanillaSymbol {
    std::string name;
    std::string section;
    int64_t address = 0;
    int64_t size = 0;
};

// MWCC suffixes function-scope statics w/ "$NNN" ("to_synth_mode$881"), a line counter that
// drifts w/ every edit above, so names match w/o it
std::string base_name(std::string_view symbol_name);

class Symbols {
public:
    explicit Symbols(const std::filesystem::path& path);
    static Symbols from_text(std::string_view text);

    // all symbols sorted by address, symbols at the same address keep file order
    const std::vector<const VanillaSymbol*>& entries() const { return entries_; }
    const std::vector<const VanillaSymbol*>& by_name(const std::string& name) const;

    std::vector<const VanillaSymbol*> starting_at(int64_t address) const;
    std::vector<const VanillaSymbol*> named_in_range(const std::string& name, int64_t start, int64_t end) const;
    std::vector<const VanillaSymbol*> named_in_section(const std::string& name, const std::string& section) const;

private:
    Symbols() = default;
    void parse(std::string_view text);
    const std::vector<const VanillaSymbol*>& by_base_name(const std::string& name) const;

    std::vector<VanillaSymbol> storage_;
    std::vector<const VanillaSymbol*> entries_;
    std::vector<int64_t> addresses_;
    std::unordered_map<std::string, std::vector<const VanillaSymbol*>> by_name_;
    std::unordered_map<std::string, std::vector<const VanillaSymbol*>> by_base_name_;
};

class Splits {
public:
    using SectionRanges = OrderedMap<std::string, AddressRange>;

    explicit Splits(const std::filesystem::path& path);
    static Splits from_text(std::string_view text);

    OrderedMap<std::string, SectionRanges> units;
    std::unordered_map<std::string, AddressRange> section_ranges;

    std::optional<AddressRange> unit_range(const std::string& unit, const std::string& section) const;

private:
    Splits() = default;
    void parse(std::string_view text);
};

} // namespace Decomp2Gecko
