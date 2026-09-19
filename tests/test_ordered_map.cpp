#include "check.h"
#include "Decomp2Gecko/ordered_map.h"

using namespace Decomp2Gecko;

TEST(ordered_map_order) {
    OrderedMap<std::string, int> map;
    map["b"] = 1;
    map["a"] = 2;
    map["b"] = 3; // reassignment keeps first position
    map.setdefault("a", 99); // no change for existing key
    map.setdefault("c", 4);
    std::vector<std::string> keys;
    for (const auto& [key, value] : map) {
        keys.push_back(key);
    }
    CHECK_EQ(keys.size(), size_t(3));
    CHECK_EQ(keys[0], std::string("b"));
    CHECK_EQ(keys[1], std::string("a"));
    CHECK_EQ(keys[2], std::string("c"));
    CHECK_EQ(*map.find("b"), 3);
    CHECK_EQ(*map.find("a"), 2);
    CHECK(map.contains("c"));
    CHECK(!map.contains("d"));
}
