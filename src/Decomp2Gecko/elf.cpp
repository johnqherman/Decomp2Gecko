#include "Decomp2Gecko/elf.h"

#include <algorithm>
#include <array>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

constexpr int64_t SYMBOL_ENTRY_SIZE = 16;
constexpr int64_t RELA_ENTRY_SIZE = 12;

} // namespace

std::vector<const Section*> ElfFile::allocated_sections() const {
    std::vector<const Section*> allocated;
    for (const Section& section : sections) {
        if (section.is_allocated() && section.size > 0) {
            allocated.push_back(&section);
        }
    }
    return allocated;
}

const std::vector<Relocation>& ElfFile::relocations_for(int section_index) const {
    static const std::vector<Relocation> none;
    auto found = relocations.find(section_index);
    return found == relocations.end() ? none : found->second;
}

namespace {

void require_bytes(const Bytes& raw, int64_t offset, int64_t count) {
    if (offset < 0 || offset + count > int64_t(raw.size())) {
        throw InvalidValueError("truncated ELF: need " + std::to_string(count) + " bytes at offset " +
            std::to_string(offset) + " but the file has " + std::to_string(raw.size()));
    }
}

uint32_t u32_at(const Bytes& raw, int64_t offset) {
    require_bytes(raw, offset, 4);
    return read_u32_be(raw, size_t(offset));
}

uint16_t u16_at(const Bytes& raw, int64_t offset) {
    require_bytes(raw, offset, 2);
    return read_u16_be(raw, size_t(offset));
}

std::string read_cstring(const Bytes& raw, int64_t table_offset, int64_t string_offset) {
    int64_t start = table_offset + string_offset;
    if (start < 0 || start > int64_t(raw.size())) {
        throw InvalidValueError("unterminated string in an ELF string table");
    }
    size_t end = raw.find('\0', size_t(start));
    if (end == Bytes::npos) {
        throw InvalidValueError("unterminated string in an ELF string table");
    }
    return raw.substr(size_t(start), end - size_t(start));
}

} // namespace

ElfFile read_elf(const std::filesystem::path& path) {
    Bytes raw = read_file_bytes(path);
    if (raw.size() < 4 || raw.compare(0, 4, "\177ELF", 4) != 0) {
        throw InvalidValueError(path.string() + ": not an ELF file");
    }
    if (raw.size() < 6 || uint8_t(raw[4]) != ELFCLASS32 || uint8_t(raw[5]) != ELFDATA2MSB) {
        throw InvalidValueError(path.string() + ": expected ELF32 big-endian");
    }

    int64_t section_header_offset = u32_at(raw, 0x20);
    int64_t header_entry_size = u16_at(raw, 0x2E);
    int64_t header_count = u16_at(raw, 0x30);
    int64_t name_table_index = u16_at(raw, 0x32);

    std::vector<std::array<int64_t, 10>> header_fields;
    for (int64_t index = 0; index < header_count; index++) {
        int64_t base = section_header_offset + index * header_entry_size;
        std::array<int64_t, 10> fields{};
        for (size_t field = 0; field < fields.size(); field++) {
            fields[field] = u32_at(raw, base + int64_t(field) * 4);
        }
        header_fields.push_back(fields);
    }

    if (name_table_index < 0 || name_table_index >= int64_t(header_fields.size())) {
        throw InvalidValueError("corrupt ELF: section index out of range");
    }
    int64_t name_table_file_offset = header_fields[size_t(name_table_index)][4];

    ElfFile elf;
    elf.path = path;
    for (size_t index = 0; index < header_fields.size(); index++) {
        const auto& fields = header_fields[index];
        Section section;
        section.index = int(index);
        section.name = read_cstring(raw, name_table_file_offset, fields[0]);
        section.header_type = int(fields[1]);
        section.flags = fields[2];
        section.address = fields[3];
        section.file_offset = fields[4];
        section.size = fields[5];
        section.link = fields[6];
        section.info = fields[7];
        section.alignment = fields[8];
        section.entry_size = fields[9];

        if (section.header_type != SHT_NOBITS) {
            section.data = Bytes(slice(raw, section.file_offset, section.file_offset + section.size));
        }
        elf.sections.push_back(std::move(section));
    }

    for (const Section& section : elf.sections) {
        if (section.header_type == SHT_SYMTAB) {
            if (section.link < 0 || section.link >= int64_t(elf.sections.size())) {
                throw InvalidValueError("corrupt ELF: section index out of range");
            }
            const Section& string_table = elf.sections[size_t(section.link)];
            int64_t entry_count = section.size / SYMBOL_ENTRY_SIZE;

            for (int64_t entry = 0; entry < entry_count; entry++) {
                int64_t base = section.file_offset + entry * SYMBOL_ENTRY_SIZE;
                require_bytes(raw, base, SYMBOL_ENTRY_SIZE);
                Symbol symbol;
                symbol.index = int(entry);
                symbol.name = read_cstring(raw, string_table.file_offset, read_u32_be(raw, size_t(base)));
                symbol.value = read_u32_be(raw, size_t(base + 4));
                symbol.size = read_u32_be(raw, size_t(base + 8));
                uint8_t info = uint8_t(raw[size_t(base + 12)]);
                symbol.symbol_type = info & 0xF;
                symbol.binding = info >> 4;
                symbol.section_index = read_u16_be(raw, size_t(base + 14));
                elf.symbols.push_back(std::move(symbol));
            }
        } else if (section.header_type == SHT_RELA) {
            // several RELA sections can patch the same target: append
            std::vector<Relocation>& target = elf.relocations[int(section.info)];
            int64_t entry_count = section.size / RELA_ENTRY_SIZE;

            for (int64_t entry = 0; entry < entry_count; entry++) {
                int64_t base = section.file_offset + entry * RELA_ENTRY_SIZE;
                require_bytes(raw, base, RELA_ENTRY_SIZE);
                uint32_t info = read_u32_be(raw, size_t(base + 4));
                Relocation relocation;
                relocation.offset = read_u32_be(raw, size_t(base));
                relocation.reloc_type = int(info & 0xFF);
                relocation.symbol_index = info >> 8;
                relocation.addend = int32_t(read_u32_be(raw, size_t(base + 8))); // signed addend
                target.push_back(relocation);
            }
        }
    }

    // stable, so entries at the same offset keep their file order
    for (auto& [section_index, entries] : elf.relocations) {
        std::stable_sort(entries.begin(), entries.end(),
            [](const Relocation& a, const Relocation& b) { return a.offset < b.offset; });
    }

    return elf;
}

} // namespace Decomp2Gecko
