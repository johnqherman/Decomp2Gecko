#include "check.h"
#include "Decomp2Gecko/vanilla.h"

using namespace Decomp2Gecko;

TEST(vanilla_symbols) {
    Symbols symbols =
        Symbols::from_text("ftPartsTable = .sbss:0x804D6544; // type:object size:0x4 scope:global data:4byte\n"
                           "doColl = .text:0x800EBE64; // type:function size:0x60 scope:local\n"
                           "doColl = .text:0x8011E7D4; // type:function size:0xEC scope:local\n"
                           "@219 = extab:0x80005520; // type:object size:0x8 scope:local hidden\n"
                           "cyan$178 = .sdata2:0x804DDC88; // type:object size:0x4 scope:local data:4byte\n");
    CHECK_EQ(symbols.by_name("ftPartsTable")[0]->address, int64_t(0x804D6544));
    std::vector<const VanillaSymbol*> in_range = symbols.named_in_range("doColl", 0x80100000, 0x80200000);
    CHECK_EQ(in_range.size(), size_t(1));
    CHECK_EQ(in_range[0]->address, int64_t(0x8011E7D4));
    CHECK_EQ(symbols.starting_at(0x80005520)[0]->section, std::string("extab"));
    // $NNN suffix drifts between builds, so name match ignores it
    CHECK_EQ(symbols.named_in_section("cyan$177", ".sdata2")[0]->name, std::string("cyan$178"));
    CHECK_EQ(base_name("to_synth_mode$881"), std::string("to_synth_mode"));
    CHECK_EQ(base_name("plain$"), std::string("plain$"));
    CHECK_EQ(symbols.by_name("@219")[0]->size, int64_t(8));
    CHECK_EQ(symbols.entries().size(), size_t(5));
    CHECK_EQ(symbols.entries()[0]->name, std::string("@219"));
}
