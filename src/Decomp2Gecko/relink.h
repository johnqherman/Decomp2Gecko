#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "Decomp2Gecko/layout.h"
#include "Decomp2Gecko/slippi.h"

namespace Decomp2Gecko {

struct CodeSummary {
    size_t gecko_lines = 0;
    int64_t in_dol_bytes = 0;
    int64_t relocated_bytes = 0;
};

struct SlippiIniSource {
    enum class Kind { Automatic, None, Path };
    Kind kind = Kind::Automatic;
    std::filesystem::path path;

    static SlippiIniSource automatic() { return {}; }
    static SlippiIniSource none() { return {Kind::None, {}}; }
    static SlippiIniSource at(std::filesystem::path ini_path) { return {Kind::Path, std::move(ini_path)}; }
};

struct SlippiCheck {
    std::optional<std::filesystem::path> ini;
    bool requested_ini_missing = false;
    std::vector<PatchSite> sites;
    std::vector<std::string> conflicts;
    std::vector<std::string> notes;
};

struct GenerateOptions {
    std::string name;
    SlippiIniSource slippi_ini;
};

struct GenerateResult {
    enum class Status {
        Ok,
        LayoutErrors, // layout left chunks w/o an address, see the report
        ReservedConflicts, // codes would write into a reserved range
        SimulationMismatch, // replaying the codes doesn't reproduce the planned image (bug)
    };
    Status status = Status::Ok;
    std::vector<std::string> report;
    std::string ini_text;
    CodeSummary codes;
    std::vector<std::string> reserved_conflicts;
    size_t mismatched_chunks = 0;
    std::string mismatch_example;
    SlippiCheck slippi;
};

GenerateResult generate(const Layout& layout, const GenerateOptions& options);

} // namespace Decomp2Gecko
