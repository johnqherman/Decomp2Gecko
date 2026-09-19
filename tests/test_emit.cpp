#include "check.h"
#include "Decomp2Gecko/emit.h"
#include "Decomp2Gecko/gecko_cost.h"
#include "Decomp2Gecko/vanilla.h"

using namespace Decomp2Gecko;

TEST(emit_zero_fill_is_a_string_write) {
    // relocated bss is zeroed w/ 06 writes because Slippi's loader ignores 08 serial writes
    std::vector<MemoryWrite> writes = {{0x81700000, Bytes(0x14, '\0')}};
    std::vector<std::string> lines = encode_gecko_lines(writes, std::nullopt);
    CHECK_EQ(lines.size(), size_t(4)); // header + 3 lines of zeros (0x14 rounds up to 0x18)
    CHECK_EQ(lines[0], std::string("07700000 00000014"));
    MemImage image;
    image.add_region(0x81700000, 0x20, Bytes(0x20, '\xAA'));
    simulate_codes(lines, image);
    CHECK_EQ(image.read(0x81700000, 0x14), Bytes(0x14, '\0'));
    CHECK_EQ(image.read(0x81700014, 0xC), Bytes(0xC, '\xAA')); // padding is never written
}

TEST(emit_segmentation_picks_cheapest) {
    // two lone words 12 bytes apart: two 04s (2 lines), not one 06 of 0x10 (3 lines)
    std::vector<AddressRange> apart = {{0x80003100, 0x80003104}, {0x8000310C, 0x80003110}};
    CHECK_EQ(estimate_lines(apart), int64_t(2));
    CHECK_EQ(segment_writes(apart).size(), size_t(2));

    // two 3-word runs 4 bytes apart: one 06 of 0x1C = 5 lines vs 3 + 3 separately
    std::vector<AddressRange> close = {{0x80003100, 0x8000310C}, {0x80003110, 0x8000311C}};
    CHECK_EQ(estimate_lines(close), int64_t(5));
    CHECK_EQ(segment_writes(close).size(), size_t(1));

    // a gap of 16 never pays: 06 of 0x30 = 7 lines vs (1 + 2) + (1 + 2) = 6
    std::vector<AddressRange> far = {{0x80003100, 0x80003110}, {0x80003120, 0x80003130}};
    CHECK_EQ(estimate_lines(far), int64_t(6));

    // adjacent ranges are one run: 8 words = one 06 of 5 lines, never eight 04s
    std::vector<AddressRange> adjacent = {{0x80003100, 0x80003110}, {0x80003110, 0x80003120}};
    CHECK_EQ(estimate_lines(adjacent), int64_t(5));

    // a hole in the image splits a write even when the gap is small
    SpanCheck no_cross = [](int64_t start, int64_t end) { return !(start < 0x80003108 && end > 0x80003108); };
    std::vector<AddressRange> across = {{0x80003100, 0x80003104}, {0x8000310C, 0x80003114}};
    CHECK_EQ(segment_writes(across, no_cross).size(), size_t(2));
}

TEST(emit_word_write_lines) {
    CHECK_EQ(write_lines(0x80003100, 0x80003104), int64_t(1));
    CHECK_EQ(write_lines(0x80003102, 0x80003106), int64_t(2)); // unaligned word needs a 06
    CHECK_EQ(write_lines(0x80003100, 0x80003108), int64_t(2));
    CHECK_EQ(write_lines(0x80003100, 0x8000310C), int64_t(3));
    CHECK_EQ(write_lines(0x80003100, 0x80003111), int64_t(4));
}

TEST(simulate_serial_write) {
    // foreign lists (slippi's) may use 08: value, count & steps for both address & value
    MemImage image;
    image.add_region(0x80003100, 0x20, Bytes(0x20, '\xAA'));
    simulate_codes({"08003100 00000005", "20020008 00000001"}, image); // 3 words, 8 bytes apart, values 5 6 7
    CHECK_EQ(image.word(0x80003100), uint32_t(5));
    CHECK_EQ(image.word(0x80003104), uint32_t(0xAAAAAAAA));
    CHECK_EQ(image.word(0x80003108), uint32_t(6));
    CHECK_EQ(image.word(0x80003110), uint32_t(7));
    CHECK_EQ(image.word(0x80003118), uint32_t(0xAAAAAAAA));
}
