#include "check.h"

#include <optional>

#include "Decomp2Gecko/reloc.h"

using namespace Decomp2Gecko;

namespace {

const std::map<int, int64_t> SMALL_DATA_BASES = {{13, 0x804DB6A0}, {2, 0x804DF9E0}};

std::optional<SmallDataBase> small_data_base_for(int64_t target_address) {
    if (target_address >= 0x804D79E0) {
        return SmallDataBase{2, SMALL_DATA_BASES.at(2)};
    }
    return SmallDataBase{13, SMALL_DATA_BASES.at(13)};
}

uint32_t word_at(const Bytes& buffer, int64_t offset) { return read_u32_be(buffer, size_t(offset)); }

} // namespace

TEST(reloc_rare_types) {
    // beq: 14-bit conditional branch displacement
    Bytes branch = packed_u32_be(0x41820000);
    int64_t site = 0x80001000;
    CHECK(!apply_relocation(branch, 0, R_PPC_REL14, site + 0x40, 0, site, small_data_base_for).has_value());
    CHECK_EQ(word_at(branch, 0), uint32_t(0x41820040));
    ReadWord read_word = [&](int64_t address) -> std::optional<uint32_t> { return word_at(branch, address - site); };
    CHECK_EQ(*decode_relocation_target(read_word, site, R_PPC_REL14, SMALL_DATA_BASES), site + 0x40);
    // @hi = plain high half, @ha compensates for a low half that'll be sign-extended
    Bytes lis = packed_u32_be(0x3C600000);
    apply_relocation(lis, 2, R_PPC_ADDR16_HI, 0x8034FFF0, 0, 0, small_data_base_for);
    CHECK_EQ(word_at(lis, 0) & 0xFFFF, uint32_t(0x8034));
    apply_relocation(lis, 2, R_PPC_ADDR16_HA, 0x8034FFF0, 0, 0, small_data_base_for);
    CHECK_EQ(word_at(lis, 0) & 0xFFFF, uint32_t(0x8035));
}

TEST(reloc_sda21) {
    // lwz r0, ftPartsTable@sda21(r0), r13-relative. word matches vanilla Fighter_LoadCommonData
    Bytes load = packed_u32_be(0x80000000);
    apply_relocation(load, 2, R_PPC_EMB_SDA21, 0x804D6544, 0, 0x80067B10, small_data_base_for);
    CHECK_EQ(word_at(load, 0), uint32_t(0x800DAEA4));
    ReadWord read_word = [&](int64_t) -> std::optional<uint32_t> { return word_at(load, 0); };
    CHECK_EQ(*decode_relocation_target(read_word, 0x80067B12, R_PPC_EMB_SDA21, SMALL_DATA_BASES), int64_t(0x804D6544));
    // lfs f0, literal@sda21(r0), r2-relative float constant
    Bytes float_load = packed_u32_be(0xC0020000);
    apply_relocation(float_load, 2, R_PPC_EMB_SDA21, 0x804DF9E0 - 0x6768, 0, 0, small_data_base_for);
    CHECK_EQ(word_at(float_load, 0), uint32_t(0xC0029898));
}

TEST(reloc_word_masks) {
    // the bits a relocation may change inside its word: what the patch planner ignores
    CHECK_EQ(*word_mask_for_relocation(R_PPC_REL24, 0x10), uint32_t(0x03FFFFFC));
    CHECK_EQ(*word_mask_for_relocation(R_PPC_REL14, 0x10), uint32_t(0x0000FFFC));
    CHECK_EQ(*word_mask_for_relocation(R_PPC_ADDR16_LO, 0x12), uint32_t(0x0000FFFF));
    CHECK_EQ(*word_mask_for_relocation(R_PPC_ADDR16_HA, 0x10), uint32_t(0xFFFF0000));
    CHECK_EQ(*word_mask_for_relocation(R_PPC_EMB_SDA21, 0x12), uint32_t(0x001FFFFF));
    CHECK_EQ(*word_mask_for_relocation(R_PPC_ADDR32, 0x10), uint32_t(0xFFFFFFFF));
    CHECK(!word_mask_for_relocation(99, 0).has_value());
}
