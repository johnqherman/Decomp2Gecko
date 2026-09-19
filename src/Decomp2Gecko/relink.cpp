#include "Decomp2Gecko/relink.h"

#include "Decomp2Gecko/emit.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

CodeSummary summarize(const CodeSet& code_set) {
    return {code_set.lines.size(), code_set.in_dol_bytes(), code_set.relocated_bytes()};
}

SlippiCheck slippi_check(
    const Layout& layout, const std::vector<AddressRange>& write_ranges, const SlippiIniSource& source) {
    SlippiCheck result;
    std::optional<std::filesystem::path> ini;
    switch (source.kind) {
        case SlippiIniSource::Kind::None: return result;
        case SlippiIniSource::Kind::Automatic: ini = find_slippi_ini(layout.decomp().game); break;
        case SlippiIniSource::Kind::Path:
            ini = std::filesystem::path(expanduser(source.path.string()));
            if (!std::filesystem::exists(*ini)) {
                result.requested_ini_missing = true;
                result.ini = ini;
                return result;
            }
            break;
    }

    if (!ini) {
        return result;
    }
    result.ini = ini;
    result.sites = parse_ini(*ini).patch_sites();
    std::tie(result.conflicts, result.notes) = check_conflicts(layout, result.sites, write_ranges);
    return result;
}

} // namespace

GenerateResult generate(const Layout& layout, const GenerateOptions& options) {
    GenerateResult result;
    result.report = layout.report_lines();
    if (!layout.errors().empty()) {
        result.status = GenerateResult::Status::LayoutErrors;
        return result;
    }

    CodeSet code_set = build_code_set(layout, options.name);
    result.reserved_conflicts = reserved_conflicts(layout, code_set);
    if (!result.reserved_conflicts.empty()) {
        result.status = GenerateResult::Status::ReservedConflicts;
        return result;
    }

    MemImage replayed = layout.decomp().dol;
    if (layout.free_used()) {
        replayed.add_region(layout.free_start(), (layout.free_used() + 31) & ~int64_t(31));
    }
    simulate_codes(code_set.lines, replayed);
    for (const Chunk* chunk : layout.chunks()) {
        for (const Extent& extent : layout.chunk_extents(*chunk)) {
            auto [start, end] = extent.range;
            if (replayed.read(start, end - start) != layout.image().read(start, end - start)) {
                if (result.mismatched_chunks == 0) {
                    result.mismatch_example = chunk->label();
                }
                result.mismatched_chunks++;
                break;
            }
        }
    }

    if (result.mismatched_chunks > 0) {
        result.status = GenerateResult::Status::SimulationMismatch;
        return result;
    }

    result.slippi = slippi_check(layout, code_set.ranges(), options.slippi_ini);
    result.codes = summarize(code_set);

    result.report.push_back("");
    int64_t zero_lines = (code_set.bss_zero_bytes + 7) / 8;
    if (zero_lines > 100) {
        result.report.push_back("warning: relocated bss costs " + std::to_string(zero_lines) +
            " lines of zero writes (0x" + hex_upper(code_set.bss_zero_bytes) +
            " bytes); slippi's loader ignores 08 serial writes, so there's no cheaper way");
    }
    result.report.push_back("gecko lines: " + std::to_string(result.codes.gecko_lines) + "  in-DOL bytes: 0x" +
        hex_upper(result.codes.in_dol_bytes) + "  relocated bytes: 0x" + hex_upper(result.codes.relocated_bytes));
    if (result.slippi.ini && !result.slippi.requested_ini_missing) {
        result.report.push_back("slippi check against " + display_path(*result.slippi.ini) + ": " +
            std::to_string(result.slippi.sites.size()) + " sites, " + std::to_string(result.slippi.conflicts.size()) +
            " conflicts, " + std::to_string(result.slippi.notes.size()) + " notes");
        for (const std::string& conflict : result.slippi.conflicts) {
            result.report.push_back("  CONFLICT: " + conflict);
        }
        for (const std::string& note : result.slippi.notes) {
            result.report.push_back("  note: " + note);
        }
    }

    result.ini_text = format_ini(code_set);
    return result;
}

} // namespace Decomp2Gecko
