#include "Decomp2Gecko/chunks.h"
#include "Decomp2Gecko/chunks_internal.h"

#include <algorithm>

#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

const char* action_label(ChunkAction action) {
    switch (action) {
        case ChunkAction::Same: return "same";
        case ChunkAction::InPlace: return "inplace";
        case ChunkAction::Patched: return "patched";
        case ChunkAction::Relocated: return "relocated";
        case ChunkAction::New: return "new";
        case ChunkAction::Alias: return "alias";
        case ChunkAction::Duplicate: return "dup";
        case ChunkAction::Dropped: return "dropped";
        case ChunkAction::Unplaced: break;
    }
    return "";
}

std::string Chunk::name() const {
    if (!symbols.empty()) {
        return symbols[0]->name;
    }
    return section->name + "+0x" + hex_upper(offset);
}

std::optional<std::string_view> Chunk::data() const {
    if (section->is_bss()) {
        return std::nullopt;
    }
    return slice(section->data, offset, end());
}

bool Chunk::is_literal() const {
    if (symbols.empty()) {
        return false;
    }
    for (const Symbol* symbol : symbols) {
        if (!starts_with(symbol->name, "@")) {
            return false;
        }
    }
    return true;
}

int64_t Chunk::alignment() const {
    if (is_code()) {
        return 8;
    }
    int64_t power = 32;
    while (power > 8 && offset % power) {
        power /= 2;
    }
    int64_t section_alignment = section->alignment ? section->alignment : 1;
    return std::max({power, section_alignment, int64_t(8)});
}

std::string Chunk::label() const { return unit->name + ":" + section->name + ":" + name(); }

std::string Chunk::note_text() const { return join(notes, " "); }

void Chunk::map_to_vanilla(const VanillaSymbol* symbol, MappingMethod method) {
    vanilla_address = symbol->address;
    vanilla_size = symbol->size;
    vanilla_symbol = symbol;
    mapping_method = method;
}

namespace {

std::vector<const Symbol*> symbols_defining_chunks(const ElfFile& object_file, const Section& section) {
    std::vector<const Symbol*> chosen;
    for (const Symbol& symbol : object_file.symbols) {
        if (symbol.section_index != section.index) {
            continue;
        }
        if (symbol.symbol_type == STT_SECTION || symbol.symbol_type == STT_FILE || symbol.index == 0) {
            continue;
        }

        // MWCC's zero-size "...data.0" alias at a section start isn't an object
        if (symbol.size == 0 && starts_with(symbol.name, "...")) {
            continue;
        }
        chosen.push_back(&symbol);
    }
    std::stable_sort(chosen.begin(), chosen.end(), [](const Symbol* a, const Symbol* b) {
        if (a->value != b->value) {
            return a->value < b->value;
        }
        if (a->size != b->size) {
            return a->size > b->size;
        }
        bool a_local = a->binding == STB_LOCAL, b_local = b->binding == STB_LOCAL;
        if (a_local != b_local) {
            return !a_local;
        }
        return a->index < b->index;
    });
    return chosen;
}

Chunk* new_chunk(std::deque<Chunk>& storage, const Unit& unit, const ElfFile& object_file, const Section& section,
    int64_t offset, int64_t size, const Symbol* symbol) {
    storage.emplace_back();
    Chunk* chunk = &storage.back();
    chunk->unit = &unit;
    chunk->object_file = &object_file;
    chunk->section = &section;
    chunk->offset = offset;
    chunk->size = size;
    if (symbol) {
        chunk->symbols.push_back(symbol);
    }
    return chunk;
}

static bool has_relocation_in_range(const std::vector<Relocation>& relocations, int64_t start, int64_t end) {
    for (const Relocation& reloc : relocations) {
        if (reloc.offset >= end) {
            break;
        }
        if (reloc.offset >= start) {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<Chunk*> split_object_into_chunks(const Unit& unit, const ElfFile& object_file, std::deque<Chunk>& storage) {
    std::vector<Chunk*> chunks;
    for (const Section* section : object_file.allocated_sections()) {
        const std::vector<Relocation>& relocations = object_file.relocations_for(section->index);
        std::vector<Chunk*> section_chunks;
        Chunk* current = nullptr;

        for (const Symbol* symbol : symbols_defining_chunks(object_file, *section)) {
            if (current != nullptr && symbol->value < current->end()) {
                current->symbols.push_back(symbol);
                current->size = std::max(current->size, symbol->value + symbol->size - current->offset);
                continue;
            }
            if (symbol->size == 0 && current != nullptr && symbol->value == current->end()) {
                continue; // trailing label, nothing to place
            }
            if (current != nullptr && symbol->value > current->end() && !section->is_bss()) {
                // non-zero bytes between symbols belong to the earlier one, zero bytes are padding
                if (any_nonzero(slice(section->data, current->end(), symbol->value)) ||
                    has_relocation_in_range(relocations, current->end(), symbol->value)) {
                    current->size = symbol->value - current->offset;
                    current->notes.push_back("+gap");
                }
            }
            current = new_chunk(storage, unit, object_file, *section, symbol->value, symbol->size, symbol);
            section_chunks.push_back(current);
        }

        for (size_t index = 0; index < section_chunks.size(); index++) {
            Chunk* chunk = section_chunks[index];
            if (chunk->size == 0) {
                int64_t next_start =
                    index + 1 < section_chunks.size() ? section_chunks[index + 1]->offset : section->size;
                chunk->size = next_start - chunk->offset;
            }
        }

        if (section_chunks.empty() && section->size) {
            section_chunks.push_back(new_chunk(storage, unit, object_file, *section, 0, section->size, nullptr));
        } else if (!section_chunks.empty() && section_chunks[0]->offset > 0 && !section->is_bss()) {
            if (any_nonzero(slice(section->data, 0, section_chunks[0]->offset)) ||
                has_relocation_in_range(relocations, 0, section_chunks[0]->offset)) {
                Chunk* leading = new_chunk(storage, unit, object_file, *section, 0, section_chunks[0]->offset, nullptr);
                section_chunks.insert(section_chunks.begin(), leading);
            }
        }

        if (!section_chunks.empty() && !section->is_bss() && section_chunks.back()->end() < section->size) {
            if (any_nonzero(slice(section->data, section_chunks.back()->end(), int64_t(section->data.size()))) ||
                has_relocation_in_range(relocations, section_chunks.back()->end(), section->size)) {
                Chunk* last = section_chunks.back();
                last->size = section->size - last->offset;
                last->notes.push_back("+tail");
            }
        }

        size_t next_relocation = 0;
        for (Chunk* chunk : section_chunks) {
            while (next_relocation < relocations.size() && relocations[next_relocation].offset < chunk->offset) {
                throw InternalError("relocation at offset 0x" + hex_upper(relocations[next_relocation].offset) +
                    " in " + unit.name + " " + section->name + " falls outside any chunk");
            }
            while (next_relocation < relocations.size() && relocations[next_relocation].offset < chunk->end()) {
                chunk->relocations.push_back(&relocations[next_relocation]);
                next_relocation++;
            }
        }

        chunks.insert(chunks.end(), section_chunks.begin(), section_chunks.end());
    }

    return chunks;
}

ChunkLocator::ChunkLocator(const std::vector<Chunk*>& chunks) {
    for (Chunk* chunk : chunks) {
        by_section_[chunk->section->index].push_back(chunk);
    }
    for (auto& [section_index, chunk_list] : by_section_) {
        std::stable_sort(
            chunk_list.begin(), chunk_list.end(), [](const Chunk* a, const Chunk* b) { return a->offset < b->offset; });
    }
}

Chunk* ChunkLocator::containing(int section_index, int64_t offset) const {
    auto found = by_section_.find(section_index);
    if (found == by_section_.end() || found->second.empty()) {
        return nullptr;
    }
    const std::vector<Chunk*>& chunk_list = found->second;

    auto after = std::upper_bound(chunk_list.begin(), chunk_list.end(), offset,
        [](int64_t value, const Chunk* chunk) { return value < chunk->offset; });

    if (after == chunk_list.begin()) {
        return nullptr;
    }
    Chunk* chunk = *(after - 1);
    if (chunk->offset <= offset && offset < std::max(chunk->end(), chunk->offset + 1)) {
        return chunk;
    }
    if (offset == chunk->end()) { // end label of the last chunk in a section
        return chunk;
    }
    return nullptr;
}

bool map_by_name(Chunk& chunk, const Decomp& decomp) {
    const Symbols& symbols = decomp.symbols;
    std::optional<AddressRange> unit_range;
    if (chunk.unit->in_vanilla) {
        unit_range = decomp.splits.unit_range(chunk.unit->name, chunk.section->name);
    }

    if (chunk.is_literal()) { // "@123" style names get renumbered per compile, never trust them
        return false;
    }

    for (const Symbol* symbol : chunk.symbols) {
        if (!symbol->is_global()) {
            continue;
        }
        const std::vector<const VanillaSymbol*>& candidates = symbols.by_name(symbol->name);
        if (candidates.size() == 1) {
            chunk.map_to_vanilla(candidates[0], MappingMethod::Global);
            return true;
        }

        if (!candidates.empty() && unit_range) {
            std::vector<const VanillaSymbol*> in_unit;
            for (const VanillaSymbol* candidate : candidates) {
                if (unit_range->first <= candidate->address && candidate->address < unit_range->second) {
                    in_unit.push_back(candidate);
                }
            }
            if (in_unit.size() == 1) {
                chunk.map_to_vanilla(in_unit[0], MappingMethod::Global);
                return true;
            }
        }
    }

    for (const Symbol* symbol : chunk.symbols) {
        if (unit_range) {
            std::vector<const VanillaSymbol*> in_unit =
                symbols.named_in_range(symbol->name, unit_range->first, unit_range->second);
            if (in_unit.size() == 1) {
                chunk.map_to_vanilla(in_unit[0], MappingMethod::Local);
                return true;
            }
            if (!in_unit.empty()) {
                std::vector<const VanillaSymbol*> same_size;
                for (const VanillaSymbol* candidate : in_unit) {
                    if (candidate->size == chunk.size) {
                        same_size.push_back(candidate);
                    }
                }
                if (same_size.size() == 1) {
                    chunk.map_to_vanilla(same_size[0], MappingMethod::Local);
                    return true;
                }
            }
        } else {
            // dtk may merge tiny sections of neighboring units, leaving a unit w/o a range of its own
            std::vector<const VanillaSymbol*> in_section = symbols.named_in_section(symbol->name, chunk.section->name);
            if (in_section.size() == 1) {
                chunk.map_to_vanilla(in_section[0], MappingMethod::Local);
                return true;
            }
        }
    }

    std::optional<std::string_view> compiled = chunk.data();
    if (chunk.symbols.empty() && unit_range && compiled && chunk.relocations.empty()) {
        auto [range_start, range_end] = *unit_range;
        int64_t expected_address = range_start + chunk.offset;
        if (expected_address + chunk.size <= range_end && decomp.dol.contains(expected_address, chunk.size)) {
            if (decomp.dol.read(expected_address, chunk.size) == *compiled) {
                chunk.vanilla_address = expected_address;
                chunk.vanilla_size = chunk.size;
                chunk.mapping_method = MappingMethod::Content;
                return true;
            }
        }
    }

    return false;
}

std::optional<std::pair<int64_t, int>> find_high_half_relocation(const Chunk& chunk, const Relocation& low_half) {
    for (const Relocation* candidate : chunk.relocations) {
        if (candidate->symbol_index == low_half.symbol_index && candidate->addend == low_half.addend &&
            (candidate->reloc_type == R_PPC_ADDR16_HA || candidate->reloc_type == R_PPC_ADDR16_HI)) {
            return std::make_pair(candidate->offset - chunk.offset, candidate->reloc_type);
        }
    }
    return std::nullopt;
}

namespace {

std::optional<int64_t> target_address_from_vanilla_bytes(
    const Chunk& referrer, const Relocation& relocation, const Decomp& decomp) {
    if (!referrer.vanilla_address) {
        throw InternalError("referrer has no vanilla address");
    }
    int64_t offset_in_chunk = relocation.offset - referrer.offset;
    HighHalf high_half;
    if (relocation.reloc_type == R_PPC_ADDR16_LO) {
        std::optional<std::pair<int64_t, int>> partner = find_high_half_relocation(referrer, relocation);
        if (!partner) {
            return std::nullopt;
        }
        high_half = std::make_pair(*referrer.vanilla_address + partner->first, partner->second);
    }
    return decode_relocation_target([&decomp](int64_t address) { return decomp.dol.try_word(address); },
        *referrer.vanilla_address + offset_in_chunk, relocation.reloc_type,
        {{13, decomp.sda_base()}, {2, decomp.sda2_base()}}, high_half);
}

} // namespace

int map_through_references(
    const std::vector<Chunk*>& chunks, const ChunkLocator& locator, const Decomp& decomp, ClaimedSymbols& claimed) {
    // referrers can disagree (a changed one points somewhere new): majority vote per address
    OrderedMap<Chunk*, OrderedMap<int64_t, int>> votes;
    for (const Chunk* referrer : chunks) {
        if (!referrer->vanilla_address || referrer->is_bss()) {
            continue;
        }
        if (!decomp.dol.contains(*referrer->vanilla_address, referrer->size)) {
            continue;
        }

        for (const Relocation* relocation : referrer->relocations) {
            const Symbol& symbol = referrer->object_file->symbols[size_t(relocation->symbol_index)];
            if (!symbol.is_defined()) {
                continue;
            }
            int64_t effective_offset = symbol.value + relocation->addend;
            Chunk* target = locator.containing(symbol.section_index, effective_offset);
            if (target == nullptr || target->vanilla_address) {
                continue;
            }
            std::optional<int64_t> encoded = target_address_from_vanilla_bytes(*referrer, *relocation, decomp);
            if (!encoded) {
                continue;
            }
            int64_t target_start = (*encoded - (effective_offset - target->offset)) & 0xFFFFFFFF;
            votes[target][target_start] += 1;
        }
    }

    int mapped = 0;
    for (auto& [chunk, tally] : votes) {
        std::vector<std::pair<int64_t, int>> ranked(tally.begin(), tally.end());
        std::stable_sort(
            ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

        for (const auto& [address, count] : ranked) {
            std::vector<const VanillaSymbol*> matching;
            for (const VanillaSymbol* symbol : decomp.symbols.starting_at(address)) {
                if (symbol->size == chunk->size && claimed.count(symbol) == 0) {
                    matching.push_back(symbol);
                }
            }
            if (matching.empty()) {
                continue;
            }
            const VanillaSymbol* vanilla_symbol = matching[0];
            if (chunk->is_bss() != is_bss_like_section(vanilla_symbol->section)) {
                continue;
            }
            if (!chunk->is_bss() && decomp.dol.contains(address, chunk->size) && chunk->is_literal() &&
                chunk->relocations.empty()) {
                if (decomp.dol.read(address, chunk->size) != *chunk->data()) {
                    continue; // literal found only via ref must have identical bytes
                }
            }

            chunk->map_to_vanilla(vanilla_symbol, MappingMethod::Reference);
            claimed.insert(vanilla_symbol);
            mapped++;
            break;
        }
    }

    return mapped;
}

bool map_by_content(Chunk& chunk, const Decomp& decomp, ClaimedSymbols& claimed) {
    std::optional<AddressRange> unit_range;
    if (chunk.unit->in_vanilla) {
        unit_range = decomp.splits.unit_range(chunk.unit->name, chunk.section->name);
    }
    std::optional<std::string_view> compiled = chunk.data();
    if (!(chunk.is_literal() && compiled && chunk.relocations.empty() && unit_range && chunk.size)) {
        return false;
    }
    auto [range_start, range_end] = *unit_range;
    std::optional<Bytes> haystack = decomp.dol.try_read(range_start, range_end - range_start);
    if (!haystack) {
        return false;
    }
    size_t position = haystack->find(*compiled);
    while (position != Bytes::npos) {
        std::vector<const VanillaSymbol*> matching;
        for (const VanillaSymbol* symbol : decomp.symbols.starting_at(range_start + int64_t(position))) {
            if (symbol->size == chunk.size && claimed.count(symbol) == 0) {
                matching.push_back(symbol);
            }
        }
        if (!matching.empty()) {
            chunk.map_to_vanilla(matching[0], MappingMethod::Content);
            claimed.insert(matching[0]);
            return true;
        }
        position = haystack->find(*compiled, position + 1);
    }
    return false;
}

} // namespace Decomp2Gecko
