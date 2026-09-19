#include "Decomp2Gecko/dol_extract.h"

#include "Decomp2Gecko/compat.h"

#include <fstream>

#include "Decomp2Gecko/util.h"
#include "Decomp2Gecko/vanilla.h"

namespace Decomp2Gecko::dolx {

namespace {

using Decomp2Gecko::DOL_ENTRY_FIELD;
using Decomp2Gecko::DOL_OFFSET_TABLE;
using Decomp2Gecko::DOL_SECTION_SLOTS;
using Decomp2Gecko::DOL_SIZE_TABLE;
using Decomp2Gecko::GC_MEM1_END;
using Decomp2Gecko::GC_MEM1_START;

Bytes read_exact(std::istream& stream, uint64_t offset, size_t count, const char* what) {
    Bytes buffer(count, '\0');
    stream.clear();
    stream.seekg(std::streamoff(offset));
    stream.read(buffer.data(), std::streamsize(count));
    if (stream.gcount() != std::streamsize(count)) {
        throw IsoError(std::string("truncated image while reading the ") + what);
    }
    return buffer;
}

int64_t dol_length_from_header(std::string_view header) {
    if (header.size() < DOL_HEADER_SIZE) {
        throw IsoError("DOL header is shorter than 0x100 bytes");
    }
    int64_t furthest_end = 0;
    for (int slot = 0; slot < DOL_SECTION_SLOTS; slot++) {
        int64_t offset = read_u32_be(header, DOL_OFFSET_TABLE + size_t(slot) * 4);
        int64_t size = read_u32_be(header, DOL_SIZE_TABLE + size_t(slot) * 4);
        if (offset == 0 || size == 0) {
            continue;
        }
        if (offset < int64_t(DOL_HEADER_SIZE)) {
            throw IsoError("corrupt DOL header: a section starts inside the header");
        }
        furthest_end = std::max(furthest_end, offset + size);
    }
    if (furthest_end == 0) {
        throw IsoError("corrupt DOL header: no sections");
    }
    return furthest_end;
}

} // namespace

IsoInfo probe_iso(std::istream& iso, uint64_t iso_size) {
    if (iso_size < DISC_HEADER_SIZE) {
        throw IsoError("too small to be a GameCube image");
    }
    Bytes header = read_exact(iso, 0, DISC_HEADER_SIZE, "disc header");
    if (read_u32_be(header, 0x1C) != GC_DISC_MAGIC) {
        throw IsoError(
            "not a GameCube disc image (bad magic at 0x1C); compressed images (RVZ/GCZ/CISO) are not supported");
    }
    IsoInfo info;
    info.game_id = header.substr(0, 6);
    for (char character : info.game_id) {
        if (character < 0x21 || character > 0x7E) {
            throw IsoError("unreadable game id in the disc header");
        }
    }
    info.disc_number = uint8_t(header[6]);
    info.version = uint8_t(header[7]);
    std::string title = header.substr(0x20, 0x400);
    info.title = strip(title.substr(0, title.find('\0')));
    info.dol_offset = read_u32_be(header, DOL_OFFSET_FIELD);
    if (info.dol_offset < int64_t(DISC_HEADER_SIZE) || uint64_t(info.dol_offset) + DOL_HEADER_SIZE > iso_size) {
        throw IsoError("DOL offset 0x" + hex_upper(info.dol_offset) + " is outside the image");
    }
    Bytes dol_header = read_exact(iso, uint64_t(info.dol_offset), DOL_HEADER_SIZE, "DOL header");
    info.dol_size = dol_length_from_header(dol_header);
    if (uint64_t(info.dol_offset + info.dol_size) > iso_size) {
        throw IsoError("truncated image: the DOL runs past the end");
    }
    int64_t entry_point = read_u32_be(dol_header, DOL_ENTRY_FIELD);
    int64_t first_text_offset = read_u32_be(dol_header, DOL_OFFSET_TABLE);
    if (entry_point < GC_MEM1_START || entry_point >= GC_MEM1_END || first_text_offset != int64_t(DOL_HEADER_SIZE)) {
        throw IsoError("corrupt DOL header (entry point or first text section look wrong)");
    }
    return info;
}

IsoInfo probe_iso(const std::filesystem::path& iso_path) {
    std::ifstream stream(iso_path, std::ios::binary);
    if (!stream) {
        throw IsoError("cannot open " + iso_path.string());
    }
    std::error_code error;
    uint64_t size = std::filesystem::file_size(iso_path, error);
    if (error) {
        throw IsoError("cannot stat " + iso_path.string());
    }
    return probe_iso(stream, size);
}

Bytes read_dol(std::istream& iso, const IsoInfo& info) {
    return read_exact(iso, uint64_t(info.dol_offset), size_t(info.dol_size), "DOL");
}

Bytes read_dol(const std::filesystem::path& iso_path, const IsoInfo& info) {
    std::ifstream stream(iso_path, std::ios::binary);
    if (!stream) {
        throw IsoError("cannot open " + iso_path.string());
    }
    return read_dol(stream, info);
}

void write_file_atomically(const std::filesystem::path& target, std::string_view data) {
    if (target.has_parent_path()) {
        std::filesystem::create_directories(target.parent_path());
    }
    std::filesystem::path temporary = target.string() + ".tmp-" + std::to_string(d2g_getpid());
    write_file_bytes(temporary, data);
    rename_replace(temporary, target);
}

std::optional<std::string> expected_dol_sha1(const std::filesystem::path& build_sha1_path) {
    std::ifstream stream(build_sha1_path);
    if (!stream) {
        return std::nullopt;
    }
    std::string line;
    while (std::getline(stream, line)) {
        std::vector<std::string> parts = split_whitespace(line);
        if (parts.size() == 2 && parts[0].size() == 40 && ends_with(parts[1], "main.dol")) {
            return parts[0];
        }
    }
    return std::nullopt;
}

} // namespace Decomp2Gecko::dolx
