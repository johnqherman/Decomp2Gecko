#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Decomp2Gecko/elf.h"
#include "Decomp2Gecko/gameconfig.h"
#include "Decomp2Gecko/vanilla.h"

namespace Decomp2Gecko {

struct Unit {
    std::string name; // dtk unit name, like "melee/ft/fighter.c"
    std::string stem; // "melee/ft/fighter"
    std::filesystem::path object_path; // build/<game>/src/<stem>.o
    bool in_vanilla = false;
};

class Decomp {
public:
    explicit Decomp(const std::filesystem::path& root);

    const std::filesystem::path root;
    const GameConfig& game;
    const std::filesystem::path build_dir;
    const std::filesystem::path dol_path;
    const std::filesystem::path symbols_path;
    const std::filesystem::path splits_path;
    const std::filesystem::path config_json;
    const std::filesystem::path main_elf;

    const MemImage dol;
    const Symbols symbols;
    const Splits splits;
    std::vector<Unit> units;
    std::vector<std::string> skipped_units;

    ElfFile load_object(const Unit& unit) const;

    // r13 & r2 as the vanilla DOL sets them. small data never moves, so they hold for every chunk
    int64_t sda_base() const;
    int64_t sda2_base() const;
    // 13 for r13-relative small data, 2 for r2-relative, nullopt if not small data
    std::optional<int> small_data_register(int64_t address) const;

private:
    static std::filesystem::path required(const std::filesystem::path& path);
    struct UnitDiscovery {
        std::vector<Unit> units;
        std::vector<std::string> skipped;
    };
    UnitDiscovery discover_units() const;
};

} // namespace Decomp2Gecko
