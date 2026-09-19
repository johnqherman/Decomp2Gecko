#include "Decomp2Gecko/patch.h"

#include <algorithm>

#include "Decomp2Gecko/gecko_cost.h"
#include "Decomp2Gecko/ppc.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

constexpr int64_t MAX_ALIGNMENT_CELLS = 16 * 1024 * 1024;
constexpr int64_t ABSORB_MAX_WORDS = 2; // a matched run this short between two edits joins them

struct Run {
    bool match;
    int64_t fresh_start;
    int64_t fresh_count;
    int64_t vanilla_start;
    int64_t vanilla_count;
    int64_t fresh_end() const { return fresh_start + fresh_count; }
    int64_t vanilla_end() const { return vanilla_start + vanilla_count; }
};

bool words_match(const PatchInput& input, int64_t fresh_index, int64_t vanilla_index) {
    uint32_t keep = ~input.mask[size_t(fresh_index)];
    return (input.fresh[size_t(fresh_index)] & keep) == (input.vanilla[size_t(vanilla_index)] & keep);
}

std::optional<std::vector<std::pair<int64_t, int64_t>>> align(const PatchInput& input, int64_t live) {
    int64_t n = live;
    int64_t m = int64_t(input.vanilla.size());
    if (n * m > MAX_ALIGNMENT_CELLS) {
        return std::nullopt;
    }
    std::vector<uint16_t> table(size_t((n + 1) * (m + 1)), 0);
    auto at = [&](int64_t i, int64_t j) -> uint16_t& { return table[size_t(i * (m + 1) + j)]; };
    for (int64_t i = n - 1; i >= 0; i--) {
        for (int64_t j = m - 1; j >= 0; j--) {
            at(i, j) = words_match(input, i, j) ? uint16_t(at(i + 1, j + 1) + 1) : std::max(at(i + 1, j), at(i, j + 1));
        }
    }

    std::vector<std::pair<int64_t, int64_t>> pairs;
    int64_t i = 0, j = 0;
    while (i < n && j < m) {
        if (words_match(input, i, j) && at(i, j) == at(i + 1, j + 1) + 1) {
            pairs.emplace_back(i, j);
            i++;
            j++;
        } else if (at(i + 1, j) >= at(i, j + 1)) {
            i++;
        } else {
            j++;
        }
    }
    return pairs;
}

std::vector<Run> runs_from_pairs(const std::vector<std::pair<int64_t, int64_t>>& pairs, int64_t n, int64_t m) {
    std::vector<Run> runs;
    size_t next_pair = 0;
    int64_t i = 0, j = 0;
    while (i < n || j < m) {
        if (next_pair < pairs.size() && pairs[next_pair] == std::make_pair(i, j)) {
            Run run{true, i, 0, j, 0};
            while (next_pair < pairs.size() && pairs[next_pair] == std::make_pair(i, j)) {
                next_pair++;
                i++;
                j++;
            }
            run.fresh_count = run.vanilla_count = i - run.fresh_start;
            runs.push_back(run);
        } else {
            int64_t next_i = next_pair < pairs.size() ? pairs[next_pair].first : n;
            int64_t next_j = next_pair < pairs.size() ? pairs[next_pair].second : m;
            runs.push_back({false, i, next_i - i, j, next_j - j});
            i = next_i;
            j = next_j;
        }
    }
    return runs;
}

bool targeted_by_conditional_from_outside(
    const PatchInput& input, const std::vector<IntraBranch>& branches, int64_t first, int64_t end, int64_t word) {
    for (const IntraBranch& branch : branches) {
        if (branch.to == word && ppc::is_b_form(input.fresh[size_t(branch.from)]) &&
            (branch.from < first || branch.from >= end)) {
            return true;
        }
    }
    return false;
}

// a run of a word or two between edits costs more as its own hook & b back than as island words
void absorb_short_runs(std::vector<Run>& runs, const PatchInput& input, const std::vector<IntraBranch>& branches) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t k = 1; k + 1 < runs.size(); k++) {
            const Run& run = runs[k];
            if (!run.match || run.fresh_count > ABSORB_MAX_WORDS || runs[k - 1].match || runs[k + 1].match) {
                continue;
            }
            bool blocked = false;
            for (int64_t word = run.fresh_start; word < run.fresh_end(); word++) {
                if (input.pinned[size_t(word)] ||
                    targeted_by_conditional_from_outside(input, branches, run.fresh_start, run.fresh_end(), word)) {
                    blocked = true;
                }
            }
            if (blocked) {
                continue;
            }
            Run merged{false, runs[k - 1].fresh_start,
                runs[k - 1].fresh_count + run.fresh_count + runs[k + 1].fresh_count, runs[k - 1].vanilla_start,
                runs[k - 1].vanilla_count + run.vanilla_count + runs[k + 1].vanilla_count};
            runs.erase(runs.begin() + int64_t(k) - 1, runs.begin() + int64_t(k) + 2);
            runs.insert(runs.begin() + int64_t(k) - 1, merged);
            changed = true;
            break;
        }
    }
}

class Builder {
public:
    Builder(const PatchInput& input, int64_t live, std::vector<IntraBranch> branches, bool far)
        : input_(input),
          live_(live),
          far_(far) {
        plan_.island_far = far;
        plan_.branches = std::move(branches);
        int64_t n = int64_t(input.fresh.size());
        plan_.home.assign(size_t(n), -1);
        plan_.island_slot.assign(size_t(n), -1);
        plan_.vanilla_partner.assign(size_t(n), -1);
        displaced_.assign(size_t(n), false);
    }

    // "" = ok
    std::string build(const std::vector<Run>& runs) {
        for (size_t k = 0; k < runs.size(); k++) {
            const Run& run = runs[k];
            std::string problem = run.match ? place_match(run) : place_edit(run, k, runs);
            if (!problem.empty()) {
                return problem;
            }
        }
        if (std::string problem = finish(); !problem.empty()) {
            return problem;
        }
        return "";
    }

    PatchPlan take() { return std::move(plan_); }

private:
    int64_t slot_address(int64_t vanilla_index) const { return input_.vanilla_address + vanilla_index * 4; }
    const uint32_t& fresh(int64_t index) const { return input_.fresh[size_t(index)]; }
    bool is_conditional(int64_t index) const {
        return ppc::is_b_form(fresh(index)) && !input_.has_relocation[size_t(index)];
    }
    bool is_pc_relative_branch(int64_t index) const {
        return (ppc::is_b_form(fresh(index)) || ppc::is_i_form(fresh(index))) && !input_.has_relocation[size_t(index)];
    }

    void put_in_place(int64_t vanilla_index, Slot slot) {
        plan_.in_place.emplace_back(slot_address(vanilla_index), slot);
        if (slot.kind == SlotKind::Fresh) {
            plan_.home[size_t(slot.fresh_index)] = slot_address(vanilla_index);
            plan_.vanilla_partner[size_t(slot.fresh_index)] = vanilla_index;
        }
    }

    void put_in_island(Slot slot) {
        if (slot.kind == SlotKind::Fresh) {
            plan_.island_slot[size_t(slot.fresh_index)] = int64_t(plan_.island.size());
        }
        plan_.island.push_back(slot);
    }

    bool falls_through_after(int64_t index) const { return index + 1 < live_ && !ppc::is_unconditional(fresh(index)); }

    std::string place_match(const Run& run) {
        for (int64_t k = 0; k < run.fresh_count; k++) {
            if (displaced_[size_t(run.fresh_start + k)]) {
                continue; // already in an island, its slot holds the hook
            }
            put_in_place(run.vanilla_start + k, {SlotKind::Fresh, run.fresh_start + k});
        }
        return "";
    }

    void unplace(int64_t fresh_index) {
        for (auto it = plan_.in_place.begin(); it != plan_.in_place.end(); it++) {
            if (it->second.kind == SlotKind::Fresh && it->second.fresh_index == fresh_index) {
                plan_.in_place.erase(it);
                break;
            }
        }
        plan_.home[size_t(fresh_index)] = -1;
    }

    std::string place_edit(const Run& run, size_t k, const std::vector<Run>& runs) {
        int64_t ins = run.fresh_count, del = run.vanilla_count;
        for (int64_t word = run.fresh_start; word < run.fresh_end(); word++) {
            if (input_.pinned[size_t(word)]) {
                return "word +0x" + hex_upper(word * 4) + " is pinned but has no vanilla twin";
            }
        }
        if (ins <= del) {
            for (int64_t w = 0; w < ins; w++) {
                put_in_place(run.vanilla_start + w, {SlotKind::Fresh, run.fresh_start + w});
            }
            int64_t leftover = del - ins;
            if (leftover == 0) {
                return "";
            }
            // leftover slots only matter if control falls into them
            int64_t before = run.fresh_end() - 1;
            bool reached = before < 0 ? true : falls_through_after(before);
            if (reached && run.fresh_end() < live_) {
                Slot skip{leftover == 1 ? SlotKind::Nop : SlotKind::BranchOver, run.fresh_end()};
                put_in_place(run.vanilla_start + ins, skip);
            }
            return "";
        }

        // a bc in place can follow a moved word only as the island's first slot
        std::optional<int64_t> displaced;
        int64_t hook_slot = run.vanilla_start;
        bool displaced_first = false;
        if (del == 0) {
            bool next_exists = k + 1 < runs.size() && runs[k + 1].match;
            bool previous_exists = k > 0 && runs[k - 1].match;
            std::optional<int64_t> after = next_exists ? std::optional<int64_t>(run.fresh_end()) : std::nullopt;
            std::optional<int64_t> before =
                previous_exists ? std::optional<int64_t>(run.fresh_start - 1) : std::nullopt;
            auto movable = [&](std::optional<int64_t> candidate) {
                if (!candidate || input_.pinned[size_t(*candidate)] || displaced_[size_t(*candidate)]) {
                    return false;
                }
                // a moved bc may have to be flipped to reach back
                return !(far_ && is_conditional(*candidate) && !ppc::inversion_toggle(fresh(*candidate)));
            };
            auto rank = [&](int64_t candidate) {
                if (!is_pc_relative_branch(candidate)) {
                    return 0;
                }
                return ppc::is_i_form(fresh(candidate)) ? 1 : 2;
            };
            // after: island = inserted words + moved word. before: island = moved word + inserted words
            bool after_ok = movable(after) &&
                !(far_ &&
                    conditional_from_outside_targets(
                        run.fresh_start, run.fresh_end() + 1, run.fresh_start + 1, run.fresh_end() + 1));
            bool before_ok = movable(before) &&
                !(far_ &&
                    conditional_from_outside_targets(
                        run.fresh_start - 1, run.fresh_end(), run.fresh_start, run.fresh_end()));
            if (after_ok && (!before_ok || rank(*after) <= rank(*before))) {
                displaced = after;
            } else if (before_ok) {
                displaced = before;
                displaced_first = true;
                hook_slot = run.vanilla_start - 1;
            } else {
                return "no instruction next to +0x" + hex_upper(run.fresh_start * 4) + " can make room for a hook";
            }
            displaced_[size_t(*displaced)] = true;
            if (displaced_first) {
                unplace(*displaced);
            }
        } else if (far_ &&
            conditional_from_outside_targets(run.fresh_start, run.fresh_end(), run.fresh_start + 1, run.fresh_end())) {
            return "a bc in place targets the middle of the insertion at +0x" + hex_upper(run.fresh_start * 4);
        }

        size_t first_slot = plan_.island.size();
        put_in_place(hook_slot, {SlotKind::Hook, displaced_first ? *displaced : run.fresh_start});
        if (displaced_first) {
            put_in_island({SlotKind::Fresh, *displaced});
        }
        for (int64_t word = run.fresh_start; word < run.fresh_end(); word++) {
            put_in_island({SlotKind::Fresh, word});
        }
        if (displaced && !displaced_first) {
            put_in_island({SlotKind::Fresh, *displaced});
        }
        int64_t last = displaced && !displaced_first ? *displaced : run.fresh_end() - 1;
        if (falls_through_after(last)) {
            put_in_island({SlotKind::BranchBack, last + 1});
        }
        plan_.islands.push_back({first_slot, slot_address(hook_slot)});
        return "";
    }

    bool conditional_from_outside_targets(
        int64_t inside_first, int64_t inside_end, int64_t check_first, int64_t check_end) const {
        for (int64_t word = check_first; word < check_end; word++) {
            if (targeted_by_conditional_from_outside(input_, plan_.branches, inside_first, inside_end, word)) {
                return true;
            }
        }
        return false;
    }

    std::string finish() {
        if (far_) {
            for (IntraBranch& branch : plan_.branches) {
                int64_t slot = plan_.island_slot[size_t(branch.from)];
                if (slot < 0 || !ppc::is_b_form(fresh(branch.from)) || plan_.island_slot[size_t(branch.to)] >= 0) {
                    continue;
                }
                if (!ppc::inversion_toggle(fresh(branch.from))) {
                    return "bc at +0x" + hex_upper(branch.from * 4) + " in an island can't be inverted";
                }
                branch.inverted = true;
                plan_.island.insert(plan_.island.begin() + slot + 1, Slot{SlotKind::InvertHelper, branch.to});
                for (int64_t& other : plan_.island_slot) {
                    if (other > slot) {
                        other++;
                    }
                }
                for (Island& island : plan_.islands) {
                    if (island.first_slot > size_t(slot)) {
                        island.first_slot++;
                    }
                }
            }
        }

        for (const IntraBranch& branch : plan_.branches) {
            bool b_form = ppc::is_b_form(fresh(branch.from));
            bool from_island = plan_.island_slot[size_t(branch.from)] >= 0;
            if (plan_.home[size_t(branch.to)] < 0 && plan_.island_slot[size_t(branch.to)] < 0) {
                return "branch at +0x" + hex_upper(branch.from * 4) + " targets a dropped word";
            }
            if (branch.inverted) {
                continue;
            }
            if (!plan_.reaches(branch.to, b_form, from_island)) {
                return "bc at +0x" + hex_upper(branch.from * 4) + " can't reach +0x" + hex_upper(branch.to * 4);
            }
        }

        // a bc can't reach beyond 32 KB : the planner shouldn't promise what it can't encode
        int64_t vanilla_bytes = int64_t(input_.vanilla.size()) * 4;
        int64_t span = far_ ? std::max(vanilla_bytes, plan_.island_bytes()) : vanilla_bytes + plan_.island_bytes();
        if (span >= 0x8000) {
            return "too big for a bc to reach across";
        }

        if (!far_) {
            plan_.island_base = input_.vanilla_address + vanilla_bytes;
            if (plan_.island_bytes() > input_.local_slack) {
                return "island doesn't fit in the local slack";
            }
            for (size_t slot = 0; slot < plan_.island.size(); slot++) {
                if (plan_.island[slot].kind == SlotKind::Fresh) {
                    plan_.home[size_t(plan_.island[slot].fresh_index)] = plan_.island_base + int64_t(slot) * 4;
                }
            }
        } else if (!plan_.island.empty()) {
            int64_t vanilla_end = input_.vanilla_address + int64_t(input_.vanilla.size()) * 4;
            if (!ppc::i_form_reaches(input_.vanilla_address, input_.far_end) ||
                !ppc::i_form_reaches(input_.far_end, input_.vanilla_address) ||
                !ppc::i_form_reaches(input_.far_start, vanilla_end)) {
                return "free region out of branch range";
            }
        }

        std::sort(plan_.in_place.begin(), plan_.in_place.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        compute_dead();
        estimate();
        return "";
    }

    void compute_dead() {
        int64_t start = input_.vanilla_address;
        int64_t end = start + int64_t(input_.vanilla.size()) * 4;
        int64_t cursor = start;
        for (const auto& [address, slot] : plan_.in_place) {
            if (address > cursor) {
                plan_.dead_vanilla.emplace_back(cursor, address);
            }
            cursor = address + 4;
        }
        if (cursor < end) {
            plan_.dead_vanilla.emplace_back(cursor, end);
        }
    }

    void estimate() {
        std::vector<int64_t> changed;
        for (const auto& [address, slot] : plan_.in_place) {
            bool differs = true;
            if (slot.kind == SlotKind::Fresh) {
                int64_t partner = plan_.vanilla_partner[size_t(slot.fresh_index)];
                differs = !words_match(input_, slot.fresh_index, partner);
                if (!differs && is_pc_relative_branch(slot.fresh_index)) {
                    for (const IntraBranch& branch : plan_.branches) {
                        if (branch.from == slot.fresh_index && plan_.island_slot[size_t(branch.to)] >= 0) {
                            differs = true;
                        }
                    }
                }
            }
            if (differs) {
                changed.push_back(address);
            }
        }
        plan_.estimated_lines = estimate_lines(changed);
        if (!plan_.island.empty()) {
            plan_.estimated_lines += 1 + (plan_.island_bytes() + 7) / 8;
        }
    }

    const PatchInput& input_;
    int64_t live_;
    bool far_;
    PatchPlan plan_;
    std::vector<bool> displaced_;
};

PatchOutcome plan_variant(
    const PatchInput& input, int64_t live, const std::vector<IntraBranch>& branches, std::vector<Run> runs, bool far) {
    absorb_short_runs(runs, input, branches);
    Builder builder(input, live, branches, far);
    std::string problem = builder.build(runs);
    if (!problem.empty()) {
        return {std::nullopt, problem};
    }
    return {builder.take(), ""};
}

} // namespace

bool PatchPlan::reaches(int64_t to, bool b_form, bool from_island) const {
    int64_t slot = island_slot[size_t(to)];
    if (slot < 0) {
        return home[size_t(to)] >= 0;
    }
    if (!island_far || !b_form || from_island) {
        return true;
    }
    for (const Island& each : islands) {
        if (int64_t(each.first_slot) == slot) {
            return true;
        }
    }
    return false;
}

std::optional<int64_t> PatchPlan::branch_target(int64_t to, bool b_form, bool from_island) const {
    if (!reaches(to, b_form, from_island)) {
        return std::nullopt;
    }
    int64_t slot = island_slot[size_t(to)];
    if (slot < 0) {
        return home[size_t(to)];
    }
    if (!island_far || !b_form || from_island) {
        return island_base >= 0 ? std::optional<int64_t>(island_base + slot * 4) : std::nullopt;
    }
    for (const Island& each : islands) {
        if (int64_t(each.first_slot) == slot) {
            return each.hook_address;
        }
    }
    return std::nullopt;
}

PatchOutcome plan_patch(const PatchInput& given) {
    int64_t n = int64_t(given.fresh.size());
    int64_t m = int64_t(given.vanilla.size());
    if (n == 0 || m == 0) {
        return {std::nullopt, "nothing to align"};
    }
    if (given.mask.size() != size_t(n) || given.has_relocation.size() != size_t(n) ||
        given.pinned.size() != size_t(n)) {
        return {std::nullopt, "inconsistent input"};
    }

    // label branches carry no relocation: their displacement is ours to rewrite, so it's masked too
    PatchInput input = given;
    std::vector<IntraBranch> branches;
    for (int64_t i = 0; i < n; i++) {
        uint32_t word = input.fresh[size_t(i)];
        if (input.has_relocation[size_t(i)] || !(ppc::is_i_form(word) || ppc::is_b_form(word))) {
            continue;
        }
        // LR must stay what vanilla had
        if (ppc::has_link(word)) {
            return {std::nullopt, "linked branch to a label at +0x" + hex_upper(i * 4) + ": position-dependent code"};
        }
        input.mask[size_t(i)] |= ppc::is_i_form(word) ? ppc::I_FORM_MASK : ppc::B_FORM_MASK;
        std::optional<int64_t> displacement = ppc::relative_displacement(word);
        if (!displacement) {
            return {std::nullopt, "absolute branch at +0x" + hex_upper(i * 4)};
        }
        int64_t target = i + *displacement / 4;
        if (target < 0 || target >= n) {
            return {std::nullopt, "branch at +0x" + hex_upper(i * 4) + " leaves the function"};
        }
        branches.push_back({i, target, false});
    }

    // trailing nops are padding
    int64_t live = n;
    int64_t last = n - 1;
    while (last > 0 && input.fresh[size_t(last)] == ppc::NOP && !input.has_relocation[size_t(last)]) {
        last--;
    }
    if (last < n - 1 && ppc::is_unconditional(input.fresh[size_t(last)])) {
        live = last + 1;
        for (const IntraBranch& branch : branches) {
            if (branch.to >= live || branch.from >= live) {
                live = n;
            }
        }
        for (int64_t i = live; i < n; i++) {
            if (input.pinned[size_t(i)]) {
                live = n;
            }
        }
    }

    std::optional<std::vector<std::pair<int64_t, int64_t>>> pairs = align(input, live);
    if (!pairs) {
        return {std::nullopt, "function too big to align"};
    }
    std::vector<Run> runs = runs_from_pairs(*pairs, live, m);

    if (input.local_slack >= 4) {
        PatchOutcome local = plan_variant(input, live, branches, runs, false);
        if (local.plan) {
            return local;
        }
    }
    return plan_variant(input, live, branches, runs, true);
}

void assign_island_base(PatchPlan& plan, int64_t base) {
    plan.island_base = base;
    for (size_t slot = 0; slot < plan.island.size(); slot++) {
        if (plan.island[slot].kind == SlotKind::Fresh) {
            plan.home[size_t(plan.island[slot].fresh_index)] = base + int64_t(slot) * 4;
        }
    }
}

} // namespace Decomp2Gecko
