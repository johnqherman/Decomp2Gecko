#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace Decomp2Gecko {

using Bytes = std::string;

inline uint32_t read_u32_be(std::string_view data, size_t offset) {
    return (uint32_t(uint8_t(data[offset])) << 24) | (uint32_t(uint8_t(data[offset + 1])) << 16) |
        (uint32_t(uint8_t(data[offset + 2])) << 8) | uint32_t(uint8_t(data[offset + 3]));
}

inline uint16_t read_u16_be(std::string_view data, size_t offset) {
    return uint16_t((uint16_t(uint8_t(data[offset])) << 8) | uint16_t(uint8_t(data[offset + 1])));
}

inline void write_u32_be(Bytes& data, size_t offset, uint32_t value) {
    data[offset] = char(value >> 24);
    data[offset + 1] = char(value >> 16);
    data[offset + 2] = char(value >> 8);
    data[offset + 3] = char(value);
}

inline void write_u16_be(Bytes& data, size_t offset, uint16_t value) {
    data[offset] = char(value >> 8);
    data[offset + 1] = char(value);
}

inline Bytes packed_u32_be(uint32_t value) {
    Bytes packed(4, '\0');
    write_u32_be(packed, 0, value);
    return packed;
}

// data[start, end) as string_view, bounds clamped to the buffer where substr would throw
inline std::string_view slice(std::string_view data, int64_t start, int64_t end) {
    if (start < 0) {
        start = 0;
    }
    if (end > int64_t(data.size())) {
        end = int64_t(data.size());
    }
    if (end < start) {
        return std::string_view();
    }
    return data.substr(size_t(start), size_t(end - start));
}

// overwrites buffer[offset, offset + data.size()) & ignores the part outside the buffer
inline void overwrite(Bytes& buffer, int64_t offset, std::string_view data) {
    for (size_t index = 0; index < data.size(); index++) {
        int64_t position = offset + int64_t(index);
        if (position >= 0 && position < int64_t(buffer.size())) {
            buffer[size_t(position)] = data[index];
        }
    }
}

inline bool any_nonzero(std::string_view data) {
    for (char byte : data) {
        if (byte != '\0') {
            return true;
        }
    }
    return false;
}

} // namespace Decomp2Gecko
