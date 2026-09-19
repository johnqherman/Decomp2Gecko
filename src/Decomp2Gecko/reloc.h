// only MWCC's seven relocation types; anything else errors out rather than patch quietly wrong
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "Decomp2Gecko/bytes.h"

namespace Decomp2Gecko {

inline constexpr int R_PPC_ADDR32 = 1;
inline constexpr int R_PPC_ADDR16 = 3;
inline constexpr int R_PPC_ADDR16_LO = 4;
inline constexpr int R_PPC_ADDR16_HI = 5;
inline constexpr int R_PPC_ADDR16_HA = 6;
inline constexpr int R_PPC_REL24 = 10;
inline constexpr int R_PPC_REL14 = 11;
inline constexpr int R_PPC_REL32 = 26;
inline constexpr int R_PPC_EMB_SDA21 = 109;

std::string reloc_type_name_or_none(int reloc_type);

inline constexpr uint32_t BRANCH_DISPLACEMENT_MASK = 0x03FFFFFC; // LI field of I-form branches (b, bl)
inline constexpr uint32_t CONDITIONAL_DISPLACEMENT_MASK = 0xFFFC; // BD field of B-form branches (bc)
inline constexpr uint32_t LOW_HALF_MASK = 0xFFFF;
inline constexpr uint32_t REGISTER_A_MASK = 0x1F << 16; // rA field of D-form instructions

using SmallDataBase = std::pair<int, int64_t>;
using SmallDataBaseFor = std::function<std::optional<SmallDataBase>(int64_t)>;

// MWCC points halfword relocations at the immediate. EMB_SDA21 also rewrites rA up top, so it spans the word
std::optional<std::pair<int64_t, int64_t>> relocation_field_span(int reloc_type, int64_t field_offset);

std::optional<uint32_t> word_mask_for_relocation(int reloc_type, int64_t field_offset);

// site_address: runtime address of the field's instruction word (PC-relative types)
std::optional<std::string> apply_relocation(Bytes& buffer, int64_t field_offset, int reloc_type, int64_t target_address,
    int64_t addend, int64_t site_address, const SmallDataBaseFor& small_data_base_for);

using ReadWord = std::function<std::optional<uint32_t>(int64_t)>;
using HighHalf = std::optional<std::pair<int64_t, int>>; // (site address, type) of an @ha/@hi partner

// an ADDR16_LO field is half an address: pass its @ha/@hi partner (site, type) to combine them
std::optional<int64_t> decode_relocation_target(const ReadWord& read_word, int64_t site_address, int reloc_type,
    const std::map<int, int64_t>& small_data_bases, const HighHalf& high_half = std::nullopt);

} // namespace Decomp2Gecko
