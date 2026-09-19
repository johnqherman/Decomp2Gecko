#include "Decomp2Gecko/gameconfig.h"

namespace Decomp2Gecko {

const ReservedRange* GameConfig::reserved_hit(int64_t start, int64_t end) const {
    for (const ReservedRange& reserved_range : reserved) {
        if (reserved_range.overlaps(start, end)) {
            return &reserved_range;
        }
    }
    return nullptr;
}

bool glob_matches(std::string_view pattern, std::string_view text) {
    if (pattern.empty()) {
        return text.empty();
    }
    if (pattern[0] == '*') {
        for (size_t length = 0; length <= text.size(); length++) {
            if (glob_matches(pattern.substr(1), text.substr(length))) {
                return true;
            }
        }
        return false;
    }
    return !text.empty() && pattern[0] == text[0] && glob_matches(pattern.substr(1), text.substr(1));
}

const ReadOnlyTable* GameConfig::read_only_table(std::string_view unit, std::string_view symbol) const {
    for (const ReadOnlyTable& table : read_only_tables) {
        if (glob_matches(table.unit, unit) && glob_matches(table.symbol, symbol)) {
            return &table;
        }
    }
    return nullptr;
}

const GameConfig& melee_game_config() {
    static const GameConfig config = [] {
        GameConfig melee;
        melee.id = "GALE01";

        // above __ArenaHi: vanilla melee never allocates there (metrotrk debugger stub), nor do slippi's codes
        melee.free_start = 0x81700000;
        melee.free_size = 0x00100000;

        melee.reserved = {
            {0x80001800, 0x80003000, "Dolphin/Slippi Gecko codehandler and code list"},
            {0x80190ABC, 0x801A0E34,
                "tournament-mode code (gmtou_0/1/2.c, gm_19EF.c); Slippi Dolphin hosts its ~7k-line Gecko code list here"},
        };

        // r13/r2/r1 as the vanilla DOL's __init_registers loads them, checked against a clean build's main.elf.
        // verify reads the modded main.elf's own values instead, bc a modified link shifts small-data sections
        melee.linker_symbols["_SDA_BASE_"] = 0x804DB6A0;
        melee.linker_symbols["_SDA2_BASE_"] = 0x804DF9E0;
        melee.linker_symbols["_stack_addr"] = 0x804EEC00;
        melee.linker_symbols["_stack_end"] = 0x804DEC00;
        melee.linker_symbols["_db_stack_addr"] = 0x804F0C00;
        melee.linker_symbols["_db_stack_end"] = 0x804EEC00;
        melee.linker_symbols["__ArenaLo"] = 0x804F0C00;
        melee.linker_symbols["__ArenaHi"] = 0x81700000;

        // sections addressed through r13/r2 (EMB_SDA21), growth here can't be relocated
        melee.small_data_sections = {".sdata", ".sbss"};
        melee.small_data2_sections = {".sdata2", ".sbss2"};

        // audited against every use in the decomp.
        // left out: ftData_Table_Unk0 & ftData_UnkIntPairs (.data assigned at runtime)
        // ftData_UnkBytePerCharacter & ftData_UnkIntBoolFunc0 (not pointer tables)
        for (const char* table : {"ftData_Table_Unk1", "ftData_OnLoad", "ftData_OnDeath", "ftData_OnUserDataRemove",
                 "ftData_CharacterStateTables", "ftData_UnkMotionStates0", "ftData_SpecialS", "ftData_SpecialAirHi",
                 "ftData_SpecialAirLw", "ftData_SpecialAirS", "ftData_SpecialAirN", "ftData_SpecialN",
                 "ftData_SpecialLw", "ftData_SpecialHi", "ftData_OnAbsorb", "ftData_OnItemPickupExt",
                 "ftData_OnItemInvisible", "ftData_OnItemVisible", "ftData_OnItemDropExt", "ftData_OnItemPickup",
                 "ftData_OnItemDrop", "ftData_UnkMotionStates1", "ftData_UnkMotionStates2", "ftData_OnKnockbackEnter",
                 "ftData_OnKnockbackExit", "ftData_UnkMotionStates3", "ftData_UnkMotionStates4", "ftData_803C1F40",
                 "ftData_UnkMotionStates5", "ftData_UnkMtxFunc0", "ftData_UnkCallbackPairs0", "ftData_803C2360",
                 "ftData_803C23E4", "ftData_803C2468", "ftData_803C24EC", "ftData_UnkDemoCallbacks0"}) {
            melee.read_only_tables.push_back({"melee/ft/ftdata.c", table,
                "per-character dispatch or resource table in ftdata.c, audited: only ever read through"});
        }

        // linux, macos, windows
        melee.slippi_ini_paths = {
            "~/.config/Slippi Launcher/netplay/Sys/GameSettings/GALE01r2.ini",
            "~/Library/Application Support/Slippi Launcher/netplay/Slippi Dolphin.app/Contents/Resources/Sys/GameSettings/GALE01r2.ini",
            "~/AppData/Roaming/Slippi Launcher/netplay/Sys/GameSettings/GALE01r2.ini",
        };
        
#if defined(_WIN32)
        melee.slippi_user_ini = "~/AppData/Roaming/SlippiOnline/GameSettings/GALE01.ini";
#elif defined(__APPLE__)
        melee.slippi_user_ini = "~/Library/Application Support/SlippiOnline/GameSettings/GALE01.ini";
#else
        melee.slippi_user_ini = "~/.config/SlippiOnline/GameSettings/GALE01.ini";
#endif
        return melee;
    }();
    return config;
}

} // namespace Decomp2Gecko
