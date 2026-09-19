#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Decomp2Gecko/bytes.h"

namespace Decomp2Gecko {

std::string hex_upper(int64_t value, int width = 0);

std::string hex_prefixed(int64_t value);

std::string pad_right(std::string_view text, size_t width);

std::string pad_left(std::string_view text, size_t width);

std::string hex_lower(std::string_view data);

bool is_ascii_space(char character);
bool is_hex_digit(char character);
bool is_word_char(char character);
std::string strip(std::string_view text);
std::string rstrip(std::string_view text);

std::vector<std::string> split_whitespace(std::string_view text);

std::vector<std::string> split_on(std::string_view text, char separator);

std::vector<std::string> split_lines(std::string_view text);

std::string join(const std::vector<std::string>& parts, std::string_view separator);

bool starts_with(std::string_view text, std::string_view prefix);
bool ends_with(std::string_view text, std::string_view suffix);

std::string quote_for_message(std::string_view text);

int64_t parse_hex(std::string_view text);

std::string expanduser(std::string_view path);

std::string display_path(const std::filesystem::path& path);

Bytes read_file_bytes(const std::filesystem::path& path);
void write_file_bytes(const std::filesystem::path& path, std::string_view data);
void rename_replace(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace Decomp2Gecko
