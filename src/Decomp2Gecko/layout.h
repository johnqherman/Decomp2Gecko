#pragma once

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Decomp2Gecko/bytes.h"
#include "Decomp2Gecko/chunks.h"
#include "Decomp2Gecko/decomp.h"
#include "Decomp2Gecko/elf.h"
#include "Decomp2Gecko/errors.h"
#include "Decomp2Gecko/patch.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/vanilla.h"

namespace Decomp2Gecko {

enum class AddressSpace {
    Vanilla, // every symbol at its vanilla address, used to detect changes
    Placed, // every symbol at the address this layout gave it, used for output
};

using LogFn = std::function<void(const std::string&)>;

struct LayoutOptions {
    bool patch_in_place = true;
    bool alias_named_data = true;
    std::vector<AddressRange> avoid; // bytes other codes patch
};

struct RelocationTarget {
    Chunk* chunk;
    int64_t offset_in_chunk; // or the absolute value when chunk is null & name is empty
    int64_t addend;
    std::optional<std::string> unresolved_name;
};

struct TargetAddress {
    std::optional<int64_t> address;
    Chunk* chunk;
    std::optional<std::string> unresolved_name;
};

struct Resolution {
    enum class Status {
        Ok,
        TargetHasNoAddress, // target chunk has no address in the requested space
        RelocationFailed, // value can't be encoded
        UnknownSymbolWithoutVanillaBytes, // unknown symbol & no vanilla bytes to fall back on
        RelocatedBranchToUnknownSymbol, // moved chunk branches to a symbol nobody can place
    };
    Status status = Status::Ok;
    Bytes bytes;
    std::vector<std::string> unresolved;
    std::string problem;
    Chunk* unplaced_target = nullptr;
};

struct Reference {
    const Chunk* referrer;
    const Relocation* relocation;
};

enum class ReferenceKind {
    Load,
    Store,
    AddressTaken,
    DataPointer,
    Other,
};

int64_t fixup_lines(const Reference& reference);

struct Extent {
    AddressRange range;
    bool moved;
};

class Layout {
public:
    explicit Layout(const std::filesystem::path& decomp_root, LogFn log = {});
    Layout(const std::filesystem::path& decomp_root, const LayoutOptions& options, LogFn log = {});
    Layout(const Layout&) = delete;
    Layout& operator=(const Layout&) = delete;

    const Decomp& decomp() const { return decomp_; }
    const LayoutOptions& options() const { return options_; }
    int64_t free_start() const { return allocator_.start; }
    int64_t free_used() const { return allocator_.used(); }
    const std::vector<Chunk*>& chunks() const { return chunks_; }
    const std::vector<std::string>& errors() const { return errors_; }
    const MemImage& image() const;

    const PatchPlan* patch_plan(const Chunk& chunk) const;
    std::optional<int64_t> placed_address_of(const Chunk& chunk, int64_t offset) const;
    std::vector<Extent> chunk_extents(const Chunk& chunk) const;
    Bytes logical_bytes(const Chunk& chunk) const;

    const std::vector<Reference>& references_to(const Chunk& chunk) const;
    ReferenceKind reference_kind(const Reference& reference) const;
    // every reference a load, or a pointer in a table the config vouches for (nothing when unknown)
    std::optional<std::string> read_only_reason(const Chunk& chunk) const;

    // same-object references fold the addend in, so "...data.0 + 0x40" hits the chunk holding it.
    // duplicates resolve to the kept copy unless follow_replacements is off
    RelocationTarget relocation_target(
        const Chunk& chunk, const Relocation& relocation, bool follow_replacements = true) const;
    TargetAddress target_address(const Chunk& chunk, const Relocation& relocation, AddressSpace space) const;
    // unknown symbols (linker-defined, like __ArenaLo) keep the vanilla DOL's field bytes
    Resolution resolve_chunk_bytes(const Chunk& chunk, AddressSpace space) const;

    std::vector<std::string> report_lines() const;

private:
    struct Allocator {
        int64_t start;
        int64_t end;
        int64_t next_free;

        Allocator(int64_t region_start, int64_t region_end)
            : start(region_start),
              end(region_end),
              next_free(region_start) {}
        int64_t take(int64_t size, int64_t alignment);
        int64_t used() const { return next_free - start; }
    };

    struct Stats {
        int64_t units = 0;
        int64_t chunks = 0;
        int64_t unchanged = 0;
        int64_t changed_inplace = 0;
        int64_t patched = 0;
        int64_t relocated = 0;
        int64_t new_chunks = 0;
        int64_t dropped = 0;
        int64_t aliased = 0;
        std::set<std::string> unresolved_names;
        std::vector<std::string> warnings;
    };

    struct GlobalDef {
        Chunk* chunk;
        int64_t offset_in_chunk;
        int binding;
    };

    void load();
    void scan_stores();
    void detect_changes();
    void pick_canonical_duplicates();
    void strip_unreferenced();
    void place();
    void build_image();
    std::optional<SmallDataBase> small_data_base_for(int64_t target_address) const;
    static LayoutOptions effective_options(const LayoutOptions& options, const Decomp& decomp);
    bool build_complete() const { return decomp_.skipped_units.empty(); }

    int64_t capacity_of(const Chunk& chunk, const std::unordered_set<const Chunk*>& pending) const;
    int64_t relocation_lines(const Chunk& chunk) const;
    int64_t grown_in_place_lines(const Chunk& chunk, const PatchInput& input) const;
    bool data_alias_eligible(const Chunk& chunk) const;
    std::vector<Chunk*> alias_onto_vanilla(const std::vector<Chunk*>& candidates, bool data_pass);
    void plan_patches(std::vector<Chunk*>& candidates);
    void scatter_patched(const Chunk& chunk, const PatchPlan& plan, Bytes resolved, MemImage& planned) const;

    const Decomp decomp_;
    const LayoutOptions options_;
    LogFn log_;
    std::vector<std::unique_ptr<ElfFile>> object_storage_;
    std::deque<Chunk> chunk_storage_; // deque: pointers stay valid
    std::vector<Chunk*> chunks_;
    std::unordered_map<const ElfFile*, ChunkLocator> locators_;
    std::unordered_map<std::string, GlobalDef> global_definitions_;
    std::vector<std::string> duplicate_globals_;
    std::unordered_map<const Chunk*, PatchPlan> patches_;
    std::unordered_set<const Chunk*> stored_to_;
    std::unordered_map<const Chunk*, std::vector<Reference>> referrers_;
    // vouched: the config's word also covers every pointer inside it
    struct ReadOnly {
        std::string reason;
        bool vouched;
    };
    std::optional<ReadOnly> read_only_status(const Chunk& chunk) const;
    mutable std::unordered_map<const Chunk*, std::optional<ReadOnly>> read_only_memo_;
    mutable std::unordered_set<const Chunk*> read_only_in_progress_;
    std::unordered_map<const VanillaSymbol*, Chunk*> chunk_of_vanilla_;
    Allocator allocator_;
    Stats stats_;
    std::optional<MemImage> image_;
    std::vector<std::string> errors_;
};

} // namespace Decomp2Gecko
