// the planner on hand-made word lists; every home, slot & line count derived by hand
#include "check.h"

#include <vector>

#include "Decomp2Gecko/patch.h"
#include "Decomp2Gecko/ppc.h"

using namespace Decomp2Gecko;

namespace {

PatchInput input_for(std::vector<uint32_t> vanilla, std::vector<uint32_t> fresh, std::vector<size_t> reloc_words = {},
    std::vector<size_t> pinned_words = {}) {
    PatchInput input;
    input.vanilla = std::move(vanilla);
    input.fresh = std::move(fresh);
    input.mask.assign(input.fresh.size(), 0);
    input.has_relocation.assign(input.fresh.size(), false);
    input.pinned.assign(input.fresh.size(), false);
    for (size_t word : reloc_words) { // every reloc here is a bl: REL24
        input.mask[word] |= ppc::I_FORM_MASK;
        input.has_relocation[word] = true;
    }
    for (size_t word : pinned_words) {
        input.pinned[word] = true;
    }
    input.vanilla_address = 0x80003110;
    input.far_start = 0x81700000;
    input.far_end = 0x81800000;
    return input;
}

size_t count_kind(const PatchPlan& plan, SlotKind kind) {
    size_t count = 0;
    for (const Slot& slot : plan.island) {
        count += slot.kind == kind;
    }
    return count;
}

// mflr / li r3,0 / addi r3,r3,1 / cmpwi r3,5 / bne -8 / bl helper / mtlr / blr
const std::vector<uint32_t> LOOP_VANILLA = {
    0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x4082FFF8, 0x4BFFFFDD, 0x7C0803A6, 0x4E800020};
// same w/ addi r4,r4,1 / stw r4,0(r5) inserted before the bne, which now reaches back 16 bytes
const std::vector<uint32_t> LOOP_FRESH = {0x7C0802A6, 0x38600000, 0x38630001, 0x2C030005, 0x38840001, 0x90850000,
    0x4082FFF0, 0x48000001, 0x7C0803A6, 0x4E800020};

} // namespace

TEST(patch_insertion_moves_a_neighbour_into_the_island) {
    PatchOutcome outcome = plan_patch(input_for(LOOP_VANILLA, LOOP_FRESH, {7}));
    CHECK(outcome.plan.has_value());
    const PatchPlan& plan = *outcome.plan;

    // the cmpwi before the insertion moves (a plain word beats the bne after it): island = cmpwi / addi / stw / b bne
    CHECK(plan.island_far);
    CHECK_EQ(plan.islands.size(), size_t(1));
    CHECK_EQ(plan.islands[0].hook_address, int64_t(0x8000311C));
    CHECK_EQ(plan.island.size(), size_t(4));
    CHECK(plan.island[0].kind == SlotKind::Fresh && plan.island[0].fresh_index == 3);
    CHECK(plan.island[1].kind == SlotKind::Fresh && plan.island[1].fresh_index == 4);
    CHECK(plan.island[2].kind == SlotKind::Fresh && plan.island[2].fresh_index == 5);
    CHECK(plan.island[3].kind == SlotKind::BranchBack && plan.island[3].fresh_index == 6);

    CHECK_EQ(plan.home[0], int64_t(0x80003110));
    CHECK_EQ(plan.home[2], int64_t(0x80003118));
    CHECK_EQ(plan.home[3], int64_t(-1)); // island, not placed yet
    CHECK_EQ(plan.home[6], int64_t(0x80003120));
    CHECK_EQ(plan.home[9], int64_t(0x8000312C));
    CHECK_EQ(plan.vanilla_partner[6], int64_t(4));
    CHECK_EQ(plan.vanilla_partner[4], int64_t(-1));
    CHECK(plan.dead_vanilla.empty());
    CHECK_EQ(plan.branches.size(), size_t(1)); // the bne; the bl has a relocation
    CHECK_EQ(plan.branches[0].from, int64_t(6));
    CHECK_EQ(plan.branches[0].to, int64_t(2));
    CHECK(!plan.branches[0].inverted);
    // one 04 for the hook + a 06 header + 16 island bytes = 1 + 1 + 2
    CHECK_EQ(plan.estimated_lines, int64_t(4));

    PatchPlan placed = plan;
    assign_island_base(placed, 0x81700000);
    CHECK_EQ(placed.home[3], int64_t(0x81700000));
    CHECK_EQ(placed.home[5], int64_t(0x81700008));
    CHECK_EQ(*placed.branch_target(2, true, false), int64_t(0x80003118));
    // a b anywhere reaches the island word, a bc in place only its first slot via the hook
    CHECK_EQ(*placed.branch_target(3, false, false), int64_t(0x81700000));
    CHECK_EQ(*placed.branch_target(3, true, false), int64_t(0x8000311C));
    CHECK(!placed.branch_target(4, true, false).has_value());
    CHECK_EQ(*placed.branch_target(4, true, true), int64_t(0x81700004));
}

TEST(patch_local_slack_keeps_the_island_next_door) {
    PatchInput input = input_for(LOOP_VANILLA, LOOP_FRESH, {7});
    input.local_slack = 0x10;
    PatchOutcome outcome = plan_patch(input);
    CHECK(outcome.plan.has_value());
    CHECK(!outcome.plan->island_far);
    CHECK_EQ(outcome.plan->island_base, int64_t(0x80003130));
    CHECK_EQ(outcome.plan->home[3], int64_t(0x80003130));
    CHECK_EQ(outcome.plan->home[5], int64_t(0x80003138));
    CHECK_EQ(outcome.plan->estimated_lines, int64_t(4));

    input.local_slack = 0xC; // three words, the island needs four
    PatchOutcome cramped = plan_patch(input);
    CHECK(cramped.plan.has_value());
    CHECK(cramped.plan->island_far);
}

TEST(patch_inverts_a_bc_that_leaves_the_island) {
    // mflr / bl / cmpwi r3,0 / beq +8 / li r3,1 / mtlr / blr / nop
    std::vector<uint32_t> vanilla = {
        0x7C0802A6, 0x4BFFFFED, 0x2C030000, 0x41820008, 0x38600001, 0x7C0803A6, 0x4E800020, 0x60000000};
    // cmpwi r4,0 / bne +8 inserted before the li: both bc's now aim at the mtlr
    std::vector<uint32_t> fresh = {0x7C0802A6, 0x48000001, 0x2C030000, 0x41820010, 0x2C040000, 0x40820008, 0x38600001,
        0x7C0803A6, 0x4E800020, 0x60000000};
    PatchOutcome outcome = plan_patch(input_for(vanilla, fresh, {1}));
    CHECK(outcome.plan.has_value());
    const PatchPlan& plan = *outcome.plan;

    // the li after the insertion moves (not a branch), hook in its slot
    CHECK_EQ(plan.islands.size(), size_t(1));
    CHECK_EQ(plan.islands[0].hook_address, int64_t(0x80003120));
    // cmpwi / bne (flipped to beq +8) / b mtlr / li / b mtlr
    CHECK_EQ(plan.island.size(), size_t(5));
    CHECK(plan.island[0].kind == SlotKind::Fresh && plan.island[0].fresh_index == 4);
    CHECK(plan.island[1].kind == SlotKind::Fresh && plan.island[1].fresh_index == 5);
    CHECK(plan.island[2].kind == SlotKind::InvertHelper && plan.island[2].fresh_index == 7);
    CHECK(plan.island[3].kind == SlotKind::Fresh && plan.island[3].fresh_index == 6);
    CHECK(plan.island[4].kind == SlotKind::BranchBack && plan.island[4].fresh_index == 7);
    CHECK_EQ(plan.branches.size(), size_t(2));
    CHECK(!plan.branches[0].inverted); // beq in place, mtlr stays in place
    CHECK(plan.branches[1].inverted);
    CHECK_EQ(plan.island_slot[6], int64_t(3));
    // the trailing nop is padding, its slot is dead now
    CHECK_EQ(plan.home[9], int64_t(-1));
    CHECK_EQ(plan.dead_vanilla.size(), size_t(1));
    CHECK_EQ(plan.dead_vanilla[0].first, int64_t(0x8000312C));
    CHECK_EQ(plan.dead_vanilla[0].second, int64_t(0x80003130));
    // hook + header + 20 bytes
    CHECK_EQ(plan.estimated_lines, int64_t(5));
}

TEST(patch_refuses_what_it_cannot_encode) {
    // bdnzf (BO = 0: counter & condition) inserted into an island w/ an in-place target has no inverse
    std::vector<uint32_t> vanilla = {0x7C0802A6, 0x2C030000, 0x38600001, 0x7C0803A6, 0x4E800020};
    std::vector<uint32_t> fresh = {0x7C0802A6, 0x2C030000, 0x2C040000, 0x40000008, 0x38600001, 0x7C0803A6, 0x4E800020};
    PatchOutcome far = plan_patch(input_for(vanilla, fresh));
    CHECK(!far.plan.has_value());
    CHECK(far.reason.find("can't be inverted") != std::string::npos);
    // next door it reaches without flipping
    PatchInput local = input_for(vanilla, fresh);
    local.local_slack = 0x20;
    CHECK(plan_patch(local).plan.has_value());

    // a pinned word inside the insertion has no vanilla slot to stay in
    PatchOutcome pinned = plan_patch(input_for(LOOP_VANILLA, LOOP_FRESH, {7}, {4}));
    CHECK(!pinned.plan.has_value());
    CHECK(pinned.reason.find("pinned") != std::string::npos);
    // pinned words in matched runs are fine
    CHECK(plan_patch(input_for(LOOP_VANILLA, LOOP_FRESH, {7}, {7})).plan.has_value());

    // a bcl to a label is position-dependent wherever it sits: refused outright
    std::vector<uint32_t> linked = {0x7C0802A6, 0x2C030000, 0x2C040000, 0x40820009, 0x38600001, 0x7C0803A6, 0x4E800020};
    PatchOutcome linked_far = plan_patch(input_for(vanilla, linked));
    CHECK(!linked_far.plan.has_value());
    CHECK(linked_far.reason.find("position-dependent") != std::string::npos);
    PatchInput linked_local = input_for(vanilla, linked);
    linked_local.local_slack = 0x20;
    CHECK(!plan_patch(linked_local).plan.has_value());

    // bl to a label: the mflr after it wants the address the function has in vanilla
    std::vector<uint32_t> positional = LOOP_FRESH;
    positional[2] = 0x48000005; // bl +4
    positional[3] = 0x7C6802A6; // mflr r3
    PatchOutcome pc_idiom = plan_patch(input_for(LOOP_VANILLA, positional, {7}));
    CHECK(!pc_idiom.plan.has_value());
    CHECK(pc_idiom.reason.find("position-dependent") != std::string::npos);

    // a relative branch w/o relocation that leaves the function
    std::vector<uint32_t> escaping = LOOP_FRESH;
    escaping[4] = 0x48000100;
    PatchOutcome escaped = plan_patch(input_for(LOOP_VANILLA, escaping, {7}));
    CHECK(!escaped.plan.has_value());
    CHECK(escaped.reason.find("leaves the function") != std::string::npos);
}

TEST(patch_refuses_huge_functions) {
    // the alignment table is capped; a function this size is relocated as before
    std::vector<uint32_t> big;
    std::vector<uint32_t> bigger;
    for (int word = 0; word < 4100; word++) {
        big.push_back(word == 4099 ? 0x4E800020 : 0x60000000);
        if (word == 2) {
            bigger.push_back(0x38600001);
        }
        bigger.push_back(big.back());
    }
    PatchOutcome outcome = plan_patch(input_for(big, bigger));
    CHECK(!outcome.plan.has_value());
    CHECK(outcome.reason.find("too big") != std::string::npos);
}

TEST(patch_end_of_function_island_has_no_branch_back) {
    // mflr / li / blr / nop  ->  mflr / li / addi / stw / blr
    std::vector<uint32_t> vanilla = {0x7C0802A6, 0x38600000, 0x4E800020, 0x60000000};
    std::vector<uint32_t> fresh = {0x7C0802A6, 0x38600000, 0x38840001, 0x90850000, 0x4E800020};
    PatchOutcome outcome = plan_patch(input_for(vanilla, fresh));
    CHECK(outcome.plan.has_value());
    const PatchPlan& plan = *outcome.plan;
    // blr moves (ties go to the word after), hook in its slot; nothing follows it
    CHECK_EQ(plan.islands[0].hook_address, int64_t(0x80003118));
    CHECK_EQ(plan.island.size(), size_t(3));
    CHECK(plan.island[2].kind == SlotKind::Fresh && plan.island[2].fresh_index == 4);
    CHECK_EQ(count_kind(plan, SlotKind::BranchBack), size_t(0));
    // the vanilla nop slot after the blr is dead, nothing branches over it
    CHECK_EQ(plan.dead_vanilla.size(), size_t(1));
    CHECK_EQ(plan.dead_vanilla[0].first, int64_t(0x8000311C));
}

TEST(patch_absorbs_a_short_run_between_insertions) {
    // A B C D E F  ->  A B X C Y D E F: the lone C between two insertions joins the island
    std::vector<uint32_t> vanilla = {0x7C0802A6, 0x38600000, 0x38630001, 0x38630002, 0x38630003, 0x4E800020};
    std::vector<uint32_t> fresh = {
        0x7C0802A6, 0x38600000, 0x38840001, 0x38630001, 0x38840002, 0x38630002, 0x38630003, 0x4E800020};
    PatchOutcome outcome = plan_patch(input_for(vanilla, fresh));
    CHECK(outcome.plan.has_value());
    const PatchPlan& plan = *outcome.plan;
    CHECK_EQ(plan.islands.size(), size_t(1));
    // the region replaces C's slot: hook there, island X C Y + b back to D
    CHECK_EQ(plan.islands[0].hook_address, int64_t(0x80003118));
    CHECK_EQ(plan.island.size(), size_t(4));
    CHECK(plan.island[3].kind == SlotKind::BranchBack && plan.island[3].fresh_index == 5);
    CHECK(plan.dead_vanilla.empty());
}

TEST(patch_shrink_branches_over_dead_words) {
    // A B C D E blr  ->  A B E blr: two words gone, a b skips their slots
    std::vector<uint32_t> vanilla = {0x7C0802A6, 0x38600000, 0x38630001, 0x38630002, 0x38630003, 0x4E800020};
    std::vector<uint32_t> fresh = {0x7C0802A6, 0x38600000, 0x38630003, 0x4E800020};
    PatchOutcome outcome = plan_patch(input_for(vanilla, fresh));
    CHECK(outcome.plan.has_value());
    const PatchPlan& plan = *outcome.plan;
    CHECK(plan.island.empty());
    CHECK_EQ(plan.in_place.size(), size_t(5)); // A B, b-over, E, blr
    CHECK(plan.in_place[2].second.kind == SlotKind::BranchOver);
    CHECK_EQ(plan.in_place[2].first, int64_t(0x80003118));
    CHECK_EQ(plan.home[2], int64_t(0x80003120));
    CHECK_EQ(plan.dead_vanilla.size(), size_t(1));
    CHECK_EQ(plan.dead_vanilla[0].first, int64_t(0x8000311C));
    CHECK_EQ(plan.estimated_lines, int64_t(1)); // just the b
}

TEST(ppc_branch_helpers) {
    CHECK_EQ(*ppc::relative_displacement(0x4082FFF8), int64_t(-8));
    CHECK_EQ(*ppc::relative_displacement(0x4BFFFFDD), int64_t(-0x24));
    CHECK(!ppc::relative_displacement(0x4E800020).has_value());
    CHECK(ppc::is_unconditional(0x4E800020)); // blr
    CHECK(ppc::is_unconditional(0x48000010)); // b
    CHECK(!ppc::is_unconditional(0x48000011)); // bl
    CHECK(!ppc::is_unconditional(0x41820008)); // beq
    CHECK(ppc::is_unconditional(0x4E800420)); // bctr
    CHECK_EQ(*ppc::encode_branch(0x80003120, 0x81700000), uint32_t(0x496FCEE0));
    CHECK_EQ(*ppc::encode_branch(0x81700010, 0x80003124), uint32_t(0x4A903114));
    CHECK_EQ(*ppc::retarget(0x4082FFF8, 0x80003120, 0x80003118), uint32_t(0x4082FFF8));
    CHECK(!ppc::retarget(0x4082FFF8, 0x80003120, 0x81700000).has_value()); // bc can't reach
    CHECK_EQ(*ppc::inversion_toggle(0x40820008), ppc::BO_CONDITION_SENSE); // bne <-> beq
    CHECK_EQ(*ppc::inversion_toggle(0x42000008), ppc::BO_COUNTER_SENSE); // bdnz <-> bdz
    CHECK(!ppc::inversion_toggle(0x40000008).has_value()); // bdnzf
    CHECK(!ppc::inversion_toggle(0x40820009).has_value()); // bnel
    CHECK(!ppc::inversion_toggle(0x48000008).has_value()); // not a bc
    CHECK(!ppc::inversion_toggle(0x42800008).has_value()); // bc always
}
