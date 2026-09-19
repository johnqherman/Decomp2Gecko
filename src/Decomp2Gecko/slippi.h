// slippi hooks (C2) & patches (04) vanilla functions: a hook left in code the mod abandoned is
// dead, & a word both write goes to whoever runs last. both are conflicts
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Decomp2Gecko/gameconfig.h"
#include "Decomp2Gecko/layout.h"
#include "Decomp2Gecko/ordered_map.h"

namespace Decomp2Gecko {

struct PatchSite {
    int64_t address;
    int64_t size;
    std::string kind; // "write" or "hook"
    std::string code_name;
};

using GeckoWords = std::vector<std::pair<uint32_t, uint32_t>>; // (high word, low word) per line

struct SlippiIni {
    OrderedMap<std::string, GeckoWords> codes; // code name -> lines, in file order
    std::unordered_set<std::string> enabled;

    std::vector<PatchSite> patch_sites(bool only_enabled = true) const;
};

std::string strip_author(std::string_view code_name);

SlippiIni parse_ini(const std::filesystem::path& path);
SlippiIni parse_ini_text(std::string_view text);

std::vector<PatchSite> patch_sites_of_code(const std::string& code_name, const GeckoWords& words);

std::optional<std::filesystem::path> find_slippi_ini(const GameConfig& game);

std::pair<std::vector<std::string>, std::vector<std::string>> check_conflicts(
    const Layout& layout, const std::vector<PatchSite>& sites, std::vector<AddressRange> write_ranges);

} // namespace Decomp2Gecko
