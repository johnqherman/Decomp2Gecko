// a whole checkout on disk, built by hand: nothing in the repo may depend on the real game
#pragma once

#include "Decomp2Gecko/compat.h"

#include <filesystem>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

#include "Decomp2Gecko/bytes.h"
#include "Decomp2Gecko/elf.h"
#include "Decomp2Gecko/util.h"
#include "Decomp2Gecko/vanilla.h"

namespace synthetic {

using Decomp2Gecko::Bytes;
namespace fs = std::filesystem;

inline Bytes words(std::initializer_list<uint32_t> values) {
    Bytes packed;
    for (uint32_t value : values) {
        packed += Decomp2Gecko::packed_u32_be(value);
    }
    return packed;
}

struct ScratchDir {
    fs::path path;
    ScratchDir() {
        path = fs::temp_directory_path() /
            ("Decomp2Gecko-synthetic-" + std::to_string(d2g_getpid()) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~ScratchDir() { fs::remove_all(path); }
    static inline int counter = 0;
};

// sections first, then .symtab/.strtab/.shstrtab, then one .rela.<name> per section
class ElfBuilder {
public:
    ElfBuilder& section(
        const std::string& name, int64_t flags, Bytes data, int64_t alignment = 4, int64_t address = 0) {
        sections_.push_back({name, Decomp2Gecko::SHT_PROGBITS, flags, std::move(data), 0, alignment, address});
        return *this;
    }
    ElfBuilder& bss(const std::string& name, int64_t size, int64_t alignment = 8, int64_t address = 0) {
        sections_.push_back({name, Decomp2Gecko::SHT_NOBITS, Decomp2Gecko::SHF_ALLOC | Decomp2Gecko::SHF_WRITE, {},
            size, alignment, address});
        return *this;
    }
    ElfBuilder& symbol(
        const std::string& name, const std::string& section, int64_t value, int64_t size, int type, int binding) {
        symbols_.push_back({name, section_index(section), value, size, type, binding});
        return *this;
    }
    ElfBuilder& undefined(const std::string& name) {
        symbols_.push_back({name, Decomp2Gecko::SHN_UNDEF, 0, 0, Decomp2Gecko::STT_NOTYPE, Decomp2Gecko::STB_GLOBAL});
        return *this;
    }
    ElfBuilder& absolute(const std::string& name, int64_t value) {
        symbols_.push_back({name, Decomp2Gecko::SHN_ABS, value, 0, Decomp2Gecko::STT_NOTYPE, Decomp2Gecko::STB_GLOBAL});
        return *this;
    }
    ElfBuilder& reloc(
        const std::string& section, int64_t offset, int type, const std::string& symbol, int64_t addend = 0) {
        relocs_.push_back({section_index(section), offset, type, symbol_index(symbol), addend});
        return *this;
    }

    Bytes build(bool executable = false) const {
        using namespace Decomp2Gecko;

        Bytes shstrtab("\0", 1);
        auto add_name = [&](const std::string& name) {
            uint32_t at = uint32_t(shstrtab.size());
            shstrtab += name;
            shstrtab += '\0';
            return at;
        };

        std::vector<uint32_t> user_names;
        for (const SectionSpec& spec : sections_) {
            user_names.push_back(add_name(spec.name));
        }
        uint32_t symtab_name = add_name(".symtab");
        uint32_t strtab_name = add_name(".strtab");
        uint32_t shstrtab_name = add_name(".shstrtab");

        std::vector<int> rela_targets;
        std::vector<uint32_t> rela_names;
        for (const RelocSpec& spec : relocs_) {
            bool seen = false;
            for (int target : rela_targets) {
                seen = seen || target == spec.section_index;
            }
            if (seen) {
                continue;
            }
            rela_targets.push_back(spec.section_index);
            rela_names.push_back(add_name(".rela" + sections_[size_t(spec.section_index - 1)].name));
        }

        Bytes strtab("\0", 1);
        Bytes symtab(16, '\0'); // null symbol
        for (const SymbolSpec& spec : symbols_) {
            Bytes entry(16, '\0');
            write_u32_be(entry, 0, uint32_t(strtab.size()));
            strtab += spec.name;
            strtab += '\0';
            write_u32_be(entry, 4, uint32_t(spec.value));
            write_u32_be(entry, 8, uint32_t(spec.size));
            entry[12] = char((spec.binding << 4) | spec.type);
            write_u16_be(entry, 14, uint16_t(spec.section_index));
            symtab += entry;
        }

        struct Header {
            uint32_t name, type, flags, address, offset, size, link, info, align, entsize;
        };
        std::vector<Header> headers = {{0, SHT_NULL, 0, 0, 0, 0, 0, 0, 0, 0}};
        Bytes file(52, '\0');
        auto append = [&](const Bytes& body) {
            uint32_t at = uint32_t(file.size());
            file += body;
            return at;
        };

        for (size_t index = 0; index < sections_.size(); index++) {
            const SectionSpec& spec = sections_[index];
            uint32_t offset = spec.type == SHT_NOBITS ? uint32_t(file.size()) : append(spec.data);
            uint32_t size = spec.type == SHT_NOBITS ? uint32_t(spec.size) : uint32_t(spec.data.size());
            headers.push_back({user_names[index], uint32_t(spec.type), uint32_t(spec.flags), uint32_t(spec.address),
                offset, size, 0, 0, uint32_t(spec.alignment), 0});
        }

        uint32_t symtab_index = uint32_t(headers.size());
        uint32_t strtab_index = symtab_index + 1;
        headers.push_back(
            {symtab_name, SHT_SYMTAB, 0, 0, append(symtab), uint32_t(symtab.size()), strtab_index, 1, 4, 16});
        headers.push_back({strtab_name, SHT_STRTAB, 0, 0, append(strtab), uint32_t(strtab.size()), 0, 0, 1, 0});
        uint32_t shstrtab_index = uint32_t(headers.size());
        headers.push_back({shstrtab_name, SHT_STRTAB, 0, 0, append(shstrtab), uint32_t(shstrtab.size()), 0, 0, 1, 0});

        for (size_t index = 0; index < rela_targets.size(); index++) {
            Bytes rela;
            for (const RelocSpec& spec : relocs_) {
                if (spec.section_index != rela_targets[index]) {
                    continue;
                }
                Bytes entry(12, '\0');
                write_u32_be(entry, 0, uint32_t(spec.offset));
                write_u32_be(entry, 4, (uint32_t(spec.symbol_index) << 8) | uint32_t(spec.type));
                write_u32_be(entry, 8, uint32_t(spec.addend));
                rela += entry;
            }
            headers.push_back({rela_names[index], SHT_RELA, 0, 0, append(rela), uint32_t(rela.size()), symtab_index,
                uint32_t(rela_targets[index]), 4, 12});
        }

        uint32_t header_table = uint32_t(file.size());
        for (const Header& header : headers) {
            Bytes raw(40, '\0');
            const uint32_t fields[10] = {header.name, header.type, header.flags, header.address, header.offset,
                header.size, header.link, header.info, header.align, header.entsize};
            for (size_t field = 0; field < 10; field++) {
                write_u32_be(raw, field * 4, fields[field]);
            }
            file += raw;
        }

        file.replace(0, 4, "\177ELF", 4);
        file[4] = char(ELFCLASS32);
        file[5] = char(ELFDATA2MSB);
        file[6] = 1;
        write_u16_be(file, 0x10, executable ? 2 : 1); // ET_EXEC / ET_REL
        write_u16_be(file, 0x12, 20); // EM_PPC
        write_u32_be(file, 0x14, 1);
        write_u32_be(file, 0x20, header_table);
        write_u16_be(file, 0x28, 52);
        write_u16_be(file, 0x2E, 40);
        write_u16_be(file, 0x30, uint16_t(headers.size()));
        write_u16_be(file, 0x32, uint16_t(shstrtab_index));
        return file;
    }

private:
    struct SectionSpec {
        std::string name;
        int type;
        int64_t flags;
        Bytes data;
        int64_t size; // for NOBITS
        int64_t alignment;
        int64_t address;
    };
    struct SymbolSpec {
        std::string name;
        int section_index;
        int64_t value;
        int64_t size;
        int type;
        int binding;
    };
    struct RelocSpec {
        int section_index;
        int64_t offset;
        int type;
        int symbol_index;
        int64_t addend;
    };

    int section_index(const std::string& name) const {
        for (size_t index = 0; index < sections_.size(); index++) {
            if (sections_[index].name == name) {
                return int(index) + 1;
            }
        }
        throw std::logic_error("synthetic object has no section " + name);
    }
    int symbol_index(const std::string& name) const {
        for (size_t index = 0; index < symbols_.size(); index++) {
            if (symbols_[index].name == name) {
                return int(index) + 1;
            }
        }
        throw std::logic_error("synthetic object has no symbol " + name);
    }

    std::vector<SectionSpec> sections_;
    std::vector<SymbolSpec> symbols_;
    std::vector<RelocSpec> relocs_;
};

// DOL w/ the given text & data sections, bss isn't part of the file
struct DolSection {
    int64_t address;
    Bytes data;
};

inline Bytes build_dol(const std::vector<DolSection>& text, const std::vector<DolSection>& data) {
    using namespace Decomp2Gecko;
    Bytes dol(0x100, '\0');
    auto place = [&](int slot, const DolSection& section) {
        write_u32_be(dol, size_t(DOL_OFFSET_TABLE + slot * 4), uint32_t(dol.size()));
        write_u32_be(dol, size_t(DOL_ADDRESS_TABLE + slot * 4), uint32_t(section.address));
        write_u32_be(dol, size_t(DOL_SIZE_TABLE + slot * 4), uint32_t(section.data.size()));
        dol += section.data;
    };
    for (size_t index = 0; index < text.size(); index++) {
        place(int(index), text[index]);
    }
    for (size_t index = 0; index < data.size(); index++) {
        place(7 + int(index), data[index]);
    }
    return dol;
}

struct Checkout {
    fs::path root;

    explicit Checkout(fs::path checkout_root) : root(std::move(checkout_root)) {}

    void vanilla(const Bytes& dol, const std::string& symbols, const std::string& splits) const {
        write(root / "orig/GALE01/sys/main.dol", dol);
        write(root / "config/GALE01/symbols.txt", symbols);
        write(root / "config/GALE01/splits.txt", splits);
    }
    void config(const std::vector<std::string>& unit_names) const {
        std::string json = "{\"units\": [";
        for (size_t index = 0; index < unit_names.size(); index++) {
            json += (index ? ", " : "") + std::string("{\"name\": \"") + unit_names[index] + "\"}";
        }
        write(root / "build/GALE01/config.json", json + "]}");
    }
    void object(const std::string& stem, const Bytes& elf) const {
        write(root / "build/GALE01/src" / (stem + ".o"), elf);
    }
    void main_elf(const Bytes& elf) const { write(root / "build/GALE01/main.elf", elf); }

    static void write(const fs::path& path, const Bytes& contents) {
        fs::create_directories(path.parent_path());
        Decomp2Gecko::write_file_bytes(path, contents);
    }
};

} // namespace synthetic
