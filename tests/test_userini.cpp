#include "Decomp2Gecko/compat.h"

#include "check.h"

#include <filesystem>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/userini.h"
#include "Decomp2Gecko/util.h"

using namespace Decomp2Gecko;
using namespace Decomp2Gecko::userini;
namespace fs = std::filesystem;

namespace {

GeneratedCode sample_code() {
    return {"My Cool Mod", "$My Cool Mod [Decomp2Gecko]", {"200679F0 480000CD", "040679F0 49698611"}};
}

// shape of a real user ini: [Gecko], [Gecko_Disabled], no [Gecko_Enabled], no blank lines
const char* kUserShapedIni = "[Gecko]\n"
                             "$Boot to CSS [Dan Salvato, Achilles]\n"
                             "041BA510 38600002\n"
                             "$Skip Memcard Prompt [UnclePunch]\n"
                             "04240B44 60000000\n"
                             "[Gecko_Disabled]\n"
                             "$Required: General Codes\n"
                             "$Required: Slippi Recording\n";

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("Decomp2Gecko-test-" + std::to_string(d2g_getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    static inline int counter = 0;
};

} // namespace

TEST(userini_keeps_other_codes) {
    std::string merged = merge_code_into_ini(kUserShapedIni, sample_code());
    CHECK_EQ(merged,
        std::string("[Gecko]\n"
                    "$Boot to CSS [Dan Salvato, Achilles]\n"
                    "041BA510 38600002\n"
                    "$Skip Memcard Prompt [UnclePunch]\n"
                    "04240B44 60000000\n"
                    "$My Cool Mod [Decomp2Gecko]\n"
                    "200679F0 480000CD\n"
                    "040679F0 49698611\n"
                    "[Gecko_Disabled]\n"
                    "$Required: General Codes\n"
                    "$Required: Slippi Recording\n"
                    "\n"
                    "[Gecko_Enabled]\n"
                    "$My Cool Mod\n"));

    InstallPlan plan = plan_install(kUserShapedIni, sample_code());
    CHECK(plan.creates_enabled_section);
    CHECK(!plan.replaces_existing);
    CHECK(!plan.already_enabled);
    CHECK(!plan.removes_from_disabled);
}

TEST(userini_reinstall) {
    const char* existing = "[Gecko]\n"
                           "$My Cool Mod [Decomp2Gecko]\n"
                           "*old description\n"
                           "04000000 00000001\n"
                           "#comment\n"
                           "04000000 00000002\n"
                           "$Other [someone]\n"
                           "04000000 00000003\n"
                           "\n"
                           "[Gecko_Enabled]\n"
                           "$Other\n"
                           "\n"
                           "[Gecko_Disabled]\n"
                           "$My Cool Mod\n";

    std::string merged = merge_code_into_ini(existing, sample_code());
    CHECK_EQ(merged,
        std::string("[Gecko]\n"
                    "$Other [someone]\n"
                    "04000000 00000003\n"
                    "$My Cool Mod [Decomp2Gecko]\n"
                    "200679F0 480000CD\n"
                    "040679F0 49698611\n"
                    "\n"
                    "[Gecko_Enabled]\n"
                    "$Other\n"
                    "$My Cool Mod\n"
                    "\n"
                    "[Gecko_Disabled]\n"));

    InstallPlan plan = plan_install(existing, sample_code());
    CHECK(plan.replaces_existing);
    CHECK(plan.removes_from_disabled);
    CHECK(!plan.creates_enabled_section);

    CHECK_EQ(merge_code_into_ini(merged, sample_code()), merged);
    CHECK(plan_install(merged, sample_code()).already_enabled);
}

TEST(userini_crlf_no_author) {
    std::string merged =
        merge_code_into_ini("[Gecko]\r\n$My Cool Mod [someone else]\r\n04000000 00000009\r\n", sample_code());
    CHECK_EQ(merged,
        std::string("[Gecko]\r\n$My Cool Mod [Decomp2Gecko]\r\n200679F0 480000CD\r\n040679F0 49698611\r\n"
                    "\r\n[Gecko_Enabled]\r\n$My Cool Mod\r\n"));
    // file w/o a trailing newline gets one
    CHECK_EQ(merge_code_into_ini("[Gecko_Enabled]\n$My Cool Mod", sample_code()),
        std::string(
            "[Gecko_Enabled]\n$My Cool Mod\n\n[Gecko]\n$My Cool Mod [Decomp2Gecko]\n200679F0 480000CD\n040679F0 49698611\n"));
}

TEST(userini_parse) {
    GeneratedCode code = parse_generated_ini(
        "[Gecko]\n$My Cool Mod [Decomp2Gecko]\n200679F0 480000CD\n040679F0 49698611\n\n[Gecko_Enabled]\n$My Cool Mod\n");
    CHECK_EQ(code.name, std::string("My Cool Mod"));
    CHECK_EQ(code.header_line, std::string("$My Cool Mod [Decomp2Gecko]"));
    CHECK_EQ(code.lines.size(), size_t(2));
    CHECK_THROWS(parse_generated_ini("[Gecko_Enabled]\n$x\n"), InvalidValueError);
    CHECK_THROWS(parse_generated_ini("[Gecko]\n$Clean [Decomp2Gecko]\n\n[Gecko_Enabled]\n$Clean\n"), InvalidValueError);
}

TEST(userini_backup) {
    TempDir scratch;
    fs::path target = scratch.path / "GALE01.ini";
    write_file_bytes(target, kUserShapedIni);
    InstallResult first = install_code(target, sample_code(), {"20260101-000000"});
    CHECK(first.backup.has_value());
    CHECK_EQ(first.backup->string(), (scratch.path / "GALE01.ini.bak-20260101-000000").string());
    CHECK_EQ(read_file_bytes(*first.backup), std::string(kUserShapedIni));
    CHECK_EQ(read_file_bytes(target), merge_code_into_ini(kUserShapedIni, sample_code()));
    CHECK(first.plan.file_exists);
    // same stamp twice mustn't overwrite the first backup
    InstallResult second = install_code(target, sample_code(), {"20260101-000000"});
    CHECK_EQ(second.backup->string(), (scratch.path / "GALE01.ini.bak-20260101-000000-2").string());
    CHECK(!fs::exists(scratch.path / "GALE01.ini.tmp"));
}

TEST(userini_new_file) {
    TempDir scratch;
    fs::path target = scratch.path / "GameSettings" / "GALE01.ini";
    InstallResult result = install_code(target, sample_code(), {"stamp"});
    CHECK(!result.backup.has_value());
    CHECK(!result.plan.file_exists);
    CHECK_EQ(read_file_bytes(target),
        std::string("[Gecko]\n$My Cool Mod [Decomp2Gecko]\n200679F0 480000CD\n040679F0 49698611\n\n"
                    "[Gecko_Enabled]\n$My Cool Mod\n"));
}
