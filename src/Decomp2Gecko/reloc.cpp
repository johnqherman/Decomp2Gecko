#include "Decomp2Gecko/reloc.h"

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

const char* reloc_type_name(int reloc_type) {
    switch (reloc_type) {
        case R_PPC_ADDR32: return "ADDR32";
        case R_PPC_ADDR16: return "ADDR16";
        case R_PPC_ADDR16_LO: return "ADDR16_LO";
        case R_PPC_ADDR16_HI: return "ADDR16_HI";
        case R_PPC_ADDR16_HA: return "ADDR16_HA";
        case R_PPC_REL24: return "REL24";
        case R_PPC_REL14: return "REL14";
        case R_PPC_REL32: return "REL32";
        case R_PPC_EMB_SDA21: return "EMB_SDA21";
        default: return nullptr;
    }
}

int64_t sign_extend(int64_t value, int bits) {
    return (value & (int64_t(1) << (bits - 1))) ? value - (int64_t(1) << bits) : value;
}

bool is_halfword_type(int reloc_type) {
    return reloc_type == R_PPC_ADDR16 || reloc_type == R_PPC_ADDR16_LO || reloc_type == R_PPC_ADDR16_HI ||
        reloc_type == R_PPC_ADDR16_HA || reloc_type == R_PPC_EMB_SDA21;
}

bool is_word_type(int reloc_type) {
    return reloc_type == R_PPC_ADDR32 || reloc_type == R_PPC_REL24 || reloc_type == R_PPC_REL14 ||
        reloc_type == R_PPC_REL32;
}

void require_field(const Bytes& buffer, int64_t offset, int64_t width) {
    if (offset < 0 || offset + width > int64_t(buffer.size())) {
        throw InvalidValueError("relocation field of " + std::to_string(width) + " bytes at offset " +
            std::to_string(offset) + " lies outside a " + std::to_string(buffer.size()) + "-byte chunk");
    }
}

void put_u32(Bytes& buffer, int64_t offset, uint32_t value) {
    require_field(buffer, offset, 4);
    write_u32_be(buffer, size_t(offset), value);
}

void put_u16(Bytes& buffer, int64_t offset, uint16_t value) {
    require_field(buffer, offset, 2);
    write_u16_be(buffer, size_t(offset), value);
}

uint32_t get_u32(const Bytes& buffer, int64_t offset) {
    require_field(buffer, offset, 4);
    return read_u32_be(buffer, size_t(offset));
}

} // namespace

std::string reloc_type_name_or_none(int reloc_type) {
    const char* name = reloc_type_name(reloc_type);
    return name ? name : "None";
}

std::optional<std::pair<int64_t, int64_t>> relocation_field_span(int reloc_type, int64_t field_offset) {
    if (is_halfword_type(reloc_type)) {
        if (reloc_type == R_PPC_EMB_SDA21) {
            return std::make_pair(field_offset & ~int64_t(3), int64_t(4));
        }
        return std::make_pair(field_offset, int64_t(2));
    }
    if (is_word_type(reloc_type)) {
        return std::make_pair(field_offset, int64_t(4));
    }
    return std::nullopt;
}

std::optional<uint32_t> word_mask_for_relocation(int reloc_type, int64_t field_offset) {
    switch (reloc_type) {
        case R_PPC_ADDR32:
        case R_PPC_REL32: return 0xFFFFFFFFu;
        case R_PPC_REL24: return BRANCH_DISPLACEMENT_MASK;
        case R_PPC_REL14: return CONDITIONAL_DISPLACEMENT_MASK;
        case R_PPC_EMB_SDA21: return REGISTER_A_MASK | LOW_HALF_MASK;
        case R_PPC_ADDR16:
        case R_PPC_ADDR16_LO:
        case R_PPC_ADDR16_HI:
        case R_PPC_ADDR16_HA: return (field_offset & 2) ? LOW_HALF_MASK : (LOW_HALF_MASK << 16);
        default: return std::nullopt;
    }
}

std::optional<std::string> apply_relocation(Bytes& buffer, int64_t field_offset, int reloc_type, int64_t target_address,
    int64_t addend, int64_t site_address, const SmallDataBaseFor& small_data_base_for) {
    int64_t value = (target_address + addend) & 0xFFFFFFFF;

    if (reloc_type == R_PPC_ADDR32 || reloc_type == R_PPC_REL32) {
        if (reloc_type == R_PPC_REL32) {
            value = (value - site_address) & 0xFFFFFFFF;
        }
        put_u32(buffer, field_offset, uint32_t(value));

    } else if (reloc_type == R_PPC_ADDR16 || reloc_type == R_PPC_ADDR16_LO) {
        put_u16(buffer, field_offset, uint16_t(value & LOW_HALF_MASK));

    } else if (reloc_type == R_PPC_ADDR16_HI) {
        put_u16(buffer, field_offset, uint16_t((value >> 16) & LOW_HALF_MASK));

    } else if (reloc_type == R_PPC_ADDR16_HA) {
        // "high adjusted": compensates for the sign of the low half that gets added later
        put_u16(buffer, field_offset, uint16_t(((value + 0x8000) >> 16) & LOW_HALF_MASK));

    } else if (reloc_type == R_PPC_REL24) {
        int64_t displacement = (value - site_address) & 0xFFFFFFFF;
        int64_t signed_displacement = sign_extend(displacement, 32);
        if (!(-0x2000000 <= signed_displacement && signed_displacement < 0x2000000) || (signed_displacement & 3)) {
            return "REL24 out of range: site " + hex_upper(site_address, 8) + " -> " + hex_upper(value, 8);
        }
        uint32_t instruction = get_u32(buffer, field_offset);
        instruction = (instruction & ~BRANCH_DISPLACEMENT_MASK) | (uint32_t(displacement) & BRANCH_DISPLACEMENT_MASK);
        put_u32(buffer, field_offset, instruction);

    } else if (reloc_type == R_PPC_REL14) {
        int64_t displacement = (value - site_address) & 0xFFFFFFFF;
        int64_t signed_displacement = sign_extend(displacement, 32);
        if (!(-0x8000 <= signed_displacement && signed_displacement < 0x8000) || (signed_displacement & 3)) {
            return "REL14 out of range: site " + hex_upper(site_address, 8) + " -> " + hex_upper(value, 8);
        }
        uint32_t instruction = get_u32(buffer, field_offset);
        instruction =
            (instruction & ~CONDITIONAL_DISPLACEMENT_MASK) | (uint32_t(displacement) & CONDITIONAL_DISPLACEMENT_MASK);
        put_u32(buffer, field_offset, instruction);

    } else if (reloc_type == R_PPC_EMB_SDA21) {
        if ((field_offset & 3) != 2) {
            return "EMB_SDA21 reloc not at low halfword (offset " + hex_upper(field_offset) + ")";
        }
        std::optional<SmallDataBase> base = small_data_base_for(value);
        if (!base) {
            return "EMB_SDA21 target " + hex_upper(value, 8) + " is not in a small-data section";
        }
        auto [base_register, base_address] = *base;
        int64_t displacement = (value - base_address) & 0xFFFFFFFF;
        int64_t signed_displacement = sign_extend(displacement, 32);
        if (!(-0x8000 <= signed_displacement && signed_displacement < 0x8000)) {
            return "EMB_SDA21 target " + hex_upper(value, 8) + " out of range of r" + std::to_string(base_register) +
                " base " + hex_upper(base_address, 8);
        }
        // compiler leaves rA as r0, linker fills in r13/r2 & the 16-bit offset
        int64_t word_offset = field_offset - 2;
        uint32_t instruction = get_u32(buffer, word_offset);
        instruction = (instruction & ~REGISTER_A_MASK) | (uint32_t(base_register) << 16);
        instruction = (instruction & ~LOW_HALF_MASK) | (uint32_t(displacement) & LOW_HALF_MASK);
        put_u32(buffer, word_offset, instruction);

    } else {
        return "unsupported relocation type " + std::to_string(reloc_type);
    }
    return std::nullopt;
}

std::optional<int64_t> decode_relocation_target(const ReadWord& read_word, int64_t site_address, int reloc_type,
    const std::map<int, int64_t>& small_data_bases, const HighHalf& high_half) {
    if (reloc_type == R_PPC_ADDR32) {
        std::optional<uint32_t> word = read_word(site_address);
        if (!word) {
            return std::nullopt;
        }
        return int64_t(*word);
    }

    if (reloc_type == R_PPC_EMB_SDA21) {
        std::optional<uint32_t> instruction = read_word(site_address & ~int64_t(3));
        if (!instruction) {
            return std::nullopt;
        }
        auto base = small_data_bases.find(int((*instruction >> 16) & 0x1F));
        if (base == small_data_bases.end()) {
            return std::nullopt;
        }
        return (base->second + sign_extend(*instruction & LOW_HALF_MASK, 16)) & 0xFFFFFFFF;
    }

    if (reloc_type == R_PPC_REL24) {
        std::optional<uint32_t> instruction = read_word(site_address & ~int64_t(3));
        if (!instruction) {
            return std::nullopt;
        }
        return (site_address + sign_extend(*instruction & BRANCH_DISPLACEMENT_MASK, 26)) & 0xFFFFFFFF;
    }

    if (reloc_type == R_PPC_REL14) {
        std::optional<uint32_t> instruction = read_word(site_address & ~int64_t(3));
        if (!instruction) {
            return std::nullopt;
        }
        return (site_address + sign_extend(*instruction & CONDITIONAL_DISPLACEMENT_MASK, 16)) & 0xFFFFFFFF;
    }

    if (reloc_type == R_PPC_ADDR16_LO && high_half) {
        auto halfword_at = [&](int64_t address) -> std::optional<int64_t> {
            std::optional<uint32_t> word = read_word(address & ~int64_t(3));
            if (!word) {
                return std::nullopt;
            }
            return (address & 2) ? int64_t(*word & LOW_HALF_MASK) : int64_t(*word >> 16);
        };
        std::optional<int64_t> low = halfword_at(site_address);
        auto [high_site, high_type] = *high_half;
        std::optional<int64_t> high = halfword_at(high_site);
        if (!low || !high) {
            return std::nullopt;
        }
        if (high_type == R_PPC_ADDR16_HA) {
            return ((*high << 16) + sign_extend(*low, 16)) & 0xFFFFFFFF;
        }
        return ((*high << 16) | *low) & 0xFFFFFFFF;
    }

    return std::nullopt;
}

} // namespace Decomp2Gecko
