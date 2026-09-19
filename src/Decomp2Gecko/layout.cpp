#include "Decomp2Gecko/layout.h"
#include "Decomp2Gecko/chunks_internal.h"

#include <algorithm>

#include "Decomp2Gecko/gecko_cost.h"
#include "Decomp2Gecko/ppc.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

int64_t Layout::Allocator::take(int64_t size, int64_t alignment) {
    int64_t address = (next_free + alignment - 1) & ~(alignment - 1);
    if (address + size > end) {
        throw LayoutError("free region exhausted: need 0x" + hex_upper(size) + " more at 0x" + hex_upper(address, 8) +
            ", region ends 0x" + hex_upper(end, 8));
    }
    next_free = address + size;
    return address;
}

Layout::Layout(const std::filesystem::path& decomp_root, LogFn log)
    : Layout(decomp_root, LayoutOptions{}, std::move(log)) {}

Layout::Layout(const std::filesystem::path& decomp_root, const LayoutOptions& options, LogFn log)
    : decomp_(decomp_root),
      options_(effective_options(options, decomp_)),
      log_(log ? std::move(log) : [](const std::string&) {}),
      allocator_(decomp_.game.free_start, decomp_.game.free_end()) {
    load();
    detect_changes();
    pick_canonical_duplicates();
    scan_stores(); // after duplicates fold, so stores into any copy count
    strip_unreferenced();
    place();
    build_image();
}

LayoutOptions Layout::effective_options(const LayoutOptions& options, const Decomp& decomp) {
    LayoutOptions effective = options;
    if (!decomp.skipped_units.empty()) {
        effective.alias_named_data = false;
    }
    return effective;
}

const MemImage& Layout::image() const {
    if (!image_) {
        throw InternalError("the planned image was never built");
    }
    return *image_;
}

void Layout::load() {
    for (const Unit& unit : decomp_.units) {
        object_storage_.push_back(std::make_unique<ElfFile>(decomp_.load_object(unit)));
        ElfFile* object_file = object_storage_.back().get();
        std::vector<Chunk*> unit_chunks = split_object_into_chunks(unit, *object_file, chunk_storage_);
        locators_[object_file] = ChunkLocator(unit_chunks);
        chunks_.insert(chunks_.end(), unit_chunks.begin(), unit_chunks.end());
    }

    stats_.units = int64_t(decomp_.units.size());
    stats_.chunks = int64_t(chunks_.size());

    for (Chunk* chunk : chunks_) {
        for (const Symbol* symbol : chunk->symbols) {
            if (!symbol->is_global()) {
                continue;
            }
            Layout::GlobalDef definition{chunk, symbol->value - chunk->offset, symbol->binding};
            auto existing = global_definitions_.find(symbol->name);
            if (existing == global_definitions_.end() ||
                (existing->second.binding == STB_WEAK && symbol->binding == STB_GLOBAL)) {
                global_definitions_[symbol->name] = definition;
            } else if (existing->second.binding == STB_GLOBAL && symbol->binding == STB_GLOBAL &&
                existing->second.chunk != chunk) {
                duplicate_globals_.push_back(symbol->name);
            }
        }
    }

    for (Chunk* chunk : chunks_) {
        map_by_name(*chunk, decomp_);
        if (chunk->vanilla_symbol != nullptr &&
            chunk->is_bss() != is_bss_like_section(chunk->vanilla_symbol->section)) {
            chunk->notes.push_back(
                "section-kind changed (" + chunk->vanilla_symbol->section + "->" + chunk->section->name + ")");
            chunk->vanilla_address = std::nullopt;
            chunk->vanilla_size = std::nullopt;
            chunk->vanilla_symbol = nullptr;
            chunk->mapping_method = MappingMethod::None;
        }
    }

    ClaimedSymbols claimed;
    for (Chunk* chunk : chunks_) {
        if (chunk->vanilla_symbol != nullptr) {
            claimed.insert(chunk->vanilla_symbol);
        }
    }

    // each round's mappings let the next decode more references
    for (const std::unique_ptr<ElfFile>& object_file : object_storage_) {
        std::vector<Chunk*> object_chunks;
        for (Chunk* chunk : chunks_) {
            if (chunk->object_file == object_file.get()) {
                object_chunks.push_back(chunk);
            }
        }
        while (map_through_references(object_chunks, locators_[object_file.get()], decomp_, claimed)) {
        }
    }

    for (Chunk* chunk : chunks_) {
        if (!chunk->vanilla_address) {
            map_by_content(*chunk, decomp_, claimed);
        }
    }

    log_("loaded " + std::to_string(stats_.units) + " units, " + std::to_string(stats_.chunks) + " chunks, " +
        std::to_string(global_definitions_.size()) + " globals");
    if (!duplicate_globals_.empty()) {
        std::set<std::string> unique_names(duplicate_globals_.begin(), duplicate_globals_.end());
        std::vector<std::string> listed;
        for (const std::string& name : unique_names) {
            if (listed.size() >= 20) {
                break;
            }
            listed.push_back(name);
        }
        stats_.warnings.push_back("duplicate global definitions: " + join(listed, ", "));
    }
}

RelocationTarget Layout::relocation_target(
    const Chunk& chunk, const Relocation& relocation, bool follow_replacements) const {
    const Symbol& symbol = chunk.object_file->symbols[size_t(relocation.symbol_index)];
    if (symbol.is_defined()) {
        int64_t effective_offset = symbol.value + relocation.addend;
        Chunk* target = locators_.at(chunk.object_file).containing(symbol.section_index, effective_offset);
        if (target == nullptr) {
            return {nullptr, 0, 0, symbol.name + "+" + hex_prefixed(relocation.addend) + "(no chunk)"};
        }
        int64_t offset_in_chunk = effective_offset - target->offset;
        while (follow_replacements && target->replaced_by != nullptr) {
            target = target->replaced_by;
        }
        return {target, offset_in_chunk, 0, std::nullopt};
    }
    if (symbol.section_index == SHN_ABS || symbol.section_index == SHN_COMMON) {
        return {nullptr, symbol.value, relocation.addend, std::nullopt};
    }
    auto definition = global_definitions_.find(symbol.name);
    if (definition != global_definitions_.end()) {
        Chunk* target = definition->second.chunk;
        while (follow_replacements && target->replaced_by != nullptr) {
            target = target->replaced_by;
        }
        return {target, definition->second.offset_in_chunk, relocation.addend, std::nullopt};
    }
    return {nullptr, 0, relocation.addend, symbol.name};
}

TargetAddress Layout::target_address(const Chunk& chunk, const Relocation& relocation, AddressSpace space) const {
    const Symbol& symbol = chunk.object_file->symbols[size_t(relocation.symbol_index)];
    RelocationTarget target = relocation_target(chunk, relocation);
    if (target.chunk != nullptr) {
        if (space == AddressSpace::Vanilla) {
            if (!target.chunk->vanilla_address) {
                return {std::nullopt, target.chunk, std::nullopt};
            }
            return {
                *target.chunk->vanilla_address + target.offset_in_chunk + target.addend, target.chunk, std::nullopt};
        }
        std::optional<int64_t> address = placed_address_of(*target.chunk, target.offset_in_chunk + target.addend);
        if (!address) {
            return {std::nullopt, target.chunk, std::nullopt};
        }
        return {*address, target.chunk, std::nullopt};
    }
    if (!target.unresolved_name) {
        return {target.offset_in_chunk + target.addend, nullptr, std::nullopt};
    }
    if (symbol.is_undefined()) {
        if (const int64_t* linker_value = decomp_.game.linker_symbols.find(symbol.name)) {
            return {*linker_value + target.addend, nullptr, std::nullopt};
        }
        const std::vector<const VanillaSymbol*>& vanilla_candidates = decomp_.symbols.by_name(symbol.name);
        if (vanilla_candidates.size() == 1) {
            return {vanilla_candidates[0]->address + target.addend, nullptr, std::nullopt};
        }
    }
    return {std::nullopt, nullptr, target.unresolved_name};
}

std::optional<SmallDataBase> Layout::small_data_base_for(int64_t target) const {
    std::optional<int> base_register = decomp_.small_data_register(target);
    if (base_register == 13) {
        return SmallDataBase{13, decomp_.sda_base()};
    }
    if (base_register == 2) {
        return SmallDataBase{2, decomp_.sda2_base()};
    }
    return std::nullopt;
}

Resolution Layout::resolve_chunk_bytes(const Chunk& chunk, AddressSpace space) const {
    Resolution resolution;
    std::optional<std::string_view> compiled = chunk.data();
    if (!compiled) {
        throw InternalError(chunk.label() + " has no bytes");
    }
    std::optional<int64_t> chunk_address =
        space == AddressSpace::Vanilla ? chunk.vanilla_address : chunk.placed_address;
    if (!chunk_address) {
        throw InternalError(chunk.label() + " has no address in the requested space");
    }
    const PatchPlan* plan = space == AddressSpace::Placed ? patch_plan(chunk) : nullptr;

    resolution.bytes = Bytes(*compiled);
    auto fail = [&](Resolution::Status status, std::string problem) {
        resolution.status = status;
        resolution.problem = std::move(problem);
        return resolution;
    };

    for (const Relocation* relocation : chunk.relocations) {
        int64_t field_offset = relocation->offset - chunk.offset;
        int64_t word = field_offset / 4;
        int64_t site_address = *chunk_address + (field_offset & ~int64_t(3));
        if (plan != nullptr) {
            if (word >= int64_t(plan->home.size()) || plan->home[size_t(word)] < 0) {
                return fail(Resolution::Status::RelocationFailed,
                    chunk.label() + ": relocation at +0x" + hex_upper(field_offset) + " sits in a dropped word");
            }
            site_address = plan->home[size_t(word)];
        }

        TargetAddress target = target_address(chunk, *relocation, space);
        if (!target.address) {
            if (!target.unresolved_name) {
                resolution.unplaced_target = target.chunk;
                return fail(Resolution::Status::TargetHasNoAddress,
                    chunk.label() + " references " + (target.chunk ? target.chunk->label() : "?") +
                        " which has no placement");
            }

            std::optional<std::pair<int64_t, int64_t>> span =
                relocation_field_span(relocation->reloc_type, field_offset);
            if (!span) {
                return fail(Resolution::Status::RelocationFailed,
                    "unsupported relocation type " + std::to_string(relocation->reloc_type));
            }
            auto [field_start, field_width] = *span;
            // same offset only while the chunk kept its shape; plan_patches never patches one of these
            bool same_shape = plan == nullptr && chunk.vanilla_address && chunk.vanilla_size == chunk.size;
            if (!same_shape) {
                return fail(Resolution::Status::UnknownSymbolWithoutVanillaBytes,
                    chunk.label() + " references unknown symbol " + *target.unresolved_name +
                        " and changed shape, so vanilla can't supply the field; add it to the game config's linker symbols");
            }
            int64_t vanilla_field = *chunk.vanilla_address + field_start;
            bool moved = *chunk_address != *chunk.vanilla_address;
            if (!decomp_.dol.contains(vanilla_field, field_width)) {
                return fail(Resolution::Status::UnknownSymbolWithoutVanillaBytes,
                    chunk.label() + " references unknown symbol " + *target.unresolved_name +
                        " and has no vanilla bytes");
            }
            bool is_pc_relative = relocation->reloc_type == R_PPC_REL24 || relocation->reloc_type == R_PPC_REL14;
            if (space != AddressSpace::Vanilla && moved && is_pc_relative) {
                return fail(Resolution::Status::RelocatedBranchToUnknownSymbol,
                    chunk.label() + " was relocated but branches to unknown symbol " + *target.unresolved_name +
                        "; add it to the game config's linker symbols");
            }

            overwrite(resolution.bytes, field_start, decomp_.dol.read(vanilla_field, field_width));
            resolution.unresolved.push_back(*target.unresolved_name);
            continue;
        }

        // addend's already folded into the address, so pass 0 here
        std::optional<std::string> failure =
            apply_relocation(resolution.bytes, field_offset, relocation->reloc_type, *target.address, 0, site_address,
                [this](int64_t target_value) { return small_data_base_for(target_value); });
        if (failure) {
            return fail(Resolution::Status::RelocationFailed, *failure);
        }
    }

    return resolution;
}

void Layout::detect_changes() {
    for (Chunk* chunk : chunks_) {
        if (!chunk->vanilla_address) {
            chunk->changed = true;
            continue;
        }
        if (chunk->vanilla_size == 0) {
            // labels in hand-written asm have size 0 in symbols.txt
            chunk->vanilla_size = chunk->size;
        }

        if (chunk->is_bss()) {
            chunk->changed = chunk->vanilla_size != chunk->size;
            continue;
        }
        if (chunk->vanilla_size != chunk->size) {
            chunk->changed = true;
            continue;
        }
        if (!decomp_.dol.contains(*chunk->vanilla_address, chunk->size)) {
            chunk->changed = true;
            chunk->notes.push_back("vanilla-not-in-dol");
            continue;
        }

        Resolution resolved = resolve_chunk_bytes(*chunk, AddressSpace::Vanilla);
        if (resolved.status == Resolution::Status::TargetHasNoAddress) {
            chunk->changed = true;
            continue;
        }
        if (resolved.status == Resolution::Status::RelocationFailed) {
            chunk->changed = true;
            chunk->notes.push_back("reloc:" + resolved.problem);
            continue;
        }
        if (resolved.status != Resolution::Status::Ok) {
            throw LayoutError(resolved.problem);
        }

        stats_.unresolved_names.insert(resolved.unresolved.begin(), resolved.unresolved.end());
        if (!resolved.unresolved.empty()) {
            chunk->changed = true;
            chunk->notes.push_back("unresolved-symbol");
            continue;
        }
        chunk->changed = resolved.bytes != decomp_.dol.read(*chunk->vanilla_address, chunk->size);
    }
}

void Layout::pick_canonical_duplicates() {
    // a unique strong definition wins (linker semantics), else the vanilla-matching copy, else the first
    OrderedMap<std::string, std::vector<Layout::GlobalDef>> definitions;
    for (Chunk* chunk : chunks_) {
        for (const Symbol* symbol : chunk->symbols) {
            if (symbol->is_global()) {
                definitions[symbol->name].push_back({chunk, symbol->value - chunk->offset, symbol->binding});
            }
        }
    }

    for (auto& [name, copies] : definitions) {
        if (copies.size() < 2) {
            continue;
        }
        const Layout::GlobalDef* canonical = nullptr;

        int strong_count = 0;
        const Layout::GlobalDef* strong_copy = nullptr;
        for (const Layout::GlobalDef& copy : copies) {
            if (copy.binding == STB_GLOBAL) {
                strong_count++;
                if (strong_count == 1) {
                    strong_copy = &copy;
                }
            }
        }

        if (strong_count == 1) {
            canonical = strong_copy;
        } else {
            for (const Layout::GlobalDef& copy : copies) {
                if (!copy.chunk->changed && copy.chunk->vanilla_address) {
                    canonical = &copy;
                    break;
                }
            }
            if (canonical == nullptr) {
                canonical = &copies[0];
            }
        }

        global_definitions_[name] = *canonical;
        for (Layout::GlobalDef& copy : copies) {
            if (copy.chunk != canonical->chunk && copy.chunk->replaced_by == nullptr) {
                copy.chunk->replaced_by = canonical->chunk;
                copy.chunk->kept = false;
                copy.chunk->action = ChunkAction::Duplicate;
            }
        }
    }

    // the kept copy owns the vanilla symbol; a dropped duplicate has no stores or refs to judge by
    for (Chunk* chunk : chunks_) {
        if (chunk->vanilla_symbol != nullptr && chunk->replaced_by == nullptr) {
            chunk_of_vanilla_.emplace(chunk->vanilla_symbol, chunk);
        }
    }
}

void Layout::strip_unreferenced() {
    // roots: everything vanilla has, the retail linker proved those reachable
    std::unordered_set<const Chunk*> reachable;
    std::vector<Chunk*> worklist;
    for (Chunk* chunk : chunks_) {
        if (chunk->vanilla_address && chunk->replaced_by == nullptr) {
            reachable.insert(chunk);
            worklist.push_back(chunk);
        }
    }

    while (!worklist.empty()) {
        Chunk* chunk = worklist.back();
        worklist.pop_back();
        for (const Relocation* relocation : chunk->relocations) {
            Chunk* target = relocation_target(*chunk, *relocation).chunk;
            if (target != nullptr && reachable.insert(target).second) {
                worklist.push_back(target);
            }
        }
    }

    for (Chunk* chunk : chunks_) {
        chunk->kept = reachable.count(chunk) != 0;
        if (!chunk->kept) {
            if (chunk->action != ChunkAction::Duplicate) {
                chunk->action = ChunkAction::Dropped;
            }
            stats_.dropped++;
        }
    }
}

void Layout::place() {
    std::unordered_set<std::string> small_data_sections(
        decomp_.game.small_data_sections.begin(), decomp_.game.small_data_sections.end());
    small_data_sections.insert(decomp_.game.small_data2_sections.begin(), decomp_.game.small_data2_sections.end());

    for (Chunk* chunk : chunks_) {
        if (!chunk->kept) {
            continue;
        }
        for (const Relocation* relocation : chunk->relocations) {
            Chunk* target = relocation_target(*chunk, *relocation).chunk;
            if (target != nullptr && target != chunk) {
                referrers_[target].push_back({chunk, relocation});
            }
        }
    }

    std::vector<Chunk*> candidates;
    std::vector<std::string> placement_errors;
    for (Chunk* chunk : chunks_) {
        if (!chunk->kept) {
            continue;
        }
        bool is_small_data = small_data_sections.count(chunk->section->name) != 0;
        if (!chunk->changed) {
            chunk->placed_address = chunk->vanilla_address;
            chunk->action = ChunkAction::Same;
            stats_.unchanged++;
        } else if (chunk->is_new()) {
            if (is_small_data) {
                placement_errors.push_back(chunk->label() + ": new small-data (" + chunk->section->name + ", 0x" +
                    hex_upper(chunk->size) + " bytes) cannot be relocated");
                continue;
            }
            candidates.push_back(chunk);
        } else {
            if (!chunk->vanilla_size || !chunk->vanilla_address) {
                throw InternalError("changed chunk without vanilla extent");
            }
            if (chunk->size <= *chunk->vanilla_size) {
                chunk->placed_address = chunk->vanilla_address;
                chunk->action = ChunkAction::InPlace;
                stats_.changed_inplace++;
                const ReservedRange* reserved =
                    decomp_.game.reserved_hit(*chunk->placed_address, *chunk->placed_address + chunk->size);
                if (reserved) {
                    placement_errors.push_back(
                        chunk->label() + ": in-place write into reserved range (" + reserved->reason + ")");
                }
            } else if (is_small_data) {
                // r13/r2 offsets = 16-bit, nothing outside the vanilla small-data area is reachable
                placement_errors.push_back(chunk->label() + ": small-data grew 0x" + hex_upper(*chunk->vanilla_size) +
                    " -> 0x" + hex_upper(chunk->size) + "; cannot relocate " + chunk->section->name);
            } else {
                candidates.push_back(chunk);
            }
        }
    }
    errors_ = placement_errors;

    // changed but unreferenced: relocating it would only waste lines
    for (Chunk* chunk : std::vector<Chunk*>(candidates)) {
        if (!chunk->is_new() && referrers_.count(chunk) == 0) {
            chunk->kept = false;
            chunk->action = ChunkAction::Dropped;
            chunk->notes.push_back("changed but unreferenced");
            stats_.dropped++;
            candidates.erase(std::find(candidates.begin(), candidates.end(), chunk));
        }
    }

    // highest address first, so each knows its followers' fate
    {
        std::unordered_set<const Chunk*> pending(candidates.begin(), candidates.end());
        std::vector<Chunk*> grown;
        for (Chunk* chunk : candidates) {
            if (!chunk->is_new() && !chunk->is_bss()) {
                grown.push_back(chunk);
            }
        }
        std::stable_sort(grown.begin(), grown.end(),
            [](const Chunk* a, const Chunk* b) { return *a->vanilla_address > *b->vanilla_address; });
        for (Chunk* chunk : grown) {
            if (chunk->is_code() && options_.patch_in_place) {
                continue; // plan_patches weighs growing in place against an island
            }
            int64_t capacity = capacity_of(*chunk, pending);
            int64_t end = *chunk->vanilla_address + chunk->size;
            if (chunk->size > capacity || decomp_.game.reserved_hit(*chunk->vanilla_address, end) != nullptr) {
                continue;
            }
            chunk->placed_address = chunk->vanilla_address;
            chunk->action = ChunkAction::InPlace;
            chunk->notes.push_back("grew into 0x" + hex_upper(capacity - *chunk->vanilla_size) + " of slack");
            stats_.changed_inplace++;
            pending.erase(chunk);
            candidates.erase(std::find(candidates.begin(), candidates.end(), chunk));
        }
    }

    candidates = alias_onto_vanilla(candidates, false);
    if (options_.patch_in_place) {
        plan_patches(candidates);
    }
    candidates = alias_onto_vanilla(candidates, true);

    std::stable_sort(candidates.begin(), candidates.end(), [](const Chunk* a, const Chunk* b) {
        if (a->is_bss() != b->is_bss()) {
            return !a->is_bss();
        }
        if (a->is_code() != b->is_code()) {
            return a->is_code();
        }
        if (a->unit->name != b->unit->name) {
            return a->unit->name < b->unit->name;
        }
        return a->offset < b->offset;
    });

    auto allocate = [&](Chunk* chunk) {
        chunk->placed_address = allocator_.take(chunk->size, chunk->alignment());
        chunk->action = chunk->is_new() ? ChunkAction::New : ChunkAction::Relocated;
        if (chunk->is_new()) {
            stats_.new_chunks++;
        } else {
            stats_.relocated++;
        }
    };
    // islands right behind relocated code so their writes merge
    for (Chunk* chunk : candidates) {
        if (chunk->is_code()) {
            allocate(chunk);
        }
    }
    for (Chunk* chunk : chunks_) {
        auto found = patches_.find(chunk);
        if (found != patches_.end() && found->second.island_far && !found->second.island.empty()) {
            assign_island_base(found->second, allocator_.take(found->second.island_bytes(), 4));
        }
    }
    for (Chunk* chunk : candidates) {
        if (!chunk->is_code()) {
            allocate(chunk);
        }
    }
}

void Layout::build_image() {
    MemImage planned = decomp_.dol;
    if (allocator_.used()) {
        planned.add_region(allocator_.start, (allocator_.used() + 31) & ~int64_t(31));
    }

    for (const Chunk* chunk : chunks_) {
        if (!chunk->kept || chunk->is_bss() || chunk->action == ChunkAction::Alias) {
            continue;
        }
        // unplaced chunks are in errors_; skipping them & their referrers shows every error at once
        if (!chunk->placed_address) {
            if (errors_.empty()) {
                throw InternalError(chunk->label() + " has no placement");
            }
            continue;
        }

        Resolution resolved = resolve_chunk_bytes(*chunk, AddressSpace::Placed);
        if (resolved.status == Resolution::Status::TargetHasNoAddress && !errors_.empty()) {
            continue;
        }
        if (resolved.status != Resolution::Status::Ok) {
            throw LayoutError(resolved.problem);
        }
        if (const PatchPlan* plan = patch_plan(*chunk)) {
            scatter_patched(*chunk, *plan, std::move(resolved.bytes), planned);
        } else {
            planned.write(*chunk->placed_address, resolved.bytes);
        }
    }

    image_ = std::move(planned);
}

std::vector<std::string> Layout::report_lines() const {
    std::vector<std::string> lines = {
        "units: " + std::to_string(stats_.units) + "  chunks: " + std::to_string(stats_.chunks) +
            "  unchanged: " + std::to_string(stats_.unchanged) +
            "  in-place: " + std::to_string(stats_.changed_inplace) + "  patched: " + std::to_string(stats_.patched) +
            "  relocated: " + std::to_string(stats_.relocated) + "  new: " + std::to_string(stats_.new_chunks) +
            "  aliased: " + std::to_string(stats_.aliased) + "  dropped: " + std::to_string(stats_.dropped),
        "free region: 0x" + hex_upper(allocator_.start, 8) + " used 0x" + hex_upper(allocator_.used()) + " of 0x" +
            hex_upper(decomp_.game.free_size),
        "",
    };

    std::vector<const Chunk*> changed;
    for (const Chunk* chunk : chunks_) {
        if ((chunk->kept || chunk->action == ChunkAction::Alias) && chunk->changed) {
            changed.push_back(chunk);
        }
    }

    std::stable_sort(changed.begin(), changed.end(), [](const Chunk* a, const Chunk* b) {
        int by_action = std::string_view(action_label(a->action)).compare(action_label(b->action));
        if (by_action != 0) {
            return by_action < 0;
        }
        if (a->unit->name != b->unit->name) {
            return a->unit->name < b->unit->name;
        }
        return a->offset < b->offset;
    });
    for (const Chunk* chunk : changed) {
        std::string vanilla_size = chunk->vanilla_size ? "0x" + hex_upper(*chunk->vanilla_size) : "-";
        std::string vanilla_address = chunk->vanilla_address ? hex_upper(*chunk->vanilla_address, 8) : "--------";
        std::string placed_address = chunk->placed_address ? hex_upper(*chunk->placed_address, 8) : "--------";
        if (chunk->replaced_by != nullptr && chunk->replaced_by->placed_address) {
            placed_address = hex_upper(*chunk->replaced_by->placed_address, 8);
        }
        lines.push_back(rstrip("  " + pad_right(action_label(chunk->action), 9) + " " +
            pad_right(chunk->unit->name, 45) + " " + pad_right(chunk->section->name, 8) + " " +
            pad_right(chunk->name(), 40) + " van " + vanilla_address + " " + pad_left(vanilla_size, 7) + " -> " +
            placed_address + " 0x" + hex_upper(chunk->size) + " " + chunk->note_text()));
    }

    std::set<std::string> dropped_names;
    for (const Chunk* chunk : chunks_) {
        if (!chunk->kept && chunk->action != ChunkAction::Alias) {
            dropped_names.insert(chunk->name());
        }
    }
    if (!dropped_names.empty()) {
        std::vector<std::string> listed;
        for (const std::string& name : dropped_names) {
            if (listed.size() >= 40) {
                break;
            }
            listed.push_back(name);
        }
        lines.push_back("");
        lines.push_back("dropped (unreferenced, absent from vanilla): " + join(listed, ", "));
    }

    if (!stats_.unresolved_names.empty()) {
        lines.push_back("");
        lines.push_back("linker-provided symbols copied from vanilla: " +
            join(std::vector<std::string>(stats_.unresolved_names.begin(), stats_.unresolved_names.end()), ", "));
    }

    if (!decomp_.skipped_units.empty()) {
        lines.push_back("");
        lines.push_back("skipped units (" + std::to_string(decomp_.skipped_units.size()) +
            " objects not found; no data aliasing, no reuse of vacated space): " + join(decomp_.skipped_units, ", "));
    }
    for (const std::string& warning : stats_.warnings) {
        lines.push_back("warning: " + warning);
    }
    for (const std::string& error : errors_) {
        lines.push_back("ERROR: " + error);
    }
    return lines;
}

} // namespace Decomp2Gecko
