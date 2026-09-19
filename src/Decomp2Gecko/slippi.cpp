#include "Decomp2Gecko/slippi.h"

#include <algorithm>

#include "Decomp2Gecko/emit.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

// two 8-digit hex words separated by whitespace at line start, anything may follow
bool parse_code_line(std::string_view line, uint32_t* first, uint32_t* second) {
    if (line.size() < 17) {
        return false;
    }
    for (size_t index = 0; index < 8; index++) {
        if (!is_hex_digit(line[index])) {
            return false;
        }
    }
    size_t position = 8;
    while (position < line.size() && is_ascii_space(line[position])) {
        position++;
    }
    if (position == 8 || position + 8 > line.size()) {
        return false;
    }
    for (size_t index = 0; index < 8; index++) {
        if (!is_hex_digit(line[position + index])) {
            return false;
        }
    }
    *first = uint32_t(parse_hex(line.substr(0, 8)));
    *second = uint32_t(parse_hex(line.substr(position, 8)));
    return true;
}

} // namespace

std::vector<PatchSite> SlippiIni::patch_sites(bool only_enabled) const {
    std::vector<PatchSite> sites;
    for (const auto& [code_name, words] : codes) {
        if (only_enabled && enabled.count(code_name) == 0) {
            continue;
        }
        std::vector<PatchSite> code_sites = patch_sites_of_code(code_name, words);
        sites.insert(sites.end(), code_sites.begin(), code_sites.end());
    }
    return sites;
}

std::string strip_author(std::string_view code_name) {
    // rfind: brackets inside the name survive
    std::string trimmed = rstrip(code_name);
    size_t last_bracket = trimmed.rfind('[');
    if (last_bracket != std::string::npos && !trimmed.empty() && trimmed.back() == ']' &&
        trimmed.size() - 1 > last_bracket) {
        size_t cut = last_bracket;
        while (cut > 0 && is_ascii_space(code_name[cut - 1])) {
            cut--;
        }
        return strip(code_name.substr(0, cut));
    }
    return strip(code_name);
}

SlippiIni parse_ini_text(std::string_view text) {
    SlippiIni ini;
    std::optional<std::string> section;
    std::optional<std::string> current_code;

    for (const std::string& raw_line : split_lines(text)) {
        // code names start w/ '$' & can contain '#', strip comments elsewhere
        std::string line = starts_with(raw_line, "$") ? strip(raw_line) : strip(raw_line.substr(0, raw_line.find('#')));
        if (line.empty()) {
            continue;
        }
        if (starts_with(line, "[") && ends_with(line, "]")) {
            section = line.substr(1, line.size() - 2);
            current_code = std::nullopt;
            continue;
        }

        if (section == "Gecko_Enabled" && starts_with(line, "$")) {
            ini.enabled.insert(strip_author(std::string_view(line).substr(1)));
        } else if (section == "Gecko") {
            if (starts_with(line, "$")) {
                current_code = strip_author(std::string_view(line).substr(1));
                ini.codes.setdefault(*current_code, {});
                continue;
            }
            uint32_t first, second;
            if (parse_code_line(line, &first, &second) && current_code) {
                ini.codes[*current_code].emplace_back(first, second);
            }
        }
    }

    return ini;
}

SlippiIni parse_ini(const std::filesystem::path& path) {
    return parse_ini_text(read_file_bytes(expanduser(path.string())));
}

std::vector<PatchSite> patch_sites_of_code(const std::string& code_name, const GeckoWords& words) {
    std::vector<PatchSite> sites;
    size_t position = 0;
    while (position < words.size()) {
        auto [first, second] = words[position];
        int64_t code_type = (first >> 24) & 0xFE; // low bit is part of the address
        int64_t address = GECKO_BASE_ADDRESS + (first & 0x01FFFFFF);

        if (code_type == 0x00) { // 8-bit write & fill
            sites.push_back({address, ((second >> 16) & 0xFFFF) + 1, "write", code_name});
        } else if (code_type == 0x02) { // 16-bit write & fill
            sites.push_back({address, 2 * (((second >> 16) & 0xFFFF) + 1), "write", code_name});
        } else if (code_type == 0x04) { // 32-bit write
            sites.push_back({address, 4, "write", code_name});
        } else if (code_type == 0x06) { // string write
            sites.push_back({address, int64_t(second), "write", code_name});
            position += (size_t(second) + 7) / 8;
        } else if (code_type == 0x08) { // serial write
            uint32_t control = position + 1 < words.size() ? words[position + 1].first : 0;
            int64_t count = ((control >> 16) & 0xFFF) + 1;
            int64_t address_step = control & 0xFFFF;
            sites.push_back({address, std::max(int64_t(4), (count - 1) * address_step + 4), "write", code_name});
            position += 1;
        } else if (code_type == 0xC0) { // asm executed from the code list, no hook
            position += second;
        } else if (code_type == 0xC2) { // asm inserted at an address
            sites.push_back({address, 4, "hook", code_name});
            position += second;
        } else if (code_type == 0xC4) {
            sites.push_back({address, 4, "hook", code_name});
            position += second & 0xFFFF;
        } else if (code_type == 0xF2 || code_type == 0xF4) {
            sites.push_back({address, 4, "hook", code_name});
            position += second & 0xFF;
        }

        position += 1;
    }

    return sites;
}

std::optional<std::filesystem::path> find_slippi_ini(const GameConfig& game) {
    for (const std::string& candidate : game.slippi_ini_paths) {
        std::filesystem::path path = expanduser(candidate);
        if (std::filesystem::exists(path)) {
            return path;
        }
    }
    return std::nullopt;
}

namespace {

std::optional<std::string> unit_containing(const Layout& layout, int64_t address) {
    for (const auto& [unit_name, sections] : layout.decomp().splits.units) {
        for (const auto& [section_name, range] : sections) {
            if (range.first <= address && address < range.second) {
                return unit_name;
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::pair<std::vector<std::string>, std::vector<std::string>> check_conflicts(
    const Layout& layout, const std::vector<PatchSite>& sites, std::vector<AddressRange> write_ranges) {
    std::vector<std::string> conflicts;
    std::vector<std::string> notes;

    struct ChunkRange {
        int64_t start;
        int64_t end;
        const Chunk* chunk;
    };
    std::vector<ChunkRange> relocated_vanilla_ranges;
    for (const Chunk* chunk : layout.chunks()) {
        if (chunk->kept && chunk->action == ChunkAction::Relocated && chunk->vanilla_address) {
            int64_t extent = chunk->vanilla_size && *chunk->vanilla_size ? *chunk->vanilla_size : chunk->size;
            relocated_vanilla_ranges.push_back({*chunk->vanilla_address, *chunk->vanilla_address + extent, chunk});
        }
    }

    // only dead slots. rewritten ones show up as overlapping writes, kept ones still hold what the stub copies
    std::vector<ChunkRange> dead_ranges;
    for (const Chunk* chunk : layout.chunks()) {
        if (const PatchPlan* plan = layout.patch_plan(*chunk)) {
            for (const auto& [start, end] : plan->dead_vanilla) {
                dead_ranges.push_back({start, end, chunk});
            }
        }
    }

    // an alias writes nothing, but what it points at should stay what vanilla has there
    std::vector<ChunkRange> alias_ranges;
    for (const Chunk* chunk : layout.chunks()) {
        if (chunk->kept && chunk->action == ChunkAction::Alias && chunk->placed_address) {
            alias_ranges.push_back({*chunk->placed_address, *chunk->placed_address + chunk->size, chunk});
        }
    }

    std::unordered_set<std::string> changed_units;
    for (const Chunk* chunk : layout.chunks()) {
        if (chunk->kept && chunk->changed) {
            changed_units.insert(chunk->unit->name);
        }
    }
    std::sort(write_ranges.begin(), write_ranges.end());

    for (const PatchSite& site : sites) {
        int64_t site_end = site.address + site.size;
        for (const auto& [write_start, write_end] : write_ranges) {
            if (site.address < write_end && site_end > write_start) {
                conflicts.push_back(site.code_name + ": " + site.kind + " at " + hex_upper(site.address, 8) +
                    " overlaps a mod write (" + hex_upper(write_start, 8) + "-" + hex_upper(write_end, 8) + ")");
                break;
            }
        }

        for (const ChunkRange& range : relocated_vanilla_ranges) {
            if (site.address < range.end && site_end > range.start) {
                conflicts.push_back(site.code_name + ": " + site.kind + " at " + hex_upper(site.address, 8) +
                    " is inside " + range.chunk->name() + ", which the mod relocates (the Slippi " + site.kind +
                    " would land in dead code)");
                break;
            }
        }

        for (const ChunkRange& range : dead_ranges) {
            if (site.address < range.end && site_end > range.start) {
                conflicts.push_back(site.code_name + ": " + site.kind + " at " + hex_upper(site.address, 8) +
                    " is inside " + range.chunk->name() + ", which the mod patches around it (the Slippi " + site.kind +
                    " would land in dead code)");
                break;
            }
        }

        for (const ChunkRange& range : alias_ranges) {
            if (site.address < range.end && site_end > range.start) {
                conflicts.push_back(site.code_name + ": " + site.kind + " at " + hex_upper(site.address, 8) +
                    " lands on vanilla bytes " + range.chunk->name() + " now shares (the mod aliases onto them)");
                break;
            }
        }

        std::optional<std::string> unit = unit_containing(layout, site.address);
        if (unit && changed_units.count(*unit)) {
            notes.push_back(site.code_name + ": " + site.kind + " at " + hex_upper(site.address, 8) + " is in " +
                *unit + ", a unit the mod changes");
        }
    }

    return {std::move(conflicts), std::move(notes)};
}

} // namespace Decomp2Gecko
