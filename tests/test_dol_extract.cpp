#include "Decomp2Gecko/compat.h"

#include "check.h"

#include <filesystem>
#include <sstream>

#include "Decomp2Gecko/dol_extract.h"
#include "Decomp2Gecko/util.h"

using namespace Decomp2Gecko;
using namespace Decomp2Gecko::dolx;
namespace fs = std::filesystem;

namespace {

void put_u32(Bytes& buffer, size_t offset, uint32_t value) { write_u32_be(buffer, offset, value); }

// tiny disc image: header, then a two-section DOL at dol_offset, then junk
Bytes make_iso(uint32_t dol_offset, const std::string& game_id) {
    Bytes iso(dol_offset + 0x160 + 0x200, '\xCC');
    for (size_t index = 0; index < 0x440; index++) {
        iso[index] = '\0';
    }
    iso.replace(0, 6, game_id);
    iso[6] = '\0'; // disc 0
    iso[7] = '\x02'; // NTSC 1.02
    put_u32(iso, 0x1C, GC_DISC_MAGIC);
    std::string title = "Super Smash Bros Melee";
    iso.replace(0x20, title.size(), title);
    put_u32(iso, 0x420, dol_offset);
    put_u32(iso, 0x424, dol_offset + 0x1000);

    // DOL header: text0 at 0x100 (0x20 bytes), data0 (slot 7) at 0x120 (0x40 bytes)
    for (size_t index = 0; index < 0x100; index++) {
        iso[dol_offset + index] = '\0';
    }
    put_u32(iso, dol_offset + 0x00, 0x100);
    put_u32(iso, dol_offset + 0x48, 0x80003100);
    put_u32(iso, dol_offset + 0x90, 0x20);
    put_u32(iso, dol_offset + 0x1C, 0x120);
    put_u32(iso, dol_offset + 0x64, 0x803B7240);
    put_u32(iso, dol_offset + 0xAC, 0x40);
    put_u32(iso, dol_offset + 0xD8, 0x804316C0);
    put_u32(iso, dol_offset + 0xDC, 0xA6309);
    put_u32(iso, dol_offset + 0xE0, 0x8000522C);

    for (size_t index = 0; index < 0x60; index++) {
        iso[dol_offset + 0x100 + index] = char(index);
    }
    return iso;
}

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("Decomp2Gecko-dol-" + std::to_string(d2g_getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
    static inline int counter = 0;
};

} // namespace

TEST(dol_probe_read) {
    Bytes iso = make_iso(0x1E800, "GALE01");
    std::istringstream stream(iso);
    IsoInfo info = probe_iso(stream, iso.size());
    CHECK_EQ(info.game_id, std::string("GALE01"));
    CHECK_EQ(info.version, 2);
    CHECK_EQ(info.title, std::string("Super Smash Bros Melee"));
    CHECK_EQ(info.dol_offset, int64_t(0x1E800));
    CHECK_EQ(info.dol_size, int64_t(0x160));
    Bytes dol = read_dol(stream, info);
    CHECK_EQ(dol.size(), size_t(0x160));
    CHECK_EQ(dol, iso.substr(0x1E800, 0x160));
}

// compressed image (RVZ/GCZ) has no gamecube magic at 0x1C
TEST(dol_bad_magic) {
    Bytes bad_magic = make_iso(0x1000, "GALE01");
    put_u32(bad_magic, 0x1C, 0);
    std::istringstream bad_magic_stream(bad_magic);
    CHECK_THROWS(probe_iso(bad_magic_stream, bad_magic.size()), IsoError);
}

TEST(dol_write_sha1) {
    TempDir scratch;
    fs::path iso_path = scratch.path / "game.iso";
    write_file_bytes(iso_path, make_iso(0x1000, "GALE01"));
    fs::path out = scratch.path / "orig" / "GALE01" / "sys" / "main.dol";

    IsoInfo info = probe_iso(iso_path);
    CHECK_EQ(info.dol_size, int64_t(0x160));
    write_file_atomically(out, read_dol(iso_path, info));
    CHECK(fs::exists(out));
    CHECK_EQ(read_file_bytes(out), read_dol(iso_path, info));

    fs::path sha_file = scratch.path / "build.sha1";
    write_file_bytes(sha_file, "08e0bf20134dfcb260699671004527b2d6bb1a45  build/GALE01/main.dol\n");
    CHECK_EQ(*expected_dol_sha1(sha_file), std::string("08e0bf20134dfcb260699671004527b2d6bb1a45"));
    write_file_bytes(sha_file, "deadbeef  something/else\n");
    CHECK(!expected_dol_sha1(sha_file).has_value());
    CHECK(!expected_dol_sha1(scratch.path / "missing").has_value());
}
