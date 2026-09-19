#include "Decomp2Gecko/util.h"

#ifndef _WIN32
#include <pwd.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#include "Decomp2Gecko/errors.h"

namespace Decomp2Gecko {
namespace {

std::string unsigned_hex(uint64_t magnitude, bool uppercase) {
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string text;
    do {
        text.insert(text.begin(), digits[magnitude & 0xF]);
        magnitude >>= 4;
    } while (magnitude != 0);
    return text;
}

uint64_t magnitude_of(int64_t value) { return value < 0 ? uint64_t(-(value + 1)) + 1 : uint64_t(value); }

int hex_digit(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string hex_upper(int64_t value, int width) {
    bool negative = value < 0;
    std::string digits = unsigned_hex(magnitude_of(value), true);
    size_t wanted_digits = width > 0 ? size_t(width) - (negative ? 1 : 0) : 0;
    if (digits.size() < wanted_digits) {
        digits.insert(0, wanted_digits - digits.size(), '0');
    }
    return negative ? "-" + digits : digits;
}

std::string hex_prefixed(int64_t value) {
    return (value < 0 ? "-0x" : "0x") + unsigned_hex(magnitude_of(value), false);
}

std::string pad_right(std::string_view text, size_t width) {
    std::string padded(text);
    if (padded.size() < width) {
        padded.append(width - padded.size(), ' ');
    }
    return padded;
}

std::string pad_left(std::string_view text, size_t width) {
    std::string padded(text);
    if (padded.size() < width) {
        padded.insert(0, width - padded.size(), ' ');
    }
    return padded;
}

std::string hex_lower(std::string_view data) {
    std::string text;
    text.reserve(data.size() * 2);
    for (char byte : data) {
        text += "0123456789abcdef"[(uint8_t(byte) >> 4) & 0xF];
        text += "0123456789abcdef"[uint8_t(byte) & 0xF];
    }
    return text;
}

bool is_ascii_space(char character) {
    return character == ' ' ||
        character == '\t' ||
        character == '\n' ||
        character == '\r' ||
        character == '\v' ||
        character == '\f';
}

bool is_hex_digit(char character) {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F');
}

bool is_word_char(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '_';
}

std::string rstrip(std::string_view text) {
    size_t end = text.size();
    while (end > 0 && is_ascii_space(text[end - 1])) {
        end--;
    }
    return std::string(text.substr(0, end));
}

std::string strip(std::string_view text) {
    size_t start = 0;
    while (start < text.size() && is_ascii_space(text[start])) {
        start++;
    }
    return rstrip(text.substr(start));
}

std::vector<std::string> split_whitespace(std::string_view text) {
    std::vector<std::string> parts;
    size_t position = 0;
    while (position < text.size()) {
        while (position < text.size() && is_ascii_space(text[position])) {
            position++;
        }
        size_t start = position;
        while (position < text.size() && !is_ascii_space(text[position])) {
            position++;
        }
        if (position > start) {
            parts.emplace_back(text.substr(start, position - start));
        }
    }
    return parts;
}

std::vector<std::string> split_on(std::string_view text, char separator) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (true) {
        size_t found = text.find(separator, start);
        if (found == std::string_view::npos) {
            parts.emplace_back(text.substr(start));
            return parts;
        }
        parts.emplace_back(text.substr(start, found - start));
        start = found + 1;
    }
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;
    size_t position = 0;
    while (position < text.size()) {
        char character = text[position];
        if (character != '\n' && character != '\r') {
            position++;
            continue;
        }
        lines.emplace_back(text.substr(start, position - start));
        // \r\n pair = one line break, not two
        if (character == '\r' && position + 1 < text.size() && text[position + 1] == '\n') {
            position++;
        }
        position++;
        start = position;
    }
    if (start < text.size()) {
        lines.emplace_back(text.substr(start));
    }
    return lines;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (size_t index = 0; index < parts.size(); index++) {
        if (index > 0) {
            joined += separator;
        }
        joined += parts[index];
    }
    return joined;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string quote_for_message(std::string_view text) {
    std::string quoted = "'";
    for (char character : text) {
        switch (character) {
            case '\\': quoted += "\\\\"; break;
            case '\'': quoted += "\\'"; break;
            case '\n': quoted += "\\n"; break;
            case '\r': quoted += "\\r"; break;
            case '\t': quoted += "\\t"; break;
            default: quoted += character; break;
        }
    }
    return quoted + "'";
}

int64_t parse_hex(std::string_view text) {
    std::string trimmed = strip(text);
    std::string_view digits = trimmed;
    if (digits.size() >= 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
        digits.remove_prefix(2);
    }
    if (digits.empty()) {
        throw InvalidValueError("invalid integer: " + quote_for_message(text));
    }
    int64_t value = 0;
    for (char character : digits) {
        int digit = hex_digit(character);
        if (digit < 0) {
            throw InvalidValueError("invalid integer: " + quote_for_message(text));
        }
        value = value * 16 + digit;
    }
    return value;
}

namespace fs = std::filesystem;

namespace {

std::string home_directory_of(const std::string& user) {
    if (user.empty()) {
        if (const char* home = std::getenv("HOME")) {
            return home;
        }
#ifdef _WIN32
        if (const char* profile = std::getenv("USERPROFILE")) {
            return profile;
        }
#else
        if (const passwd* entry = getpwuid(getuid())) {
            return entry->pw_dir;
        }
#endif
        return "";
    }
#ifndef _WIN32
    if (const passwd* entry = getpwnam(user.c_str())) {
        return entry->pw_dir;
    }
#endif
    return "";
}

} // namespace

std::string expanduser(std::string_view path) {
    if (path.empty() || path[0] != '~') {
        return std::string(path);
    }
    size_t slash = path.find_first_of("/\\");
    std::string user(path.substr(1, slash == std::string_view::npos ? std::string_view::npos : slash - 1));
    std::string home = home_directory_of(user);
    if (home.empty()) {
        return std::string(path);
    }
    std::replace(home.begin(), home.end(), '\\', '/');
    if (slash == std::string_view::npos) {
        return home;
    }
    if (home.back() == '/') {
        home.pop_back();
    }
    std::string result = home + std::string(path.substr(slash));
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string display_path(const fs::path& path) {
    std::string text = path.lexically_normal().string();
    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    return text.empty() ? "." : text;
}

Bytes read_file_bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw MissingFileError("no such file: " + quote_for_message(path.string()));
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return contents.str();
}

void write_file_bytes(const fs::path& path, std::string_view data) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        throw MissingFileError("cannot write " + quote_for_message(path.string()));
    }
    stream.write(data.data(), std::streamsize(data.size()));
}

void rename_replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    std::error_code ec;
    for (int attempt = 0; attempt < 5; attempt++) {
        fs::rename(from, to, ec);
        if (!ec) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    throw std::runtime_error("cannot rename " + from.string() + " to " + to.string() + ": " + ec.message());
#else
    fs::rename(from, to);
#endif
}

} // namespace Decomp2Gecko
