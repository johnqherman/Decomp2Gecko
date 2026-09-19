// a grown function stays put: insertions go to islands entered & left by branches.
// plan_patch() failing = the layout relocates it
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Decomp2Gecko/vanilla.h"

namespace Decomp2Gecko {

struct PatchInput {
    std::vector<uint32_t> vanilla;
    std::vector<uint32_t> fresh;
    std::vector<uint32_t> mask; // relocation field bits, ignored when matching
    std::vector<bool> has_relocation;
    std::vector<bool> pinned; // must keep its vanilla slot
    int64_t vanilla_address = 0;
    int64_t local_slack = 0;
    int64_t far_start = 0;
    int64_t far_end = 0;
};

enum class SlotKind {
    Fresh,
    Hook,
    BranchOver,
    BranchBack,
    InvertHelper,
    Nop,
};

struct Slot {
    SlotKind kind;
    int64_t fresh_index = -1; // the word, or the word a branch slot goes to
};

struct IntraBranch {
    int64_t from;
    int64_t to;
    bool inverted = false; // sense flipped, InvertHelper follows
};

struct Island {
    size_t first_slot;
    int64_t hook_address;
};

struct PatchPlan {
    bool island_far = false;
    int64_t island_base = -1;
    std::vector<Slot> island; // all islands, back to back
    std::vector<Island> islands;
    std::vector<std::pair<int64_t, Slot>> in_place;
    std::vector<int64_t> home; // fresh index -> address, -1 = dropped nop
    std::vector<int64_t> island_slot; // fresh index -> index into island, -1 = in place
    std::vector<int64_t> vanilla_partner; // fresh index -> vanilla word index, -1 = none
    std::vector<IntraBranch> branches;
    std::vector<AddressRange> dead_vanilla;
    int64_t estimated_lines = 0;

    int64_t island_bytes() const { return int64_t(island.size()) * 4; }
    int64_t island_end() const { return island_base + island_bytes(); }
    // a bc in place reaches a far island only at its first slot, through the hook
    bool reaches(int64_t to, bool b_form, bool from_island) const;
    std::optional<int64_t> branch_target(int64_t to, bool b_form, bool from_island) const;
};

struct PatchOutcome {
    std::optional<PatchPlan> plan;
    std::string reason;
};

PatchOutcome plan_patch(const PatchInput& input);

void assign_island_base(PatchPlan& plan, int64_t base);

} // namespace Decomp2Gecko
