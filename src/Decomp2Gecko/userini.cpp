#include "Decomp2Gecko/userini.h"

#include "Decomp2Gecko/compat.h"

#include <chrono>
#include <ctime>
#include <stdexcept>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/slippi.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko::userini {

namespace {

// text before the first header is a section w/ an empty name
struct IniSection {
    std::string name;
    std::optional<std::string> header_line;
    std::vector<std::string> lines;
};

struct ParsedIni {
    std::string newline = "\n";
    std::vector<IniSection> sections;
};

bool is_section_header(const std::string& line, std::string* name) {
    std::string trimmed = strip(line);
    if (trimmed.size() < 2 || trimmed.front() != '[' || trimmed.back() != ']') {
        return false;
    }
    *name = trimmed.substr(1, trimmed.size() - 2);
    return true;
}

std::optional<std::string> declared_code_name(const std::string& line) {
    std::string trimmed = strip(line);
    if (trimmed.empty() || trimmed[0] != '$') {
        return std::nullopt;
    }
    return strip_author(std::string_view(trimmed).substr(1));
}

ParsedIni parse_sections(std::string_view text) {
    ParsedIni parsed;
    size_t first_newline = text.find('\n');
    if (first_newline != std::string_view::npos && first_newline > 0 && text[first_newline - 1] == '\r') {
        parsed.newline = "\r\n";
    }
    std::vector<std::string> lines = split_on(text, '\n');
    if (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    for (std::string& line : lines) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
    }
    parsed.sections.push_back({});
    for (std::string& line : lines) {
        std::string name;
        if (is_section_header(line, &name)) {
            parsed.sections.push_back({name, line, {}});
        } else {
            parsed.sections.back().lines.push_back(std::move(line));
        }
    }
    return parsed;
}

std::string render(const ParsedIni& parsed) {
    std::string text;
    for (const IniSection& section : parsed.sections) {
        if (section.header_line) {
            text += *section.header_line + parsed.newline;
        }
        for (const std::string& line : section.lines) {
            text += line + parsed.newline;
        }
    }
    return text;
}

size_t last_non_blank_index(const std::vector<std::string>& lines) {
    size_t count = lines.size();
    while (count > 0 && strip(lines[count - 1]).empty()) {
        count--;
    }
    return count;
}

IniSection* last_section_named(ParsedIni& parsed, const std::string& name) {
    IniSection* found = nullptr;
    for (IniSection& section : parsed.sections) {
        if (section.name == name) {
            found = &section;
        }
    }
    return found;
}

IniSection& append_section(ParsedIni& parsed, const std::string& name) {
    IniSection& previous = parsed.sections.back();
    bool has_content = previous.header_line || !previous.lines.empty();
    if (has_content && !previous.lines.empty() && !strip(previous.lines.back()).empty()) {
        previous.lines.push_back("");
    }
    if (has_content && previous.lines.empty() && previous.header_line) {
        previous.lines.push_back("");
    }
    parsed.sections.push_back({name, "[" + name + "]", {}});
    return parsed.sections.back();
}

bool remove_code_blocks(IniSection& section, const std::string& code_name) {
    // trailing blank lines stay out, so a second merge changes nothing
    size_t content_end = last_non_blank_index(section.lines);
    std::vector<std::string> kept;
    bool removed = false;
    bool skipping = false;
    for (size_t index = 0; index < content_end; index++) {
        const std::string& line = section.lines[index];
        std::optional<std::string> declared = declared_code_name(line);
        if (declared) {
            skipping = *declared == code_name;
            if (skipping) {
                removed = true;
            }
        }
        if (!skipping) {
            kept.push_back(line);
        }
    }
    kept.insert(kept.end(), section.lines.begin() + ptrdiff_t(content_end), section.lines.end());
    section.lines = std::move(kept);
    return removed;
}

InstallPlan merge(ParsedIni& parsed, const GeneratedCode& code) {
    InstallPlan plan;
    for (IniSection& section : parsed.sections) {
        if (section.name == "Gecko" && remove_code_blocks(section, code.name)) {
            plan.replaces_existing = true;
        }
    }

    IniSection* gecko = last_section_named(parsed, "Gecko");
    if (gecko == nullptr) {
        gecko = &append_section(parsed, "Gecko");
    }
    std::vector<std::string> block = {code.header_line};
    block.insert(block.end(), code.lines.begin(), code.lines.end());
    size_t insert_at = last_non_blank_index(gecko->lines);
    gecko->lines.insert(gecko->lines.begin() + ptrdiff_t(insert_at), block.begin(), block.end());

    for (const IniSection& section : parsed.sections) {
        if (section.name != "Gecko_Enabled") {
            continue;
        }
        for (const std::string& line : section.lines) {
            if (declared_code_name(line) == code.name) {
                plan.already_enabled = true;
            }
        }
    }

    if (!plan.already_enabled) {
        IniSection* enabled = last_section_named(parsed, "Gecko_Enabled");
        if (enabled == nullptr) {
            enabled = &append_section(parsed, "Gecko_Enabled");
            plan.creates_enabled_section = true;
        }
        size_t enabled_at = last_non_blank_index(enabled->lines);
        enabled->lines.insert(enabled->lines.begin() + ptrdiff_t(enabled_at), "$" + code.name);
    }

    for (IniSection& section : parsed.sections) {
        if (section.name != "Gecko_Disabled") {
            continue;
        }
        std::vector<std::string> kept;
        for (const std::string& line : section.lines) {
            if (declared_code_name(line) == code.name) {
                plan.removes_from_disabled = true;
            } else {
                kept.push_back(line);
            }
        }
        section.lines = std::move(kept);
    }

    return plan;
}

bool is_code_line(const std::string& line) {
    std::string trimmed = strip(line);
    if (trimmed.size() < 17 || trimmed[8] != ' ') {
        return false;
    }
    for (size_t index = 0; index < 17; index++) {
        if (index == 8) {
            continue;
        }
        if (!is_hex_digit(trimmed[index])) {
            return false;
        }
    }
    return true;
}

std::string local_timestamp() {
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y%m%d-%H%M%S", &local);
    return buffer;
}

} // namespace

GeneratedCode parse_generated_ini(std::string_view text) {
    ParsedIni parsed = parse_sections(text);
    for (const IniSection& section : parsed.sections) {
        if (section.name != "Gecko") {
            continue;
        }
        GeneratedCode code;
        bool in_block = false;
        for (const std::string& line : section.lines) {
            std::optional<std::string> declared = declared_code_name(line);
            if (declared) {
                if (in_block) { // second code: only the first is ours
                    break;
                }
                in_block = true;
                code.name = *declared;
                code.header_line = strip(line);
                continue;
            }
            if (in_block && is_code_line(line)) {
                code.lines.push_back(strip(line));
            }
        }
        if (!in_block) {
            throw InvalidValueError("no $code header under [Gecko]");
        }
        if (code.lines.empty()) {
            throw InvalidValueError("the code \"" + code.name + "\" has no lines; nothing to install");
        }
        return code;
    }
    throw InvalidValueError("no [Gecko] section found");
}

InstallPlan plan_install(std::string_view existing_text, const GeneratedCode& code) {
    ParsedIni parsed = parse_sections(existing_text);
    return merge(parsed, code);
}

std::string merge_code_into_ini(std::string_view existing_text, const GeneratedCode& code) {
    ParsedIni parsed = parse_sections(existing_text);
    merge(parsed, code);
    return render(parsed);
}

InstallResult install_code(
    const std::filesystem::path& target, const GeneratedCode& code, const InstallOptions& options) {
    namespace fs = std::filesystem;
    InstallResult result;
    result.target = target;
    std::string existing;
    result.plan.file_exists = fs::exists(target);
    if (result.plan.file_exists) {
        existing = read_file_bytes(target);
    }

    InstallPlan merged_plan = plan_install(existing, code);
    merged_plan.file_exists = result.plan.file_exists;
    result.plan = merged_plan;
    std::string merged = merge_code_into_ini(existing, code);

    if (result.plan.file_exists) {
        std::string stamp = options.backup_stamp.empty() ? local_timestamp() : options.backup_stamp;
        fs::path backup = target.string() + ".bak-" + stamp;
        for (int attempt = 2; fs::exists(backup); attempt++) {
            backup = target.string() + ".bak-" + stamp + "-" + std::to_string(attempt);
        }
        write_file_bytes(backup, existing);
        result.backup = backup;
    }

    try {
        if (target.has_parent_path()) {
            fs::create_directories(target.parent_path());
        }
        fs::path temporary = target.string() + ".tmp-" + std::to_string(d2g_getpid());
        write_file_bytes(temporary, merged);
        rename_replace(temporary, target);
    } catch (const std::exception& error) {
        std::string prefix = result.backup ? "wrote backup " + result.backup->string() + " but " : "";
        throw std::runtime_error(prefix + "could not write " + target.string() + ": " + error.what());
    }
    return result;
}

} // namespace Decomp2Gecko::userini
