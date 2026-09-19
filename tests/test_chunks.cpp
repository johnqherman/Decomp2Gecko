#include "check.h"
#include "Decomp2Gecko/chunks.h"
#include "Decomp2Gecko/reloc.h"

using namespace Decomp2Gecko;

namespace {

// two functions with a non-zero gap, a zero-size label inside the second, and trailing bytes
ElfFile make_object() {
    ElfFile object;
    object.path = "synthetic.o";
    Section null_section;
    null_section.index = 0;
    object.sections.push_back(null_section);

    Section text;
    text.index = 1;
    text.name = ".text";
    text.header_type = SHT_PROGBITS;
    text.flags = SHF_ALLOC | SHF_EXECINSTR;
    text.size = 0x30;
    text.alignment = 4;
    text.data = Bytes(0x30, '\0');
    text.data[0x0C] = 'G'; // in the gap between functions, belongs to the first
    text.data[0x2C] = 'T'; // tail byte after the last symbol
    object.sections.push_back(text);

    Section data;
    data.index = 2;
    data.name = ".data";
    data.header_type = SHT_PROGBITS;
    data.flags = SHF_ALLOC | SHF_WRITE;
    data.size = 0x10;
    data.alignment = 8;
    data.data = Bytes(0x10, '\0');
    data.data[0] = 'L'; // before the first symbol, so this chunk has no owning symbol
    object.sections.push_back(data);

    auto add_symbol = [&](const char* name, int64_t value, int64_t size, int type, int binding, int section_index) {
        Symbol symbol;
        symbol.index = int(object.symbols.size());
        symbol.name = name;
        symbol.value = value;
        symbol.size = size;
        symbol.symbol_type = type;
        symbol.binding = binding;
        symbol.section_index = section_index;
        object.symbols.push_back(symbol);
    };

    add_symbol("", 0, 0, STT_NOTYPE, STB_LOCAL, 0);
    add_symbol("...text.0", 0, 0, STT_NOTYPE, STB_LOCAL, 1);
    add_symbol("second", 0x10, 0x18, STT_FUNC, STB_GLOBAL, 1);
    add_symbol("first", 0x00, 0x08, STT_FUNC, STB_LOCAL, 1);
    add_symbol("label_in_second", 0x14, 0, STT_NOTYPE, STB_LOCAL, 1);
    add_symbol("table", 0x08, 0x08, STT_OBJECT, STB_GLOBAL, 2);
    add_symbol("table_alias", 0x08, 0x04, STT_OBJECT, STB_LOCAL, 2);

    Relocation call;
    call.offset = 0x14;
    call.reloc_type = R_PPC_REL24;
    call.symbol_index = 3;
    object.relocations[1].push_back(call);
    return object;
}

} // namespace

TEST(chunks_split) {
    Unit unit{"synthetic.c", "synthetic", "synthetic.o", false};
    ElfFile object = make_object();
    std::deque<Chunk> storage;
    std::vector<Chunk*> chunks = split_object_into_chunks(unit, object, storage);
    CHECK_EQ(chunks.size(), size_t(4));

    Chunk* first = chunks[0];
    CHECK_EQ(first->name(), std::string("first"));
    CHECK_EQ(first->size, int64_t(0x10)); // absorbed the non-zero gap
    CHECK_EQ(first->note_text(), std::string("+gap"));

    Chunk* second = chunks[1];
    CHECK_EQ(second->name(), std::string("second"));
    CHECK_EQ(second->symbols.size(), size_t(2)); // the label is inside this chunk
    CHECK_EQ(second->size, int64_t(0x20)); // tail byte extends it to section end
    CHECK_EQ(second->note_text(), std::string("+tail"));
    CHECK_EQ(second->relocations.size(), size_t(1));
    CHECK_EQ(second->alignment(), int64_t(8));
    CHECK_EQ(second->label(), std::string("synthetic.c:.text:second"));

    Chunk* leading = chunks[2];
    CHECK(leading->symbols.empty());
    CHECK_EQ(leading->name(), std::string(".data+0x0"));
    CHECK_EQ(leading->size, int64_t(8));

    Chunk* table = chunks[3];
    CHECK_EQ(table->name(), std::string("table")); // bigger symbol wins the tie at offset 8
    CHECK_EQ(table->symbols.size(), size_t(2));
    CHECK_EQ(table->alignment(), int64_t(8));

    ChunkLocator locator(chunks);
    CHECK(locator.containing(1, 0x14) == second);
    CHECK(locator.containing(1, 0x0F) == first);
    CHECK(locator.containing(1, 0x30) == second); // end label of the last chunk
    CHECK(locator.containing(1, 0x31) == nullptr);
    CHECK(locator.containing(2, 0x08) == table);
    CHECK(locator.containing(7, 0) == nullptr);
}

TEST(chunks_relocation_in_zero_gap) {
    // like make_object() but the gap between first & second is all zeroes with a relocation in it
    ElfFile object;
    object.path = "reloc_gap.o";
    Section null_section;
    null_section.index = 0;
    object.sections.push_back(null_section);

    Section text;
    text.index = 1;
    text.name = ".text";
    text.header_type = SHT_PROGBITS;
    text.flags = SHF_ALLOC | SHF_EXECINSTR;
    text.size = 0x20;
    text.alignment = 4;
    text.data = Bytes(0x20, '\0'); // all zeroes, gap at 0x08..0x10 included

    auto add_symbol = [&](const char* name, int64_t value, int64_t size, int type, int binding, int section_index) {
        Symbol symbol;
        symbol.index = int(object.symbols.size());
        symbol.name = name;
        symbol.value = value;
        symbol.size = size;
        symbol.symbol_type = type;
        symbol.binding = binding;
        symbol.section_index = section_index;
        object.symbols.push_back(symbol);
    };

    add_symbol("", 0, 0, STT_NOTYPE, STB_LOCAL, 0);
    add_symbol("alpha", 0x00, 0x08, STT_FUNC, STB_GLOBAL, 1);
    add_symbol("beta", 0x10, 0x10, STT_FUNC, STB_GLOBAL, 1);

    Relocation reloc_in_gap;
    reloc_in_gap.offset = 0x0C;
    reloc_in_gap.reloc_type = R_PPC_ADDR32;
    reloc_in_gap.symbol_index = 2;
    object.relocations[1].push_back(reloc_in_gap);

    object.sections.push_back(text);

    Unit unit{"reloc_gap.c", "reloc_gap", "reloc_gap.o", false};
    std::deque<Chunk> storage;
    std::vector<Chunk*> chunks = split_object_into_chunks(unit, object, storage);
    CHECK_EQ(chunks.size(), size_t(2));

    Chunk* alpha = chunks[0];
    CHECK_EQ(alpha->name(), std::string("alpha"));
    CHECK_EQ(alpha->size, int64_t(0x10));
    CHECK_EQ(alpha->note_text(), std::string("+gap"));
    CHECK_EQ(alpha->relocations.size(), size_t(1));
    CHECK_EQ(alpha->relocations[0]->offset, int64_t(0x0C));

    Chunk* beta = chunks[1];
    CHECK_EQ(beta->name(), std::string("beta"));
    CHECK_EQ(beta->size, int64_t(0x10));
}
