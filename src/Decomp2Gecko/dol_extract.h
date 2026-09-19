#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "Decomp2Gecko/bytes.h"

namespace Decomp2Gecko::dolx {

inline constexpr uint32_t GC_DISC_MAGIC = 0xC2339F3D; // at 0x1C of every gamecube disc
inline constexpr size_t DISC_HEADER_SIZE = 0x440;
inline constexpr size_t DOL_HEADER_SIZE = 0x100;
inline constexpr size_t DOL_OFFSET_FIELD = 0x420;

struct IsoInfo {
    std::string game_id; // bytes 0..5, like "GALE01"
    int disc_number = 0; // byte 0x06
    int version = 0; // byte 0x07 (2 == NTSC 1.02)
    std::string title; // from 0x20, NUL-terminated
    int64_t dol_offset = 0; // big-endian u32 at 0x420
    int64_t dol_size = 0; // computed from DOL header
};

class IsoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

IsoInfo probe_iso(std::istream& iso, uint64_t iso_size);
IsoInfo probe_iso(const std::filesystem::path& iso_path);

Bytes read_dol(std::istream& iso, const IsoInfo& info);
Bytes read_dol(const std::filesystem::path& iso_path, const IsoInfo& info);

// temp file + rename, so a crash never leaves a half-written DOL
void write_file_atomically(const std::filesystem::path& target, std::string_view data);

// dtk's vanilla DOL hash from config/<game>/build.sha1 ("<40 hex>  build/<game>/main.dol")
std::optional<std::string> expected_dol_sha1(const std::filesystem::path& build_sha1_path);

} // namespace Decomp2Gecko::dolx
