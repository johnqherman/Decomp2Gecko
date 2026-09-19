// sections, symbols & RELA tables only; works for .o & main.elf
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "Decomp2Gecko/bytes.h"

namespace Decomp2Gecko {

inline constexpr int SHT_NULL = 0;
inline constexpr int SHT_PROGBITS = 1;
inline constexpr int SHT_SYMTAB = 2;
inline constexpr int SHT_STRTAB = 3;
inline constexpr int SHT_RELA = 4;
inline constexpr int SHT_NOBITS = 8;

inline constexpr int64_t SHF_WRITE = 0x1;
inline constexpr int64_t SHF_ALLOC = 0x2;
inline constexpr int64_t SHF_EXECINSTR = 0x4;

inline constexpr int STT_NOTYPE = 0;
inline constexpr int STT_OBJECT = 1;
inline constexpr int STT_FUNC = 2;
inline constexpr int STT_SECTION = 3;
inline constexpr int STT_FILE = 4;

inline constexpr int STB_LOCAL = 0;
inline constexpr int STB_GLOBAL = 1;
inline constexpr int STB_WEAK = 2;

inline constexpr int SHN_UNDEF = 0;
inline constexpr int SHN_LORESERVE = 0xFF00;
inline constexpr int SHN_ABS = 0xFFF1;
inline constexpr int SHN_COMMON = 0xFFF2;

inline constexpr int ELFCLASS32 = 1;
inline constexpr int ELFDATA2MSB = 2;

struct Section {
    int index = 0;
    std::string name;
    int header_type = 0;
    int64_t flags = 0;
    int64_t address = 0;
    int64_t file_offset = 0;
    int64_t size = 0;
    int64_t link = 0;
    int64_t info = 0;
    int64_t alignment = 0;
    int64_t entry_size = 0;
    Bytes data;

    bool is_allocated() const { return (flags & SHF_ALLOC) != 0; }
    bool is_bss() const { return header_type == SHT_NOBITS; }
    bool is_code() const { return (flags & SHF_EXECINSTR) != 0; }
};

struct Symbol {
    int index = 0;
    std::string name;
    int64_t value = 0;
    int64_t size = 0;
    int symbol_type = 0;
    int binding = 0;
    int section_index = 0;

    bool is_defined() const { return section_index != SHN_UNDEF && section_index < SHN_LORESERVE; }
    bool is_undefined() const { return section_index == SHN_UNDEF; }
    // visible to other objects: both strong globals & weak symbols
    bool is_global() const { return binding == STB_GLOBAL || binding == STB_WEAK; }
};

struct Relocation {
    int64_t offset = 0; // byte offset of the patched field in its section
    int reloc_type = 0;
    int64_t symbol_index = 0;
    int64_t addend = 0;
};

struct ElfFile {
    std::filesystem::path path;
    std::vector<Section> sections;
    std::vector<Symbol> symbols;
    // keyed by patched section index, sorted by offset
    std::unordered_map<int, std::vector<Relocation>> relocations;

    std::vector<const Section*> allocated_sections() const;
    const std::vector<Relocation>& relocations_for(int section_index) const;
};

ElfFile read_elf(const std::filesystem::path& path);

} // namespace Decomp2Gecko
