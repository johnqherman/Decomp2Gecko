#pragma once

#include <cstdint>
#include <optional>

namespace Decomp2Gecko::ppc {

inline constexpr uint32_t OPCODE_BC = 16; // B-form: bc, bcl
inline constexpr uint32_t OPCODE_B = 18; // I-form: b, bl
inline constexpr uint32_t OPCODE_XL = 19; // bclr / bcctr
inline constexpr uint32_t NOP = 0x60000000; // ori r0, r0, 0
inline constexpr uint32_t I_FORM_MASK = 0x03FFFFFC; // LI
inline constexpr uint32_t B_FORM_MASK = 0x0000FFFC; // BD
inline constexpr uint32_t AA_BIT = 0x2;
inline constexpr uint32_t LK_BIT = 0x1;
// BO is bits 21-25; toggling one of these flips a bc's sense
inline constexpr uint32_t BO_CONDITION_SENSE = 0x08 << 21; // 001zy <-> 011zy
inline constexpr uint32_t BO_COUNTER_SENSE = 0x02 << 21; // 1z00y <-> 1z01y (bdnz <-> bdz)

inline uint32_t opcode(uint32_t word) { return word >> 26; }
inline bool is_i_form(uint32_t word) { return opcode(word) == OPCODE_B; }
inline bool is_b_form(uint32_t word) { return opcode(word) == OPCODE_BC; }
inline bool is_absolute(uint32_t word) { return (word & AA_BIT) != 0; }
inline bool has_link(uint32_t word) { return (word & LK_BIT) != 0; }
inline uint32_t bo_field(uint32_t word) { return (word >> 21) & 0x1F; }
inline bool bo_always(uint32_t bo) { return (bo & 0x14) == 0x14; } // 1z1zz

inline std::optional<int64_t> relative_displacement(uint32_t word) {
    if (is_absolute(word)) {
        return std::nullopt;
    }
    if (is_i_form(word)) {
        int64_t displacement = word & I_FORM_MASK;
        return (displacement & 0x02000000) ? displacement - 0x04000000 : displacement;
    }
    if (is_b_form(word)) {
        int64_t displacement = word & B_FORM_MASK;
        return (displacement & 0x8000) ? displacement - 0x10000 : displacement;
    }
    return std::nullopt;
}

// b, bc always, blr, bctr (w/o LK): nothing falls through
inline bool is_unconditional(uint32_t word) {
    if (has_link(word)) {
        return false;
    }
    if (is_i_form(word)) {
        return true;
    }
    if (is_b_form(word)) {
        return bo_always(bo_field(word));
    }
    if (opcode(word) == OPCODE_XL) {
        uint32_t extended = (word >> 1) & 0x3FF;
        return (extended == 16 || extended == 528) && bo_always(bo_field(word));
    }
    return false;
}

inline bool i_form_reaches(int64_t from, int64_t to) {
    int64_t displacement = to - from;
    return -0x2000000 <= displacement && displacement < 0x2000000 && (displacement & 3) == 0;
}

inline bool b_form_reaches(int64_t from, int64_t to) {
    int64_t displacement = to - from;
    return -0x8000 <= displacement && displacement < 0x8000 && (displacement & 3) == 0;
}

inline std::optional<uint32_t> encode_branch(int64_t from, int64_t to, bool link = false) {
    if (!i_form_reaches(from, to)) {
        return std::nullopt;
    }
    return (OPCODE_B << 26) | (uint32_t(to - from) & I_FORM_MASK) | (link ? LK_BIT : 0);
}

inline std::optional<uint32_t> retarget(uint32_t word, int64_t from, int64_t to) {
    if (is_i_form(word)) {
        if (!i_form_reaches(from, to)) {
            return std::nullopt;
        }
        return (word & ~I_FORM_MASK) | (uint32_t(to - from) & I_FORM_MASK);
    }
    if (is_b_form(word)) {
        if (!b_form_reaches(from, to)) {
            return std::nullopt;
        }
        return (word & ~B_FORM_MASK) | (uint32_t(to - from) & B_FORM_MASK);
    }
    return std::nullopt;
}

// nothing for branch-always, the counter+condition forms, or bcl (its LR would name the helper)
inline std::optional<uint32_t> inversion_toggle(uint32_t word) {
    if (!is_b_form(word) || has_link(word)) {
        return std::nullopt;
    }
    uint32_t bo = bo_field(word);
    bool ignores_condition = (bo & 0x10) != 0;
    bool keeps_counter = (bo & 0x04) != 0;
    if (ignores_condition && keeps_counter) {
        return std::nullopt; // always
    }
    if (!ignores_condition && keeps_counter) {
        return BO_CONDITION_SENSE;
    }
    if (ignores_condition && !keeps_counter) {
        return BO_COUNTER_SENSE;
    }
    return std::nullopt;
}

} // namespace Decomp2Gecko::ppc
