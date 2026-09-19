#include "check.h"
#include "Decomp2Gecko/gameconfig.h"

using namespace Decomp2Gecko;

TEST(gameconfig_invariants) {
    const GameConfig& config = melee_game_config();
    // layout hands out the free region w/o a run-time check
    CHECK(config.reserved_hit(config.free_start, config.free_end()) == nullptr);
    // reserved check is half-open & returns the first hit in declaration order
    CHECK(config.reserved_hit(0x80190ABC, 0x80190AC0) == &config.reserved[1]);
    CHECK(config.reserved_hit(0x80003000, 0x80003004) == nullptr);
    // small-data relocation needs both bases, & the verify pass reads them by these names
    CHECK(config.linker_symbols.find("_SDA_BASE_") != nullptr);
    CHECK(config.linker_symbols.find("_SDA2_BASE_") != nullptr);
}

TEST(gameconfig_read_only_tables) {
    CHECK(glob_matches("ftData_*", "ftData_CharacterStateTables"));
    CHECK(glob_matches("*", ""));
    CHECK(!glob_matches("ftData_*", "ftPp_Init_CostumeStrings"));
    CHECK(glob_matches("melee/ft/*.c", "melee/ft/ftdata.c"));
    CHECK(glob_matches("melee/ft/*.c", "melee/ft/kinds/ftFox/ftfox.c")); // '*' spans slashes
    const GameConfig& melee = melee_game_config();
    CHECK(melee.read_only_table("melee/ft/ftdata.c", "ftData_803C2360") != nullptr);
    CHECK(melee.read_only_table("melee/ft/ftdata.c", "ftData_CharacterStateTables") != nullptr);
    // assigned at runtime (ftdata.c: ftData_Table_Unk0[kind].data = ...), so never vouched for
    CHECK(melee.read_only_table("melee/ft/ftdata.c", "ftData_Table_Unk0") == nullptr);
    CHECK(melee.read_only_table("melee/ft/ftdata.c", "ftData_UnkIntPairs") == nullptr);
    CHECK(melee.read_only_table("melee/ft/ftdata.c", "ftData_UnkBytePerCharacter") == nullptr);
    CHECK(melee.read_only_table("melee/ft/kinds/ftPopo/ftpopo.c", "ftPp_Init_CostumeStrings") == nullptr);
    CHECK(melee.read_only_table("melee/gr/ground.c", "ftData_Fake") == nullptr);
}
