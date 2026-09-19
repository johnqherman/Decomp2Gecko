#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Decomp2Gecko/decomp.h"
#include "Decomp2Gecko/elf.h"
#include "Decomp2Gecko/vanilla.h"

namespace Decomp2Gecko {

inline bool is_bss_like_section(std::string_view section_name) {
    return section_name == ".bss" || section_name == ".sbss" || section_name == ".sbss2";
}

enum class ChunkAction {
    Unplaced,
    Same,
    InPlace,
    Patched,
    Relocated,
    New,
    Alias, // identical bytes already in vanilla, refs point there instead
    Duplicate, // second copy of a weak symbol, another chunk is the one we keep
    Dropped,
};

enum class MappingMethod { None, Global, Local, Reference, Content };

struct Chunk {
    const Unit* unit = nullptr;
    const ElfFile* object_file = nullptr;
    const Section* section = nullptr;
    int64_t offset = 0;
    int64_t size = 0;
    std::vector<const Symbol*> symbols; // first one is the chunk's name
    std::vector<const Relocation*> relocations;

    std::optional<int64_t> vanilla_address;
    std::optional<int64_t> vanilla_size;
    const VanillaSymbol* vanilla_symbol = nullptr;
    MappingMethod mapping_method = MappingMethod::None;

    bool changed = false;
    bool kept = true;
    Chunk* replaced_by = nullptr;
    std::optional<int64_t> placed_address;
    ChunkAction action = ChunkAction::Unplaced;
    std::vector<std::string> notes;

    std::string name() const;
    int64_t end() const { return offset + size; }
    std::optional<std::string_view> data() const;
    bool is_bss() const { return section->is_bss(); }
    bool is_code() const { return section->is_code(); }
    bool is_new() const { return !vanilla_address.has_value(); }
    bool is_literal() const;

    // where a moved copy goes: 8 for code, else the existing alignment capped at 32
    int64_t alignment() const;
    std::string label() const;
    std::string note_text() const;
    void map_to_vanilla(const VanillaSymbol* symbol, MappingMethod method);
};

std::vector<Chunk*> split_object_into_chunks(const Unit& unit, const ElfFile& object_file, std::deque<Chunk>& storage);

class ChunkLocator {
public:
    ChunkLocator() = default;
    explicit ChunkLocator(const std::vector<Chunk*>& chunks);
    Chunk* containing(int section_index, int64_t offset) const;

private:
    std::unordered_map<int, std::vector<Chunk*>> by_section_;
};

} // namespace Decomp2Gecko
