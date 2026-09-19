#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Decomp2Gecko/ordered_map.h"

namespace Decomp2Gecko {

struct ReservedRange {
    int64_t start;
    int64_t end;
    std::string reason;

    bool overlaps(int64_t range_start, int64_t range_end) const { return range_start < end && range_end > start; }
};

// a pointer table the game only reads through, never writes through or compares.
// what it points at (transitively) may share the same bytes elsewhere
struct ReadOnlyTable {
    std::string unit; // glob
    std::string symbol; // glob
    std::string reason;
};

// '*' only
bool glob_matches(std::string_view pattern, std::string_view text);

struct GameConfig {
    std::string id; // "GALE01": names the orig/, config/ & build/ directories
    int64_t free_start = 0;
    int64_t free_size = 0;
    std::vector<ReservedRange> reserved; // never written to, never handed out as free space
    OrderedMap<std::string, int64_t> linker_symbols; // symbols the objects use but symbols.txt lacks
    std::vector<std::string> small_data_sections; // addressed through r13
    std::vector<std::string> small_data2_sections; // addressed through r2
    std::vector<std::string> slippi_ini_paths; // slippi launcher's shipped GALE01r2.ini, per OS
    std::string slippi_user_ini; // dolphin's per-game user ini, install appends to it
    std::vector<ReadOnlyTable> read_only_tables;

    int64_t free_end() const { return free_start + free_size; }
    const ReservedRange* reserved_hit(int64_t start, int64_t end) const;
    const ReadOnlyTable* read_only_table(std::string_view unit, std::string_view symbol) const;
};

const GameConfig& melee_game_config();

} // namespace Decomp2Gecko
