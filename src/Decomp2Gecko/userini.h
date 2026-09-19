#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Decomp2Gecko::userini {

struct GeneratedCode {
    std::string name; // "My Cool Mod" (author suffix stripped)
    std::string header_line; // "$My Cool Mod [Decomp2Gecko]" verbatim
    std::vector<std::string> lines; // "200679F0 480000CD", ... verbatim
};

// parses format_ini() output; throws InvalidValueError w/o a [Gecko] code or lines (a clean tree's)
GeneratedCode parse_generated_ini(std::string_view text);

struct InstallPlan {
    bool file_exists = false;
    bool replaces_existing = false; // "$Name" already sits under [Gecko]
    bool already_enabled = false; // "$Name" already listed under [Gecko_Enabled]
    bool creates_enabled_section = false; // file has no [Gecko_Enabled] section yet
    bool removes_from_disabled = false; // "$Name" is listed under [Gecko_Disabled]
};

InstallPlan plan_install(std::string_view existing_text, const GeneratedCode& code);

std::string merge_code_into_ini(std::string_view existing_text, const GeneratedCode& code);

struct InstallOptions {
    std::string backup_stamp; // "YYYYMMDD-HHMMSS", empty means "now" (tests pass a fixed one)
};

struct InstallResult {
    std::filesystem::path target;
    std::optional<std::filesystem::path> backup;
    InstallPlan plan;
};

// backs up to "<target>.bak-<stamp>" (-2, -3 on collision), writes via temp file + rename;
// throws std::runtime_error naming the backup if the final write fails
InstallResult install_code(
    const std::filesystem::path& target, const GeneratedCode& code, const InstallOptions& options = {});

} // namespace Decomp2Gecko::userini
