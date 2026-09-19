#include "Decomp2Gecko/json_mini.h"

#include <cstdlib>

#include "Decomp2Gecko/errors.h"

namespace Decomp2Gecko {

const JsonValue* JsonValue::get(const std::string& key) const {
    if (kind != Kind::Object) {
        return nullptr;
    }
    const JsonValue* found = nullptr;
    for (size_t index = 0; index < keys.size(); index++) {
        if (keys[index] == key) {
            found = &values[index];
        }
    }
    return found;
}

namespace {

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse_document() {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (position_ != text_.size()) {
            fail("Extra data");
        }
        return value;
    }

private:
    [[noreturn]] void fail(const std::string& what) {
        size_t line = 1, column = 1;
        for (size_t index = 0; index < position_ && index < text_.size(); index++) {
            if (text_[index] == '\n') {
                line++;
                column = 1;
            } else {
                column++;
            }
        }
        throw InvalidValueError(what + ": line " + std::to_string(line) + " column " + std::to_string(column) +
            " (char " + std::to_string(position_) + ")");
    }

    void skip_whitespace() {
        while (position_ < text_.size() &&
            (text_[position_] == ' ' || text_[position_] == '\t' || text_[position_] == '\n' ||
                text_[position_] == '\r')) {
            position_++;
        }
    }

    bool consume(std::string_view literal) {
        if (text_.compare(position_, literal.size(), literal) != 0) {
            return false;
        }
        position_ += literal.size();
        return true;
    }

    JsonValue parse_value() {
        if (position_ >= text_.size()) {
            fail("Expecting value");
        }
        char head = text_[position_];
        JsonValue value;
        if (head == '{') {
            return parse_object();
        }
        if (head == '[') {
            return parse_array();
        }
        if (head == '"') {
            value.kind = JsonValue::Kind::String;
            value.text = parse_string();
            return value;
        }
        if (consume("null")) {
            return value;
        }
        if (consume("true")) {
            value.kind = JsonValue::Kind::Boolean;
            value.boolean = true;
            return value;
        }
        if (consume("false")) {
            value.kind = JsonValue::Kind::Boolean;
            return value;
        }
        if (head == '-' || (head >= '0' && head <= '9')) {
            return parse_number();
        }
        fail("Expecting value");
    }

    JsonValue parse_number() {
        size_t start = position_;
        if (text_[position_] == '-') {
            position_++;
        }
        auto digits = [&]() {
            size_t begin = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                position_++;
            }
            return position_ > begin;
        };
        if (!digits()) {
            fail("Expecting value");
        }
        bool fractional = false;
        if (position_ < text_.size() && text_[position_] == '.') {
            fractional = true;
            position_++;
            if (!digits()) {
                fail("Expecting value");
            }
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            fractional = true;
            position_++;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) {
                position_++;
            }
            if (!digits()) {
                fail("Expecting value");
            }
        }
        std::string literal(text_.substr(start, position_ - start));
        JsonValue value;
        value.kind = JsonValue::Kind::Number;
        value.number = std::strtod(literal.c_str(), nullptr);
        if (!fractional) {
            value.is_integer = true;
            value.integer = std::strtoll(literal.c_str(), nullptr, 10);
        }
        return value;
    }

    static void append_utf8(std::string& out, uint32_t code_point) {
        if (code_point < 0x80) {
            out += char(code_point);
        } else if (code_point < 0x800) {
            out += char(0xC0 | (code_point >> 6));
            out += char(0x80 | (code_point & 0x3F));
        } else if (code_point < 0x10000) {
            out += char(0xE0 | (code_point >> 12));
            out += char(0x80 | ((code_point >> 6) & 0x3F));
            out += char(0x80 | (code_point & 0x3F));
        } else {
            out += char(0xF0 | (code_point >> 18));
            out += char(0x80 | ((code_point >> 12) & 0x3F));
            out += char(0x80 | ((code_point >> 6) & 0x3F));
            out += char(0x80 | (code_point & 0x3F));
        }
    }

    uint32_t parse_hex4() {
        if (position_ + 4 > text_.size()) {
            fail("Invalid \\uXXXX escape");
        }
        uint32_t value = 0;
        for (int index = 0; index < 4; index++) {
            char character = text_[position_++];
            value <<= 4;
            if (character >= '0' && character <= '9') {
                value |= uint32_t(character - '0');
            } else if (character >= 'a' && character <= 'f') {
                value |= uint32_t(character - 'a' + 10);
            } else if (character >= 'A' && character <= 'F') {
                value |= uint32_t(character - 'A' + 10);
            } else {
                fail("Invalid \\uXXXX escape");
            }
        }
        return value;
    }

    std::string parse_string() {
        position_++;
        std::string out;
        while (true) {
            if (position_ >= text_.size()) {
                fail("Unterminated string starting at");
            }
            char character = text_[position_++];
            if (character == '"') {
                return out;
            }
            if (character != '\\') {
                out += character;
                continue;
            }
            if (position_ >= text_.size()) {
                fail("Unterminated string starting at");
            }
            char escape = text_[position_++];
            switch (escape) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t code_point = parse_hex4();
                    // surrogate pairs encode chars above U+FFFF
                    if (code_point >= 0xD800 && code_point <= 0xDBFF && consume("\\u")) {
                        uint32_t low = parse_hex4();
                        code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
                    }
                    append_utf8(out, code_point);
                    break;
                }
                default: fail("Invalid \\escape");
            }
        }
    }

    JsonValue parse_array() {
        JsonValue array;
        array.kind = JsonValue::Kind::Array;
        position_++;
        skip_whitespace();
        if (consume("]")) {
            return array;
        }
        while (true) {
            skip_whitespace();
            array.items.push_back(parse_value());
            skip_whitespace();
            if (consume(",")) {
                continue;
            }
            if (consume("]")) {
                return array;
            }
            fail("Expecting ',' delimiter");
        }
    }

    JsonValue parse_object() {
        JsonValue object;
        object.kind = JsonValue::Kind::Object;
        position_++;
        skip_whitespace();
        if (consume("}")) {
            return object;
        }
        while (true) {
            skip_whitespace();
            if (position_ >= text_.size() || text_[position_] != '"') {
                fail("Expecting property name enclosed in double quotes");
            }
            std::string key = parse_string();
            skip_whitespace();
            if (!consume(":")) {
                fail("Expecting ':' delimiter");
            }
            skip_whitespace();
            object.keys.push_back(std::move(key));
            object.values.push_back(parse_value());
            skip_whitespace();
            if (consume(",")) {
                continue;
            }
            if (consume("}")) {
                return object;
            }
            fail("Expecting ',' delimiter");
        }
    }

    std::string_view text_;
    size_t position_ = 0;
};

} // namespace

JsonValue parse_json(std::string_view text) { return JsonParser(text).parse_document(); }

} // namespace Decomp2Gecko
