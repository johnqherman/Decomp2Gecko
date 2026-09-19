#include "Decomp2Gecko/vanilla.h"

#include <algorithm>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {
void MemImage::add_region(int64_t address, int64_t size, std::string_view data) {
    if (int64_t(data.size()) > size) {
        throw InvalidValueError("region " + hex_upper(address, 8) + " data exceeds its size");
    }
    Bytes buffer(size_t(size), '\0');
    buffer.replace(0, data.size(), data);
    auto insert_at = std::lower_bound(region_starts_.begin(), region_starts_.end(), address);
    size_t insert_index = size_t(insert_at - region_starts_.begin());
    if (insert_index > 0) {
        int64_t previous_end = region_starts_[insert_index - 1] + int64_t(region_buffers_[insert_index - 1].size());
        if (previous_end > address) {
            throw InvalidValueError("region " + hex_upper(address, 8) + " overlaps previous region");
        }
    }
    if (insert_index < region_starts_.size() && address + size > region_starts_[insert_index]) {
        throw InvalidValueError("region " + hex_upper(address, 8) + " overlaps next region");
    }
    region_starts_.insert(region_starts_.begin() + ptrdiff_t(insert_index), address);
    region_buffers_.insert(region_buffers_.begin() + ptrdiff_t(insert_index), std::move(buffer));
}

const Bytes* MemImage::region_holding(int64_t address, int64_t length, int64_t* region_start) const {
    auto after = std::upper_bound(region_starts_.begin(), region_starts_.end(), address);
    if (after == region_starts_.begin()) {
        return nullptr;
    }
    size_t index = size_t(after - region_starts_.begin()) - 1;
    int64_t start = region_starts_[index];
    const Bytes& buffer = region_buffers_[index];
    if (address + length <= start + int64_t(buffer.size())) {
        *region_start = start;
        return &buffer;
    }
    return nullptr;
}

bool MemImage::contains(int64_t address, int64_t length) const {
    int64_t start;
    return region_holding(address, length, &start) != nullptr;
}

std::optional<Bytes> MemImage::try_read(int64_t address, int64_t length) const {
    int64_t start;
    const Bytes* buffer = region_holding(address, length, &start);
    if (buffer == nullptr) {
        return std::nullopt;
    }
    return buffer->substr(size_t(address - start), size_t(length));
}

std::optional<uint32_t> MemImage::try_word(int64_t address) const {
    std::optional<Bytes> word_bytes = try_read(address, 4);
    if (!word_bytes) {
        return std::nullopt;
    }
    return read_u32_be(*word_bytes, 0);
}

Bytes MemImage::read(int64_t address, int64_t length) const {
    std::optional<Bytes> data = try_read(address, length);
    if (!data) {
        throw InternalError("address range " + hex_upper(address, 8) + "+" + hex_upper(length) + " not in image");
    }
    return *data;
}

uint32_t MemImage::word(int64_t address) const { return read_u32_be(read(address, 4), 0); }

void MemImage::write(int64_t address, std::string_view data) {
    int64_t start;
    const Bytes* buffer = region_holding(address, int64_t(data.size()), &start);
    if (buffer == nullptr) {
        throw InternalError(
            "address range " + hex_upper(address, 8) + "+" + hex_upper(int64_t(data.size())) + " not in image");
    }
    const_cast<Bytes*>(buffer)->replace(size_t(address - start), data.size(), data);
}

namespace {

MemImage read_dol_bytes(const Bytes& raw) {
    auto u32_at = [&](int64_t offset) -> int64_t {
        if (offset + 4 > int64_t(raw.size())) {
            throw InvalidValueError("truncated DOL header");
        }
        return read_u32_be(raw, size_t(offset));
    };
    MemImage image;
    for (int slot = 0; slot < DOL_SECTION_SLOTS; slot++) {
        int64_t file_offset = u32_at(DOL_OFFSET_TABLE + slot * 4);
        int64_t load_address = u32_at(DOL_ADDRESS_TABLE + slot * 4);
        int64_t size = u32_at(DOL_SIZE_TABLE + slot * 4);
        if (file_offset && size) {
            image.add_region(load_address, size, slice(raw, file_offset, file_offset + size));
        }
    }
    return image;
}

} // namespace

MemImage read_dol(const std::filesystem::path& path) { return read_dol_bytes(read_file_bytes(path)); }

std::string base_name(std::string_view symbol_name) {
    size_t dollar = symbol_name.rfind('$');
    if (dollar == std::string_view::npos || dollar + 1 >= symbol_name.size()) {
        return std::string(symbol_name);
    }
    for (size_t index = dollar + 1; index < symbol_name.size(); index++) {
        if (symbol_name[index] < '0' || symbol_name[index] > '9') {
            return std::string(symbol_name);
        }
    }
    return std::string(symbol_name.substr(0, dollar));
}

namespace {

// "ftPartsTable = .sbss:0x804D6544; // type:object size:0x4 scope:global data:4byte"
// ^(\S+)\s*=\s*([\w.]+):0x([0-9A-Fa-f]+);\s*//\s*(.*)$
bool parse_symbol_line(
    std::string_view line, std::string* name, std::string* section, std::string* address_hex, std::string* attributes) {
    size_t position = 0;
    while (position < line.size() && !is_ascii_space(line[position]) && line[position] != '=') {
        position++;
    }
    if (position == 0) {
        return false;
    }
    *name = std::string(line.substr(0, position));
    while (position < line.size() && is_ascii_space(line[position])) {
        position++;
    }
    if (position >= line.size() || line[position] != '=') {
        return false;
    }
    position++;

    while (position < line.size() && is_ascii_space(line[position])) {
        position++;
    }
    size_t section_start = position;
    while (position < line.size() && (is_word_char(line[position]) || line[position] == '.')) {
        position++;
    }
    if (position == section_start) {
        return false;
    }
    *section = std::string(line.substr(section_start, position - section_start));
    if (line.compare(position, 3, ":0x") != 0) {
        return false;
    }
    position += 3;

    size_t hex_start = position;
    while (position < line.size() && is_hex_digit(line[position])) {
        position++;
    }
    if (position == hex_start) {
        return false;
    }
    *address_hex = std::string(line.substr(hex_start, position - hex_start));
    if (position >= line.size() || line[position] != ';') {
        return false;
    }
    position++;

    while (position < line.size() && is_ascii_space(line[position])) {
        position++;
    }
    if (line.compare(position, 2, "//") != 0) {
        return false;
    }
    position += 2;
    while (position < line.size() && is_ascii_space(line[position])) {
        position++;
    }
    *attributes = std::string(line.substr(position));
    return true;
}

} // namespace

Symbols::Symbols(const std::filesystem::path& path) { parse(read_file_bytes(path)); }

Symbols Symbols::from_text(std::string_view text) {
    Symbols symbols;
    symbols.parse(text);
    return symbols;
}

void Symbols::parse(std::string_view text) {
    for (const std::string& raw_line : split_lines(text)) {
        std::string name, section, address_hex, attribute_text;
        if (!parse_symbol_line(strip(raw_line), &name, &section, &address_hex, &attribute_text)) {
            continue;
        }

        VanillaSymbol symbol;
        symbol.name = name;
        symbol.section = section;
        symbol.address = parse_hex(address_hex);
        // only the size attribute matters, a symbol w/o one (a label) has no size
        for (const std::string& token : split_whitespace(attribute_text)) {
            if (starts_with(token, "size:")) {
                symbol.size = parse_hex(token.substr(5));
            }
        }
        storage_.push_back(std::move(symbol));
    }

    // stable sort (symbols sharing an address stay in file order)
    for (const VanillaSymbol& symbol : storage_) {
        by_name_[symbol.name].push_back(&symbol);
        by_base_name_[base_name(symbol.name)].push_back(&symbol);
        entries_.push_back(&symbol);
    }

    std::stable_sort(entries_.begin(), entries_.end(),
        [](const VanillaSymbol* a, const VanillaSymbol* b) { return a->address < b->address; });
    for (const VanillaSymbol* symbol : entries_) {
        addresses_.push_back(symbol->address);
    }
}

const std::vector<const VanillaSymbol*>& Symbols::by_name(const std::string& name) const {
    static const std::vector<const VanillaSymbol*> none;
    auto found = by_name_.find(name);
    return found == by_name_.end() ? none : found->second;
}

const std::vector<const VanillaSymbol*>& Symbols::by_base_name(const std::string& name) const {
    static const std::vector<const VanillaSymbol*> none;
    auto found = by_base_name_.find(name);
    return found == by_base_name_.end() ? none : found->second;
}

std::vector<const VanillaSymbol*> Symbols::starting_at(int64_t address) const {
    std::vector<const VanillaSymbol*> found;
    size_t index = size_t(std::lower_bound(addresses_.begin(), addresses_.end(), address) - addresses_.begin());
    while (index < entries_.size() && entries_[index]->address == address) {
        found.push_back(entries_[index]);
        index++;
    }
    return found;
}

std::vector<const VanillaSymbol*> Symbols::named_in_range(const std::string& name, int64_t start, int64_t end) const {
    std::vector<const VanillaSymbol*> found;
    for (const VanillaSymbol* symbol : by_base_name(base_name(name))) {
        if (start <= symbol->address && symbol->address < end) {
            found.push_back(symbol);
        }
    }
    return found;
}

std::vector<const VanillaSymbol*> Symbols::named_in_section(const std::string& name, const std::string& section) const {
    std::vector<const VanillaSymbol*> found;
    for (const VanillaSymbol* symbol : by_base_name(base_name(name))) {
        if (symbol->section == section) {
            found.push_back(symbol);
        }
    }
    return found;
}

namespace {

// a header w/ trailing attributes ("MetroTRK/__exception.s: comment:0") isn't one: its ranges
// attach to the previous unit & locals there map by that
bool parse_unit_line(std::string_view line, std::string* unit_name) {
    std::string trimmed = rstrip(line);
    if (trimmed.size() < 2 || trimmed.back() != ':') {
        return false;
    }
    std::string_view name(trimmed.data(), trimmed.size() - 1);
    for (char character : name) {
        if (is_ascii_space(character)) {
            return false;
        }
    }
    *unit_name = std::string(name);
    return true;
}

// indented range line: "<section> start:0x... end:0x...", trailing text is ignored
bool parse_range_line(std::string_view line, std::string* section_name, int64_t* start, int64_t* end) {
    size_t position = 0;
    while (position < line.size() && is_ascii_space(line[position])) {
        position++;
    }
    if (position == 0) {
        return false;
    }
    size_t name_start = position;
    while (position < line.size() && !is_ascii_space(line[position])) {
        position++;
    }
    if (position == name_start) {
        return false;
    }
    std::string name(line.substr(name_start, position - name_start));
    auto expect_field = [&](std::string_view prefix, int64_t* value) {
        size_t space_start = position;
        while (position < line.size() && is_ascii_space(line[position])) {
            position++;
        }
        if (position == space_start) {
            return false;
        }
        if (line.compare(position, prefix.size(), prefix) != 0) {
            return false;
        }
        position += prefix.size();
        size_t hex_start = position;
        while (position < line.size() && is_hex_digit(line[position])) {
            position++;
        }
        if (position == hex_start) {
            return false;
        }
        *value = parse_hex(line.substr(hex_start, position - hex_start));
        return true;
    };
    if (!expect_field("start:0x", start) || !expect_field("end:0x", end)) {
        return false;
    }
    *section_name = name;
    return true;
}

} // namespace

Splits::Splits(const std::filesystem::path& path) { parse(read_file_bytes(path)); }

Splits Splits::from_text(std::string_view text) {
    Splits splits;
    splits.parse(text);
    return splits;
}

void Splits::parse(std::string_view text) {
    std::optional<std::string> current_unit;
    for (const std::string& line : split_lines(text)) {
        std::string unit_name;
        if (parse_unit_line(line, &unit_name)) {
            current_unit = unit_name;
            units[unit_name].clear(); // repeated unit name resets its sections but keeps its position
            continue;
        }

        std::string section_name;
        int64_t start, end;
        if (parse_range_line(line, &section_name, &start, &end) && current_unit) {
            units[*current_unit][section_name] = {start, end};
        }
    }

    for (const auto& [unit, unit_sections] : units) {
        for (const auto& [section_name, range] : unit_sections) {
            auto extent = section_ranges.find(section_name);
            if (extent == section_ranges.end()) {
                section_ranges.emplace(section_name, range);
            } else {
                extent->second = {
                    std::min(extent->second.first, range.first), std::max(extent->second.second, range.second)};
            }
        }
    }
}

std::optional<AddressRange> Splits::unit_range(const std::string& unit, const std::string& section) const {
    const SectionRanges* unit_sections = units.find(unit);
    if (unit_sections == nullptr) {
        return std::nullopt;
    }
    const AddressRange* range = unit_sections->find(section);
    if (range == nullptr) {
        return std::nullopt;
    }
    return *range;
}

} // namespace Decomp2Gecko
