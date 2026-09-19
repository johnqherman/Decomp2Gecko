#include "Decomp2Gecko/layout.h"

#include <algorithm>
#include <unordered_set>

#include "Decomp2Gecko/gecko_cost.h"
#include "Decomp2Gecko/ppc.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

const PatchPlan* Layout::patch_plan(const Chunk& chunk) const {
    auto found = patches_.find(&chunk);
    return found == patches_.end() ? nullptr : &found->second;
}

std::optional<int64_t> Layout::placed_address_of(const Chunk& chunk, int64_t offset) const {
    if (!chunk.placed_address) {
        return std::nullopt;
    }
    const PatchPlan* plan = patch_plan(chunk);
    if (plan == nullptr) {
        return *chunk.placed_address + offset;
    }
    if (offset == 0) {
        return *chunk.placed_address;
    }
    if (offset < 0 || (offset & 3) || offset / 4 >= int64_t(plan->home.size())) {
        return std::nullopt;
    }
    int64_t home = plan->home[size_t(offset / 4)];
    return home >= 0 ? std::optional<int64_t>(home) : std::nullopt;
}

std::vector<Extent> Layout::chunk_extents(const Chunk& chunk) const {
    std::vector<Extent> extents;
    if (!chunk.kept || !chunk.placed_address || chunk.action == ChunkAction::Alias) {
        return extents;
    }
    if (const PatchPlan* plan = patch_plan(chunk)) {
        extents.push_back({{*chunk.vanilla_address, *chunk.vanilla_address + *chunk.vanilla_size}, false});
        if (!plan->island.empty()) {
            extents.push_back({{plan->island_base, plan->island_end()}, plan->island_far});
        }
        return extents;
    }
    bool moved = chunk.action == ChunkAction::New || chunk.action == ChunkAction::Relocated;
    if (chunk.is_bss() && !moved) {
        return extents; // the DOL loader zeroes it
    }
    int64_t size = chunk.is_bss() ? (chunk.size + 3) & ~int64_t(3) : chunk.size;
    extents.push_back({{*chunk.placed_address, *chunk.placed_address + size}, moved});
    return extents;
}

Bytes Layout::logical_bytes(const Chunk& chunk) const {
    const PatchPlan* plan = patch_plan(chunk);
    if (plan == nullptr) {
        return image().read(*chunk.placed_address, chunk.size);
    }
    Bytes bytes;
    for (int64_t home : plan->home) {
        bytes += packed_u32_be(home >= 0 ? image().word(home) : ppc::NOP);
    }
    return bytes;
}

int64_t Layout::capacity_of(const Chunk& chunk, const std::unordered_set<const Chunk*>& pending) const {
    int64_t start = *chunk.vanilla_address;
    int64_t own_end = start + *chunk.vanilla_size;
    if (chunk.vanilla_symbol == nullptr) {
        return own_end - start; // mapped by content, no idea what follows
    }
    const std::string& section = chunk.vanilla_symbol->section;
    int64_t limit = own_end;
    auto section_range = decomp_.splits.section_ranges.find(section);
    if (section_range != decomp_.splits.section_ranges.end()) {
        limit = std::max(own_end, section_range->second.second);
    }
    for (const ReservedRange& reserved : decomp_.game.reserved) {
        if (reserved.start >= own_end && reserved.start < limit) {
            limit = reserved.start;
        }
    }

    // an alias parked on a dead neighbour keeps its bytes alive
    std::vector<AddressRange> alias_ranges;
    for (const Chunk* other : chunks_) {
        if (other->action == ChunkAction::Alias && other->kept && other->placed_address) {
            alias_ranges.emplace_back(*other->placed_address, *other->placed_address + other->size);
        }
    }
    auto aliased = [&](int64_t range_start, int64_t range_end) {
        for (const auto& [alias_start, alias_end] : alias_ranges) {
            if (alias_start < range_end && alias_end > range_start) {
                return true;
            }
        }
        return false;
    };
    // padding has to be zero bytes, anything else is data nobody named
    auto first_nonzero = [&](int64_t range_start, int64_t range_end) -> std::optional<int64_t> {
        std::optional<Bytes> bytes = decomp_.dol.try_read(range_start, range_end - range_start);
        if (!bytes) {
            return range_start;
        }
        for (int64_t offset = 0; offset < range_end - range_start; offset++) {
            if ((*bytes)[size_t(offset)] != '\0') {
                return range_start + offset;
            }
        }
        return std::nullopt;
    };

    // pending code may still be patched in place, so it stays. w/o every object nothing is known dead
    int64_t cursor = own_end;
    const std::vector<const VanillaSymbol*>& entries = decomp_.symbols.entries();
    auto first = std::lower_bound(entries.begin(), entries.end(), own_end,
        [](const VanillaSymbol* symbol, int64_t address) { return symbol->address < address; });
    for (auto it = first; it != entries.end() && (*it)->address < limit; it++) {
        const VanillaSymbol* symbol = *it;
        if (symbol->section != section) {
            limit = symbol->address;
            break;
        }
        if (std::optional<int64_t> nonzero = first_nonzero(cursor, symbol->address)) {
            limit = *nonzero;
            break;
        }
        auto owner = chunk_of_vanilla_.find(symbol);
        bool dead = false;
        int64_t symbol_end = symbol->address + symbol->size;
        if (build_complete() && owner != chunk_of_vanilla_.end()) {
            const Chunk* other = owner->second;
            symbol_end = symbol->address + other->vanilla_size.value_or(symbol->size);
            dead = ((!other->kept && other->action == ChunkAction::Dropped) ||
                       (pending.count(other) != 0 && !other->is_code())) &&
                !aliased(symbol->address, symbol_end);
        }
        if (!dead) {
            limit = symbol->address;
            break;
        }
        cursor = std::max(cursor, symbol_end);
    }
    if (limit > cursor) {
        if (std::optional<int64_t> nonzero = first_nonzero(cursor, limit)) {
            limit = *nonzero;
        }
    }
    return std::max(limit, own_end) - start;
}

int64_t Layout::relocation_lines(const Chunk& chunk) const {
    // the copy plus a fixup in every referrer that would otherwise stay untouched
    int64_t lines = 1 + (chunk.size + 7) / 8;
    for (const Reference& reference : references_to(chunk)) {
        lines += fixup_lines(reference);
    }
    return lines;
}

// relocation fields count as unchanged, like the planner's own estimate
int64_t Layout::grown_in_place_lines(const Chunk& chunk, const PatchInput& input) const {
    std::vector<int64_t> changed;
    for (size_t word = 0; word < input.fresh.size(); word++) {
        int64_t address = *chunk.vanilla_address + int64_t(word) * 4;
        uint32_t keep = ~input.mask[word];
        if ((input.fresh[word] & keep) != (decomp_.dol.word(address) & keep)) {
            changed.push_back(address);
        }
    }
    return estimate_lines(changed);
}

void Layout::plan_patches(std::vector<Chunk*>& candidates) {
    std::unordered_set<const Chunk*> pending(candidates.begin(), candidates.end());
    std::vector<Chunk*> still_pending;
    for (Chunk* chunk : candidates) {
        bool plannable = chunk->is_code() && !chunk->is_new() && chunk->vanilla_size && *chunk->vanilla_size > 0 &&
            (chunk->size & 3) == 0 && (*chunk->vanilla_size & 3) == 0 &&
            decomp_.dol.contains(*chunk->vanilla_address, *chunk->vanilla_size) && chunk->data();
        if (!plannable) {
            still_pending.push_back(chunk);
            continue;
        }

        PatchInput input;
        input.vanilla_address = *chunk->vanilla_address;
        Bytes vanilla_bytes = decomp_.dol.read(*chunk->vanilla_address, *chunk->vanilla_size);
        for (int64_t offset = 0; offset < *chunk->vanilla_size; offset += 4) {
            input.vanilla.push_back(read_u32_be(vanilla_bytes, size_t(offset)));
        }
        Bytes fresh_bytes(*chunk->data());
        int64_t words = chunk->size / 4;
        for (int64_t offset = 0; offset < chunk->size; offset += 4) {
            input.fresh.push_back(read_u32_be(fresh_bytes, size_t(offset)));
        }
        input.mask.assign(size_t(words), 0);
        input.has_relocation.assign(size_t(words), false);
        input.pinned.assign(size_t(words), false);
        std::string problem;
        for (const Relocation* relocation : chunk->relocations) {
            int64_t field_offset = relocation->offset - chunk->offset;
            std::optional<uint32_t> mask = word_mask_for_relocation(relocation->reloc_type, field_offset);
            if (!mask) {
                problem = "unsupported relocation type " + std::to_string(relocation->reloc_type);
                break;
            }
            size_t word = size_t(field_offset / 4);
            input.mask[word] |= *mask;
            input.has_relocation[word] = true;
            // a bc to another symbol has 14 bits of reach, neither end may move
            if (relocation->reloc_type == R_PPC_REL14) {
                input.pinned[word] = true;
                Chunk* target = relocation_target(*chunk, *relocation).chunk;
                if (target != nullptr && target != chunk && pending.count(target) != 0) {
                    problem = "bc to " + target->name() + ", which may move";
                    break;
                }
            }
            // unknown symbols' fields are copied from vanilla by position
            if (std::optional<std::string> name =
                    target_address(*chunk, *relocation, AddressSpace::Placed).unresolved_name) {
                problem = "unresolved symbol " + *name + " keeps vanilla bytes";
                break;
            }
        }
        if (!problem.empty()) {
            chunk->notes.push_back("patch: " + problem);
            still_pending.push_back(chunk);
            continue;
        }
        int64_t capacity = capacity_of(*chunk, pending);
        input.local_slack = capacity - *chunk->vanilla_size;
        input.far_start = allocator_.start;
        input.far_end = allocator_.end;
        int64_t vanilla_end = *chunk->vanilla_address + *chunk->vanilla_size;
        int64_t relocated = relocation_lines(*chunk);

        int64_t grown = INT64_MAX;
        if (chunk->size <= capacity &&
            decomp_.game.reserved_hit(*chunk->vanilla_address, *chunk->vanilla_address + chunk->size) == nullptr) {
            grown = grown_in_place_lines(*chunk, input);
        }

        PatchOutcome outcome = plan_patch(input);
        int64_t patched = INT64_MAX;
        if (!outcome.plan) {
            chunk->notes.push_back("patch: " + outcome.reason);
        } else {
            PatchPlan& plan = *outcome.plan;
            // interior refs need a word that still exists; a bc from outside (REL14) one that stays put
            bool interior_ok = true;
            auto check_interior = [&](const Chunk& referrer, const Relocation& relocation) {
                RelocationTarget target = relocation_target(referrer, relocation);
                int64_t offset = target.offset_in_chunk + target.addend;
                if (target.chunk != chunk || offset == 0) {
                    return;
                }
                bool aligned = offset > 0 && offset < chunk->size && (offset & 3) == 0;
                bool stays = aligned && plan.home[size_t(offset / 4)] >= 0 && plan.island_slot[size_t(offset / 4)] < 0;
                bool moves = aligned && plan.island_slot[size_t(offset / 4)] >= 0;
                bool live = relocation.reloc_type == R_PPC_REL14 ? stays : (stays || moves);
                interior_ok = interior_ok && live;
            };
            for (const Reference& reference : references_to(*chunk)) {
                check_interior(*reference.referrer, *reference.relocation);
            }
            for (const Relocation* relocation : chunk->relocations) {
                check_interior(*chunk, *relocation); // its own labels
            }
            bool reserved = decomp_.game.reserved_hit(*chunk->vanilla_address, vanilla_end) != nullptr ||
                (!plan.island_far && !plan.island.empty() &&
                    decomp_.game.reserved_hit(plan.island_base, plan.island_end()) != nullptr);
            if (!interior_ok || reserved) {
                chunk->notes.push_back(
                    !interior_ok ? "patch: a reference into the function has no home" : "patch: reserved range");
            } else {
                patched = plan.estimated_lines;
            }
        }

        std::string costs = " est grow " + (grown == INT64_MAX ? std::string("-") : std::to_string(grown)) +
            " / patch " + (patched == INT64_MAX ? std::string("-") : std::to_string(patched)) + " / reloc " +
            std::to_string(relocated);
        if (grown <= patched && grown < relocated) {
            chunk->placed_address = chunk->vanilla_address;
            chunk->action = ChunkAction::InPlace;
            chunk->notes.push_back("grew into 0x" + hex_upper(capacity - *chunk->vanilla_size) + " of slack," + costs);
            stats_.changed_inplace++;
        } else if (patched < relocated) {
            chunk->placed_address = chunk->vanilla_address;
            chunk->action = ChunkAction::Patched;
            chunk->notes.push_back("island 0x" + hex_upper(outcome.plan->island_bytes()) +
                (outcome.plan->island_far ? " far," : " local,") + costs);
            stats_.patched++;
            patches_.emplace(chunk, std::move(*outcome.plan));
        } else {
            chunk->notes.push_back("relocated," + costs);
            still_pending.push_back(chunk);
        }
    }
    candidates = still_pending;
}

void Layout::scatter_patched(const Chunk& chunk, const PatchPlan& plan, Bytes resolved, MemImage& planned) const {
    auto fail = [&](const std::string& what) { throw LayoutError(chunk.label() + ": " + what); };
    auto word_at = [&](int64_t index) { return read_u32_be(resolved, size_t(index * 4)); };
    auto set_word = [&](int64_t index, uint32_t word) { write_u32_be(resolved, size_t(index * 4), word); };

    for (const IntraBranch& branch : plan.branches) {
        uint32_t word = word_at(branch.from);
        int64_t from_home = plan.home[size_t(branch.from)];
        std::string label = "branch at +0x" + hex_upper(branch.from * 4);
        if (from_home < 0) {
            fail(label + " has no home");
        }
        if (branch.inverted) {
            std::optional<uint32_t> toggle = ppc::inversion_toggle(word);
            if (!toggle) {
                fail(label + " can't be inverted");
            }
            std::optional<uint32_t> flipped = ppc::retarget(word ^ *toggle, from_home, from_home + 8);
            set_word(branch.from, *flipped);
            continue;
        }
        bool from_island = plan.island_slot[size_t(branch.from)] >= 0;
        std::optional<int64_t> target = plan.branch_target(branch.to, ppc::is_b_form(word), from_island);
        if (!target) {
            fail(label + " has no reachable target");
        }
        std::optional<uint32_t> encoded = ppc::retarget(word, from_home, *target);
        if (!encoded) {
            fail(label + " out of range");
        }
        set_word(branch.from, *encoded);
    }

    auto branch_to = [&](int64_t address, int64_t fresh_index) {
        int64_t home = plan.home[size_t(fresh_index)];
        std::optional<uint32_t> encoded = home >= 0 ? ppc::encode_branch(address, home) : std::nullopt;
        if (!encoded) {
            fail("no branch from " + hex_upper(address, 8) + " to word +0x" + hex_upper(fresh_index * 4));
        }
        return *encoded;
    };
    auto slot_word = [&](int64_t address, const Slot& slot) -> uint32_t {
        switch (slot.kind) {
            case SlotKind::Fresh: return word_at(slot.fresh_index);
            case SlotKind::Nop: return ppc::NOP;
            case SlotKind::Hook:
            case SlotKind::BranchOver:
            case SlotKind::BranchBack:
            case SlotKind::InvertHelper: return branch_to(address, slot.fresh_index);
        }
        return ppc::NOP;
    };
    for (const auto& [address, slot] : plan.in_place) {
        planned.write(address, packed_u32_be(slot_word(address, slot)));
    }
    for (size_t index = 0; index < plan.island.size(); index++) {
        int64_t address = plan.island_base + int64_t(index) * 4;
        planned.write(address, packed_u32_be(slot_word(address, plan.island[index])));
    }
}

} // namespace Decomp2Gecko
