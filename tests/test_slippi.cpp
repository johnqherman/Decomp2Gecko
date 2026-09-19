#include "check.h"
#include "Decomp2Gecko/slippi.h"

using namespace Decomp2Gecko;

TEST(slippi_parse_ini) {
    SlippiIni ini =
        parse_ini_text("# GALE01\n[Core]\nCPUThread = True\n\n[Gecko_Enabled]\n$Required: General Codes\n$Off Code\n\n"
                       "[Gecko]\n$Required: General Codes [Achilles, Dan Salvato]\n0415EE98 38600001 #Unlock All\n"
                       "*description line\n0415EDDC 38600001\n$Not Enabled [x]\nC2001800 00000002\n60000000 00000000\n"
                       "60000000 00000000\n$Off Code\n04000000 00000000\n");
    CHECK_EQ(ini.codes.size(), size_t(3));
    CHECK_EQ(ini.codes.begin()->first, std::string("Required: General Codes"));
    CHECK_EQ(ini.codes.find("Required: General Codes")->size(), size_t(2));
    CHECK(ini.enabled.count("Required: General Codes") == 1);
    CHECK(ini.enabled.count("Off Code") == 1);
    CHECK(ini.enabled.count("Not Enabled") == 0);
    std::vector<PatchSite> sites = ini.patch_sites();
    CHECK_EQ(sites.size(), size_t(3));
    CHECK_EQ(sites[0].address, int64_t(0x8015EE98));
    CHECK_EQ(sites[2].code_name, std::string("Off Code"));
}

TEST(slippi_code_types) {
    GeckoWords words = {
        {0x00001000, 0x00030041}, // 8-bit write repeated 4 times
        {0x02001010, 0x00010042}, // 16-bit write repeated twice
        {0x04001020, 0x38600001}, // 32-bit write
        {0x06001030, 0x0000000C}, // 12-byte string: two data lines follow
        {0x2C000168, 0x41820018},
        {0x2C00016D, 0x00000000},
        {0x08001040, 0x00000000}, // serial: 4 words stepping by 4
        {0x20030004, 0x00000000},
        {0xC0000000, 0x00000001}, // asm in the code list, one line
        {0x60000000, 0x00000000},
        {0xC2001050, 0x00000001}, // hook w/ one asm line
        {0x60000000, 0x00000000},
        {0xC5001060, 0x00000001}, // C4 w/ the high-address bit set
        {0x60000000, 0x00000000},
        {0xF2001080, 0x01234501}, // F2 checksum hook w/ one asm line at 0x80001080
        {0x60000000, 0x00000000},
        {0x04001070, 0x00000000},
    };
    std::vector<PatchSite> sites = patch_sites_of_code("test", words);
    CHECK_EQ(sites.size(), size_t(9));
    CHECK_EQ(sites[0].size, int64_t(4));
    CHECK_EQ(sites[1].size, int64_t(4));
    CHECK_EQ(sites[2].size, int64_t(4));
    CHECK_EQ(sites[3].size, int64_t(12));
    CHECK_EQ(sites[4].size, int64_t(16));
    CHECK_EQ(sites[5].kind, std::string("hook"));
    CHECK_EQ(sites[5].address, int64_t(0x80001050));
    CHECK_EQ(sites[6].address, int64_t(0x81001060));
    CHECK_EQ(sites[7].kind, std::string("hook"));
    CHECK_EQ(sites[7].address, int64_t(0x80001080));
    CHECK_EQ(sites[8].address, int64_t(0x80001070));
}

TEST(strip_author_brackets) {
    CHECK_EQ(strip_author("Foo [author]"), std::string("Foo"));
    CHECK_EQ(strip_author("Foo [Bar] [author]"), std::string("Foo [Bar]"));
    CHECK_EQ(strip_author("Foo"), std::string("Foo"));
    CHECK_EQ(strip_author("Foo [Bar]"), std::string("Foo"));
    CHECK_EQ(strip_author("  Foo [author]  "), std::string("Foo"));
    CHECK_EQ(strip_author("Foo [a] [b] [c]"), std::string("Foo [a] [b]"));
}
