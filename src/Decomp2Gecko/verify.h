#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Decomp2Gecko/layout.h"

namespace Decomp2Gecko {

struct VerifyResult {
    bool ran = false;
    int64_t checked = 0;
    int64_t ok = 0;
    std::vector<std::string> skipped;
    std::vector<std::string> problems;
};

VerifyResult verify(const Layout& layout);

} // namespace Decomp2Gecko
