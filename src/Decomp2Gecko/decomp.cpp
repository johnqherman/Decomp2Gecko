#include "Decomp2Gecko/decomp.h"

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/json_mini.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace fs = std::filesystem;

namespace {

fs::path resolve_path(const fs::path& path) {
    std::error_code error;
    fs::path resolved = fs::weakly_canonical(fs::absolute(path), error);
    if (error) {
        return fs::absolute(path).lexically_normal();
    }
    return resolved;
}

std::string before_last_dot(const std::string& name) {
    size_t dot = name.rfind('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

} // namespace

fs::path Decomp::required(const fs::path& path) {
    if (!fs::exists(path)) {
        throw MissingFileError("missing " + display_path(path));
    }
    return path;
}

Decomp::Decomp(const fs::path& root_path)
    : root(resolve_path(expanduser(root_path.string()))),
      game(melee_game_config()),
      build_dir(root / "build" / game.id),
      dol_path(root / "orig" / game.id / "sys" / "main.dol"),
      symbols_path(root / "config" / game.id / "symbols.txt"),
      splits_path(root / "config" / game.id / "splits.txt"),
      config_json(build_dir / "config.json"),
      main_elf(build_dir / "main.elf"),
      dol(read_dol(required(dol_path))),
      symbols(required(symbols_path)),
      splits(required(splits_path)) {
    auto discovery = discover_units();
    units = std::move(discovery.units);
    skipped_units = std::move(discovery.skipped);
}

// config.json lists units in link order; the first definition of a duplicate wins, like the real linker
Decomp::UnitDiscovery Decomp::discover_units() const {
    if (!fs::exists(config_json)) {
        throw NotBuiltError("missing " + display_path(config_json) + "; configure and build the decomp first");
    }
    JsonValue config = parse_json(read_file_bytes(config_json));
    UnitDiscovery result;
    if (const JsonValue* configured_units = config.get("units")) {
        for (const JsonValue& unit : configured_units->items) {
            const JsonValue* name = unit.get("name");
            if (!name || !name->is_string()) {
                throw InvalidValueError("config.json lists a unit without a name");
            }
            std::string stem = before_last_dot(name->text);
            fs::path object_path = build_dir / "src" / (stem + ".o");
            if (!fs::exists(object_path)) {
                result.skipped.push_back(name->text);
                continue;
            }
            result.units.push_back({name->text, stem, object_path, splits.units.contains(name->text)});
        }
    }
    if (result.units.empty()) {
        throw NotBuiltError("no compiled objects found under " + display_path(build_dir) + "; build the decomp first");
    }
    return result;
}

ElfFile Decomp::load_object(const Unit& unit) const { return read_elf(unit.object_path); }

int64_t Decomp::sda_base() const {
    const int64_t* value = game.linker_symbols.find("_SDA_BASE_");
    if (value == nullptr) {
        throw InternalError("_SDA_BASE_ is unknown");
    }
    return *value;
}

int64_t Decomp::sda2_base() const {
    const int64_t* value = game.linker_symbols.find("_SDA2_BASE_");
    if (value == nullptr) {
        throw InternalError("_SDA2_BASE_ is unknown");
    }
    return *value;
}

std::optional<int> Decomp::small_data_register(int64_t address) const {
    for (const std::string& section_name : game.small_data_sections) {
        auto extent = splits.section_ranges.find(section_name);
        if (extent != splits.section_ranges.end() && extent->second.first <= address &&
            address < extent->second.second) {
            return 13;
        }
    }
    for (const std::string& section_name : game.small_data2_sections) {
        auto extent = splits.section_ranges.find(section_name);
        if (extent != splits.section_ranges.end() && extent->second.first <= address &&
            address < extent->second.second) {
            return 2;
        }
    }
    return std::nullopt;
}

} // namespace Decomp2Gecko
