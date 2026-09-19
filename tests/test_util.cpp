#include "check.h"

#include <algorithm>
#include <cstdlib>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/util.h"

using namespace Decomp2Gecko;

TEST(util_edge_cases) {
    CHECK_EQ(hex_upper(-5, 8), std::string("-0000005")); // sign counts toward the width
    CHECK_EQ(hex_prefixed(-5), std::string("-0x5"));
    CHECK_EQ(pad_right("abcdefg", 5), std::string("abcdefg")); // never truncates
    std::vector<std::string> lines = split_lines("a\nb\r\nc\rd\n"); // Slippi INI written on Windows uses \r\n
    CHECK_EQ(lines.size(), size_t(4));
    CHECK_EQ(lines[3], std::string("d"));
    CHECK_EQ(split_lines("").size(), size_t(0));
    CHECK_EQ(quote_for_message("it's"), std::string("'it\\'s'"));
    CHECK_THROWS(parse_hex("0xZZ"), InvalidValueError);
    CHECK_THROWS(parse_hex(""), InvalidValueError);
    const char* home = std::getenv("HOME");
    if (!home) {
        home = std::getenv("USERPROFILE");
    }
    CHECK(home != nullptr);
    std::string expected_home = std::string(home);
    std::replace(expected_home.begin(), expected_home.end(), '\\', '/');
    CHECK_EQ(expanduser("~/code/melee"), expected_home + "/code/melee");
    CHECK_EQ(expanduser("/abs/path"), std::string("/abs/path"));
    CHECK_EQ(display_path("out//x.ini/"), std::string("out/x.ini"));
    CHECK_THROWS(read_file_bytes("/nonexistent/Decomp2Gecko"), MissingFileError);
}
