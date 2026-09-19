#include "Decomp2Gecko/compat.h"

#include "check.h"

#include <filesystem>

#include "Decomp2Gecko/elf.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/util.h"

using namespace Decomp2Gecko;
namespace fs = std::filesystem;

namespace {

void put_u32(Bytes& buffer, size_t offset, uint32_t value) { write_u32_be(buffer, offset, value); }
void put_u16(Bytes& buffer, size_t offset, uint16_t value) { write_u16_be(buffer, offset, value); }

// .text w/ two words, one function, & two RELA sections both patching .text (MWCC does this)
Bytes make_object() {
    // section bodies first, so their file offsets are known by the time headers get written
    Bytes text = packed_u32_be(0x48000001) + packed_u32_be(0x60000000);
    Bytes strtab = Bytes("\0func\0", 6);
    Bytes shstrtab = Bytes("\0.text\0.symtab\0.strtab\0.shstrtab\0.rela.text\0.rela.more\0", 54);

    Bytes symtab(32, '\0'); // null symbol + "func"
    put_u32(symtab, 16, 1); // st_name -> "func"
    put_u32(symtab, 20, 0); // st_value
    put_u32(symtab, 24, 8); // st_size
    symtab[28] = char((STB_GLOBAL << 4) | STT_FUNC);
    put_u16(symtab, 30, 1); // st_shndx -> .text

    Bytes rela_text(12, '\0');
    put_u32(rela_text, 0, 4); // r_offset
    put_u32(rela_text, 4, (1 << 8) | R_PPC_ADDR32);
    put_u32(rela_text, 8, uint32_t(-16)); // negative addend must survive the round-trip
    Bytes rela_more(12, '\0');
    put_u32(rela_more, 0, 0); // earlier offset in a later section; the sorted merge must interleave them
    put_u32(rela_more, 4, (1 << 8) | R_PPC_REL24);

    Bytes file(52, '\0');
    file.replace(0, 4, "\177ELF", 4);
    file[4] = char(ELFCLASS32);
    file[5] = char(ELFDATA2MSB);
    file[6] = 1;
    put_u16(file, 0x10, 1); // ET_REL
    put_u16(file, 0x12, 20); // EM_PPC

    size_t offsets[7];
    const Bytes* bodies[7] = {nullptr, &text, &symtab, &strtab, &shstrtab, &rela_text, &rela_more};
    for (int index = 1; index < 7; index++) {
        offsets[index] = file.size();
        file += *bodies[index];
    }

    size_t header_table = file.size();
    put_u32(file, 0x20, uint32_t(header_table));
    put_u16(file, 0x2E, 40); // e_shentsize
    put_u16(file, 0x30, 7); // e_shnum
    put_u16(file, 0x32, 4); // e_shstrndx

    auto add_header = [&](uint32_t name, uint32_t type, uint32_t flags, size_t offset, uint32_t size, uint32_t link,
                          uint32_t info, uint32_t align, uint32_t entsize) {
        Bytes header(40, '\0');
        put_u32(header, 0, name);
        put_u32(header, 4, type);
        put_u32(header, 8, flags);
        put_u32(header, 16, uint32_t(offset));
        put_u32(header, 20, size);
        put_u32(header, 24, link);
        put_u32(header, 28, info);
        put_u32(header, 32, align);
        put_u32(header, 36, entsize);
        file += header;
    };

    add_header(0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0);
    add_header(1, SHT_PROGBITS, uint32_t(SHF_ALLOC | SHF_EXECINSTR), offsets[1], uint32_t(text.size()), 0, 0, 4, 0);
    add_header(7, SHT_SYMTAB, 0, offsets[2], uint32_t(symtab.size()), 3, 1, 4, 16);
    add_header(15, SHT_STRTAB, 0, offsets[3], uint32_t(strtab.size()), 0, 0, 1, 0);
    add_header(23, SHT_STRTAB, 0, offsets[4], uint32_t(shstrtab.size()), 0, 0, 1, 0);
    add_header(33, SHT_RELA, 0, offsets[5], uint32_t(rela_text.size()), 2, 1, 4, 12);
    add_header(44, SHT_RELA, 0, offsets[6], uint32_t(rela_more.size()), 2, 1, 4, 12);
    return file;
}

struct TempFile {
    fs::path path;
    explicit TempFile(const Bytes& contents) {
        path = fs::temp_directory_path() / ("Decomp2Gecko-elf-" + std::to_string(d2g_getpid()) + ".o");
        write_file_bytes(path, contents);
    }
    ~TempFile() { fs::remove(path); }
};

} // namespace

TEST(elf_read) {
    TempFile object(make_object());
    ElfFile elf = read_elf(object.path);

    CHECK_EQ(elf.sections.size(), size_t(7));
    CHECK_EQ(elf.sections[1].name, std::string(".text"));
    CHECK(elf.sections[1].is_code());
    CHECK(elf.sections[1].is_allocated());
    CHECK_EQ(elf.sections[1].data.size(), size_t(8));
    CHECK_EQ(elf.allocated_sections().size(), size_t(1));

    CHECK_EQ(elf.symbols.size(), size_t(2));
    CHECK_EQ(elf.symbols[1].name, std::string("func"));
    CHECK_EQ(elf.symbols[1].size, int64_t(8));
    CHECK(elf.symbols[1].is_global());
    CHECK(elf.symbols[1].is_defined());
    CHECK_EQ(elf.symbols[1].binding, STB_GLOBAL);
    CHECK_EQ(elf.symbols[1].symbol_type, STT_FUNC);

    // both RELA sections patch .text, merged list comes back sorted by offset
    const std::vector<Relocation>& relocations = elf.relocations_for(1);
    CHECK_EQ(relocations.size(), size_t(2));
    CHECK_EQ(relocations[0].offset, int64_t(0));
    CHECK_EQ(relocations[0].reloc_type, R_PPC_REL24);
    CHECK_EQ(relocations[1].offset, int64_t(4));
    CHECK_EQ(relocations[1].reloc_type, R_PPC_ADDR32);
    CHECK_EQ(relocations[1].symbol_index, int64_t(1));
    CHECK_EQ(relocations[1].addend, int64_t(-16));
    CHECK(elf.relocations_for(3).empty());
    CHECK_EQ(elf.sections[2].name, std::string(".symtab"));
}
