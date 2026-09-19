#include "Decomp2Gecko/layout.h"

#include <unordered_set>

#include "Decomp2Gecko/gecko_cost.h"
#include "Decomp2Gecko/ppc.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

const std::unordered_set<uint32_t> LOAD_OPCODES = {
    32, 34, 40, 42, 46, 48, 50 // lwz lbz lhz lha lmw lfs lfd
};

const std::unordered_set<uint32_t> STORE_OPCODES = {
    36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55 // stw stwu stb stbu sth sthu stmw stfs stfsu stfd stfdu
};

} // namespace

void Layout::scan_stores() {
    // only D-form stores name their target. stores through a pointer are invisible here
    for (Chunk* chunk : chunks_) {
        if (!chunk->is_code()) {
            continue;
        }
        for (const Relocation* relocation : chunk->relocations) {
            if (relocation->reloc_type != R_PPC_ADDR16_LO && relocation->reloc_type != R_PPC_EMB_SDA21) {
                continue;
            }
            int64_t word_offset = relocation->offset & ~int64_t(3);
            if (word_offset + 4 > int64_t(chunk->section->data.size())) {
                continue;
            }
            uint32_t word = read_u32_be(chunk->section->data, size_t(word_offset));
            if (STORE_OPCODES.count(ppc::opcode(word)) == 0) {
                continue;
            }
            if (Chunk* target = relocation_target(*chunk, *relocation).chunk) {
                stored_to_.insert(target);
            }
        }
    }
}

const std::vector<Reference>& Layout::references_to(const Chunk& chunk) const {
    static const std::vector<Reference> none;
    auto found = referrers_.find(&chunk);
    return found == referrers_.end() ? none : found->second;
}

ReferenceKind Layout::reference_kind(const Reference& reference) const {
    // update forms (lwzu & co.) leave the address in the base register, so they're not loads here
    const Chunk& referrer = *reference.referrer;
    if (!referrer.is_code()) {
        return ReferenceKind::DataPointer;
    }
    int64_t word_offset = reference.relocation->offset & ~int64_t(3);
    if (word_offset + 4 > int64_t(referrer.section->data.size())) {
        return ReferenceKind::Other;
    }
    uint32_t opcode = ppc::opcode(read_u32_be(referrer.section->data, size_t(word_offset)));
    int reloc_type = reference.relocation->reloc_type;
    if (reloc_type == R_PPC_ADDR16_LO || reloc_type == R_PPC_ADDR16 || reloc_type == R_PPC_EMB_SDA21) {
        if (LOAD_OPCODES.count(opcode)) {
            return ReferenceKind::Load;
        }
        if (STORE_OPCODES.count(opcode)) {
            return ReferenceKind::Store;
        }
        return ReferenceKind::AddressTaken; // addi, ori & anything else that keeps the address
    }
    if (reloc_type == R_PPC_ADDR16_HA || reloc_type == R_PPC_ADDR16_HI) {
        // a spare @ha leaves the address in a register & anything may follow
        int64_t highs = 0, lows = 0;
        for (const Relocation* other : referrer.relocations) {
            if (other->symbol_index != reference.relocation->symbol_index ||
                other->addend != reference.relocation->addend) {
                continue;
            }
            if (other->reloc_type == R_PPC_ADDR16_LO || other->reloc_type == R_PPC_ADDR16) {
                lows++;
            } else if (other->reloc_type == R_PPC_ADDR16_HA || other->reloc_type == R_PPC_ADDR16_HI) {
                highs++;
            }
        }
        return highs <= lows ? ReferenceKind::Other : ReferenceKind::AddressTaken;
    }
    return ReferenceKind::AddressTaken; // unexpected, assume the worst
}

int64_t fixup_lines(const Reference& reference) {
    if (reference.referrer->action != ChunkAction::Same) {
        return 0;
    }
    int reloc_type = reference.relocation->reloc_type;
    if (reloc_type == R_PPC_ADDR16_LO) {
        return 2; // it & its @ha
    }
    return reloc_type == R_PPC_ADDR16_HA || reloc_type == R_PPC_ADDR16_HI ? 0 : 1;
}

std::optional<std::string> Layout::read_only_reason(const Chunk& chunk) const {
    std::optional<ReadOnly> status = read_only_status(chunk);
    return status ? std::optional<std::string>(status->reason) : std::nullopt;
}

std::optional<Layout::ReadOnly> Layout::read_only_status(const Chunk& chunk) const {
    auto memo = read_only_memo_.find(&chunk);
    if (memo != read_only_memo_.end()) {
        return memo->second;
    }
    if (read_only_in_progress_.count(&chunk)) {
        return std::nullopt; // a cycle of tables pointing at each other proves nothing
    }
    read_only_in_progress_.insert(&chunk);

    std::optional<ReadOnly> status;
    if (stored_to_.count(&chunk) == 0) {
        const ReadOnlyTable* listed = decomp_.game.read_only_table(chunk.unit->name, chunk.name());
        if (listed != nullptr) {
            // audit only covers vanilla code. a changed function could write through what it loads
            bool untouched = true;
            for (const Reference& reference : references_to(chunk)) {
                untouched = untouched && !reference.referrer->changed;
            }
            if (untouched) {
                status = ReadOnly{"listed in the game config: " + listed->reason, true};
            }
        } else {
            const Chunk* via = nullptr;
            bool loaded = false;
            bool clean = true;
            for (const Reference& reference : references_to(chunk)) {
                ReferenceKind kind = reference_kind(reference);
                if (kind == ReferenceKind::Other) {
                    continue;
                }
                if (kind == ReferenceKind::Load) {
                    loaded = true;
                    continue;
                }
                if (kind != ReferenceKind::DataPointer) {
                    clean = false;
                    break;
                }
                const Chunk& table = *reference.referrer;
                bool pure_pointer_table = !table.is_bss() && !table.relocations.empty() && table.size % 4 == 0;
                for (const Relocation* relocation : table.relocations) {
                    pure_pointer_table = pure_pointer_table && relocation->reloc_type == R_PPC_ADDR32;
                }
                std::optional<ReadOnly> table_status = pure_pointer_table ? read_only_status(table) : std::nullopt;
                if (!table_status || !table_status->vouched) {
                    clean = false;
                    break;
                }
                via = &table;
            }
            if (clean) {
                if (!loaded && via == nullptr) {
                    status = ReadOnly{"never referenced", false};
                } else if (via == nullptr) {
                    status = ReadOnly{"only ever loaded", false};
                } else if (loaded) {
                    status = ReadOnly{"only ever loaded, or reached through read-only table " + via->name(), false};
                } else {
                    status = ReadOnly{"referenced only from read-only table " + via->name(), true};
                }
            }
        }
    }

    read_only_in_progress_.erase(&chunk);
    read_only_memo_[&chunk] = status;
    return status;
}

bool Layout::data_alias_eligible(const Chunk& chunk) const {
    if (!options_.alias_named_data || chunk.section->name != ".data" || chunk.symbols.empty() || chunk.is_literal()) {
        return false;
    }
    return chunk.data() && read_only_reason(chunk).has_value();
}

std::vector<Chunk*> Layout::alias_onto_vanilla(const std::vector<Chunk*>& candidates, bool data_pass) {
    // code, .rodata & literals never change. named .data only when proven read-only
    std::unordered_map<int64_t, std::vector<const VanillaSymbol*>> vanilla_by_size;
    for (const VanillaSymbol* vanilla_symbol : decomp_.symbols.entries()) {
        if (vanilla_symbol->size && !is_bss_like_section(vanilla_symbol->section)) {
            vanilla_by_size[vanilla_symbol->size].push_back(vanilla_symbol);
        }
    }
    std::unordered_set<const Chunk*> pending(candidates.begin(), candidates.end());

    // never alias onto bytes rewritten in place, or that pending code might still patch
    std::vector<AddressRange> rewritten_ranges;
    for (const Chunk* chunk : chunks_) {
        if (!chunk->kept || !chunk->changed) {
            continue;
        }
        if (chunk->action == ChunkAction::InPlace && chunk->placed_address) {
            rewritten_ranges.emplace_back(*chunk->placed_address, *chunk->placed_address + chunk->size);
        } else if (const PatchPlan* plan = patch_plan(*chunk)) {
            rewritten_ranges.emplace_back(*chunk->vanilla_address, *chunk->vanilla_address + *chunk->vanilla_size);
            if (!plan->island_far && !plan->island.empty()) {
                rewritten_ranges.emplace_back(plan->island_base, plan->island_end());
            }
        } else if (pending.count(chunk) && chunk->vanilla_address && chunk->vanilla_size) {
            rewritten_ranges.emplace_back(*chunk->vanilla_address, *chunk->vanilla_address + *chunk->vanilla_size);
        }
    }
    auto overlaps_rewrite = [&](int64_t address, int64_t size) {
        for (const auto& [start, end] : rewritten_ranges) {
            if (address < end && address + size > start) {
                return true;
            }
        }
        return false;
    };

    auto references_pending = [&](const Chunk& chunk) {
        for (const Relocation* relocation : chunk.relocations) {
            RelocationTarget target = relocation_target(chunk, *relocation);
            if (target.chunk != nullptr && (pending.count(target.chunk) || target.chunk == &chunk)) {
                return true;
            }
        }
        return false;
    };

    // the destination must never change either
    auto destination_ok = [&](const Chunk& candidate, const VanillaSymbol& symbol) {
        int64_t end = symbol.address + candidate.size;
        for (const auto& [avoid_start, avoid_end] : options_.avoid) {
            if (symbol.address < avoid_end && end > avoid_start) {
                return false;
            }
        }
        if (decomp_.game.reserved_hit(symbol.address, end) != nullptr) {
            return false;
        }
        bool word_accessed = candidate.is_code() || !candidate.relocations.empty();
        if (word_accessed && symbol.address % 4 != 0) {
            return false;
        }
        if (candidate.is_code()) {
            return symbol.section == ".text" || symbol.section == ".init";
        }
        if (symbol.section == ".rodata" || symbol.section == ".sdata2") {
            return true;
        }
        auto owner = chunk_of_vanilla_.find(&symbol);
        return owner != chunk_of_vanilla_.end() &&
            (owner->second->is_literal() || read_only_status(*owner->second).has_value());
    };

    // resolved as if it lived at the candidate, so PC-relative branches compare
    auto find_match = [&](Chunk& chunk) -> const VanillaSymbol* {
        std::optional<int64_t> saved_address = chunk.placed_address;
        const VanillaSymbol* match = nullptr;
        auto same_size = vanilla_by_size.find(chunk.size);
        if (same_size != vanilla_by_size.end()) {
            for (const VanillaSymbol* vanilla_symbol : same_size->second) {
                if (!decomp_.dol.contains(vanilla_symbol->address, chunk.size) ||
                    overlaps_rewrite(vanilla_symbol->address, chunk.size) || !destination_ok(chunk, *vanilla_symbol)) {
                    continue;
                }
                chunk.placed_address = vanilla_symbol->address;
                Resolution resolved = resolve_chunk_bytes(chunk, AddressSpace::Placed);
                if (resolved.status == Resolution::Status::Ok &&
                    resolved.bytes == decomp_.dol.read(vanilla_symbol->address, chunk.size)) {
                    match = vanilla_symbol;
                    break;
                }
            }
        }
        chunk.placed_address = saved_address;
        return match;
    };

    // aliasing in-place data trades its rewrite for a fixup in each untouched referrer
    auto alias_lines = [&](const Chunk& chunk) {
        int64_t lines = 0;
        for (const Reference& reference : references_to(chunk)) {
            lines += fixup_lines(reference);
        }
        return lines;
    };
    auto rewrite_lines = [&](const Chunk& chunk) {
        Resolution resolved = resolve_chunk_bytes(chunk, AddressSpace::Placed);
        if (resolved.status != Resolution::Status::Ok) {
            return int64_t(0);
        }
        Bytes vanilla_bytes = decomp_.dol.read(*chunk.vanilla_address, chunk.size);
        std::vector<int64_t> changed;
        for (int64_t offset = 0; offset + 4 <= chunk.size; offset += 4) {
            if (slice(resolved.bytes, offset, offset + 4) != slice(vanilla_bytes, offset, offset + 4)) {
                changed.push_back(*chunk.vanilla_address + offset);
            }
        }
        return estimate_lines(changed);
    };

    std::vector<Chunk*> still_pending;
    std::unordered_map<Bytes, Chunk*> identical;
    for (Chunk* chunk : candidates) {
        bool eligible = data_pass ? data_alias_eligible(*chunk)
                                  : (chunk->is_code() || chunk->section->name == ".rodata" || chunk->is_literal());
        if (chunk->is_bss() || !eligible || references_pending(*chunk)) {
            still_pending.push_back(chunk);
            continue;
        }
        if (const VanillaSymbol* match = find_match(*chunk)) {
            chunk->placed_address = match->address;
            chunk->action = ChunkAction::Alias;
            chunk->notes.push_back(
                "= vanilla " + match->name + (data_pass ? " (data: " + *read_only_reason(*chunk) + ")" : ""));
            pending.erase(chunk);
            stats_.aliased++;
            continue;
        }
        if (data_pass) {
            // two new tables w/ the same bytes only need one copy
            chunk->placed_address = allocator_.start;
            Resolution resolved = resolve_chunk_bytes(*chunk, AddressSpace::Placed);
            chunk->placed_address = std::nullopt;
            if (resolved.status == Resolution::Status::Ok) {
                auto [first, inserted] = identical.emplace(resolved.bytes, chunk);
                if (!inserted && first->second->section->name == chunk->section->name) {
                    chunk->replaced_by = first->second;
                    chunk->kept = false;
                    chunk->action = ChunkAction::Alias;
                    chunk->notes.push_back(
                        "= " + first->second->name() + " (identical; data: " + *read_only_reason(*chunk) + ")");
                    pending.erase(chunk);
                    stats_.aliased++;
                    continue;
                }
            }
        }
        still_pending.push_back(chunk);
    }

    if (data_pass) {
        for (Chunk* chunk : chunks_) {
            if (!chunk->kept || chunk->action != ChunkAction::InPlace || !data_alias_eligible(*chunk) ||
                references_pending(*chunk)) {
                continue;
            }
            const VanillaSymbol* match = find_match(*chunk);
            if (match == nullptr || alias_lines(*chunk) >= rewrite_lines(*chunk)) {
                continue;
            }
            chunk->placed_address = match->address;
            chunk->action = ChunkAction::Alias;
            chunk->notes.push_back("= vanilla " + match->name + " (data: " + *read_only_reason(*chunk) + ")");
            stats_.changed_inplace--;
            stats_.aliased++;
        }
    }
    return still_pending;
}

} // namespace Decomp2Gecko
