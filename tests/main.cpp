#include "check.h"

int main(int argc, char** argv) {
    std::string filter = argc > 1 ? argv[1] : "";
    int failures = 0;
    int ran = 0;
    for (const testing::TestCase& test : testing::registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) {
            continue;
        }
        ran++;
        try {
            test.body();
        } catch (const std::exception& error) {
            failures++;
            std::cerr << "FAIL " << test.name << "\n    " << error.what() << "\n";
        }
    }
    std::cerr << ran - failures << "/" << ran << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
