#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Decomp2Gecko {

struct JsonValue {
    enum class Kind { Null, Boolean, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    int64_t integer = 0;
    bool is_integer = false;
    std::string text;
    std::vector<JsonValue> items;
    std::vector<std::string> keys;
    std::vector<JsonValue> values;

    bool is_string() const { return kind == Kind::String; }

    // last duplicate key wins; nullptr when absent or not an object
    const JsonValue* get(const std::string& key) const;
};

JsonValue parse_json(std::string_view text);

} // namespace Decomp2Gecko
