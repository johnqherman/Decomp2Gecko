#include "Decomp2Gecko/emit.h"

#include <algorithm>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/gecko_cost.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

int64_t CodeSet::in_dol_bytes() const {
    int64_t total = 0;
    for (const MemoryWrite& write : writes) {
        if (write.address < DOL_ADDRESS_LIMIT) {
            total += int64_t(write.data.size());
        }
    }
    return total;
}

int64_t CodeSet::relocated_bytes() const {
    int64_t total = 0;
    for (const MemoryWrite& write : writes) {
        if (write.address >= DOL_ADDRESS_LIMIT) {
            total += int64_t(write.data.size());
        }
    }
    return total;
}

std::vector<AddressRange> CodeSet::ranges() const {
    std::vector<AddressRange> touched;
    for (const MemoryWrite& write : writes) {
        touched.emplace_back(write.address, write.address + int64_t(write.data.size()));
    }
    return touched;
}

namespace {

int64_t gecko_offset(int64_t address) {
    int64_t offset = address - GECKO_BASE_ADDRESS;
    if (!(0 <= offset && offset < GECKO_ADDRESS_SPACE)) {
        throw InvalidValueError("address " + hex_upper(address, 8) + " not encodable in a Gecko code");
    }
    return offset;
}

std::pair<std::vector<MemoryWrite>, int64_t> collect_memory_writes(const Layout& layout) {
    const Decomp& decomp = layout.decomp();
    const MemImage& image = layout.image();
    std::vector<AddressRange> changed;
    int64_t bss_zero_bytes = 0;

    for (const Chunk* chunk : layout.chunks()) {
        for (const Extent& extent : layout.chunk_extents(*chunk)) {
            auto [start, end] = extent.range;
            if (extent.moved) {
                changed.emplace_back(start, end);
                if (chunk->is_bss()) { // DOL loader zeroes in-place bss, but relocated bss we must clear
                    bss_zero_bytes += end - start;
                }
                continue;
            }

            // whole words, so a lone changed word is one aligned 04; exact bytes when rounding would leave the image
            int64_t word_start = start & ~int64_t(3);
            int64_t word_end = (end + 3) & ~int64_t(3);
            if (!image.contains(word_start, word_end - word_start)) {
                word_start = start;
                word_end = end;
            }
            Bytes vanilla_bytes = decomp.dol.read(word_start, word_end - word_start);
            Bytes patched_bytes = image.read(word_start, word_end - word_start);
            if (vanilla_bytes == patched_bytes) {
                continue;
            }
            for (int64_t position = 0; position < word_end - word_start; position += 4) {
                int64_t step = std::min<int64_t>(4, word_end - word_start - position);
                if (slice(vanilla_bytes, position, position + step) !=
                    slice(patched_bytes, position, position + step)) {
                    changed.emplace_back(word_start + position, word_start + position + step);
                }
            }
        }
    }

    std::sort(changed.begin(), changed.end());
    std::vector<AddressRange> merged;
    for (const auto& [start, end] : changed) {
        if (!merged.empty() && start <= merged.back().second) {
            merged.back().second = std::max(merged.back().second, end);
        } else {
            merged.emplace_back(start, end);
        }
    }

    std::vector<MemoryWrite> writes;
    auto can_span = [&image](int64_t start, int64_t end) { return image.contains(start, end - start); };
    for (const auto& [start, end] : segment_writes(merged, can_span)) {
        writes.push_back({start, image.read(start, end - start)});
    }
    return {std::move(writes), bss_zero_bytes};
}

} // namespace

std::vector<std::string> encode_gecko_lines(const std::vector<MemoryWrite>& writes, const GuardWord& guard) {
    std::vector<std::string> lines;
    if (guard) {
        auto [guard_address, vanilla_word] = *guard;
        lines.push_back(hex_upper((int64_t(GECKO_IF_EQUAL) << 24) | gecko_offset(guard_address), 8) + " " +
            hex_upper(vanilla_word, 8));
    }

    for (const MemoryWrite& write : writes) {
        if (write.data.size() == 4 && write.address % 4 == 0) {
            lines.push_back(hex_upper((int64_t(GECKO_WRITE32) << 24) | gecko_offset(write.address), 8) + " " +
                hex_upper(read_u32_be(write.data, 0), 8));
            continue;
        }

        lines.push_back(hex_upper((int64_t(GECKO_STRING_WRITE) << 24) | gecko_offset(write.address), 8) + " " +
            hex_upper(int64_t(write.data.size()), 8));
        Bytes padded = write.data + Bytes((8 - write.data.size() % 8) % 8, '\0');
        for (size_t position = 0; position < padded.size(); position += 8) {
            lines.push_back(
                hex_upper(read_u32_be(padded, position), 8) + " " + hex_upper(read_u32_be(padded, position + 4), 8));
        }
    }

    if (guard) {
        lines.push_back("E2000001 00000000");
    }
    return lines;
}

CodeSet build_code_set(const Layout& layout, const std::string& name) {
    auto [writes, bss_zero_bytes] = collect_memory_writes(layout);
    // guard on the first changed DOL word
    GuardWord guard_pair;
    for (const MemoryWrite& write : writes) {
        if (layout.decomp().dol.contains(write.address, 4)) {
            guard_pair = std::make_pair(write.address, layout.decomp().dol.word(write.address));
            break;
        }
    }
    CodeSet code_set;
    code_set.name = name;
    code_set.guard = guard_pair;
    code_set.writes = std::move(writes);
    code_set.bss_zero_bytes = bss_zero_bytes;
    code_set.lines = encode_gecko_lines(code_set.writes, guard_pair);
    return code_set;
}

std::vector<std::string> reserved_conflicts(const Layout& layout, const CodeSet& code_set) {
    std::vector<std::string> conflicts;
    for (const auto& [start, end] : code_set.ranges()) {
        if (const ReservedRange* reserved = layout.decomp().game.reserved_hit(start, end)) {
            conflicts.push_back("write " + hex_upper(start, 8) + "-" + hex_upper(end, 8) + " overlaps reserved " +
                hex_upper(reserved->start, 8) + "-" + hex_upper(reserved->end, 8) + " (" + reserved->reason + ")");
        }
    }
    return conflicts;
}

std::string format_ini(const CodeSet& code_set, const std::string& author) {
    std::vector<std::string> lines = {"[Gecko]", "$" + code_set.name + " [" + author + "]"};
    lines.insert(lines.end(), code_set.lines.begin(), code_set.lines.end());
    lines.push_back("");
    lines.push_back("[Gecko_Enabled]");
    lines.push_back("$" + code_set.name);
    lines.push_back("");
    return join(lines, "\n");
}

void simulate_codes(const std::vector<std::string>& lines, MemImage& image) {
    std::vector<std::pair<int64_t, int64_t>> words;
    for (const std::string& line : lines) {
        std::vector<std::string> tokens = split_whitespace(line);
        if (tokens.size() != 2) {
            throw InvalidValueError("expected two words per Gecko line: " + quote_for_message(line));
        }
        words.emplace_back(parse_hex(tokens[0]), parse_hex(tokens[1]));
    }

    auto word_at = [&](size_t index) -> const std::pair<int64_t, int64_t>& {
        if (index >= words.size()) {
            throw InvalidValueError("truncated Gecko code list");
        }
        return words[index];
    };

    size_t position = 0;
    int64_t skip_depth = 0; // how many enclosing conditionals are false right now
    while (position < words.size()) {
        auto [first, second] = words[position];
        int64_t code_type = first >> 24;
        int64_t address = GECKO_BASE_ADDRESS + (first & GECKO_ADDRESS_MASK);

        if (code_type == GECKO_ENDIF) {
            skip_depth = std::max(int64_t(0), skip_depth - (first & 0xFF));
            position += 1;
        } else if (code_type == GECKO_IF_EQUAL || code_type == GECKO_IF_EQUAL + 1) {
            if (skip_depth || image.word(address & ~int64_t(3)) != uint32_t(second)) {
                skip_depth += 1;
            }
            position += 1;
        } else if (code_type == GECKO_WRITE32 || code_type == GECKO_WRITE32 + 1) {
            if (!skip_depth) {
                image.write(address, packed_u32_be(uint32_t(second)));
            }
            position += 1;
        } else if (code_type == GECKO_STRING_WRITE || code_type == GECKO_STRING_WRITE + 1) {
            int64_t length = second;
            int64_t data_lines = (length + 7) / 8;
            Bytes data;
            for (int64_t index = 0; index < data_lines; index++) {
                const auto& [high, low] = word_at(position + 1 + size_t(index));
                data += packed_u32_be(uint32_t(high));
                data += packed_u32_be(uint32_t(low));
            }
            data.resize(size_t(std::min(length, int64_t(data.size()))));

            if (!skip_depth) {
                image.write(address, data);
            }
            position += 1 + size_t(data_lines);
        } else if (code_type == GECKO_SERIAL_WRITE || code_type == GECKO_SERIAL_WRITE + 1) {
            auto [control, value_step] = word_at(position + 1);
            int64_t width_code = control >> 28;
            int64_t count = ((control >> 16) & 0xFFF) + 1;
            int64_t address_step = control & 0xFFFF;

            if (!skip_depth) {
                int64_t value = second;
                for (int64_t index = 0; index < count; index++) {
                    int64_t target = address + index * address_step;
                    if (width_code == 2) {
                        image.write(target, packed_u32_be(uint32_t(value & 0xFFFFFFFF)));
                    } else if (width_code == 1) {
                        Bytes halfword(2, '\0');
                        write_u16_be(halfword, 0, uint16_t(value & 0xFFFF));
                        image.write(target, halfword);
                    } else {
                        image.write(target, Bytes(1, char(value & 0xFF)));
                    }
                    value += value_step;
                }
            }
            position += 2;
        } else {
            throw InvalidValueError("unsupported code type " + hex_upper(code_type, 2) + " in simulation");
        }
    }
}

} // namespace Decomp2Gecko
