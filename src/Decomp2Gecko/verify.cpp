#include "Decomp2Gecko/verify.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "Decomp2Gecko/chunks_internal.h"
#include "Decomp2Gecko/ppc.h"
#include "Decomp2Gecko/reloc.h"
#include "Decomp2Gecko/util.h"

namespace Decomp2Gecko {

namespace {

struct LinkedImage {
    MemImage image;
    std::unordered_map<std::string, int64_t> global_addresses;
    std::map<int, int64_t> small_data_bases;
};

LinkedImage load_linked_image(const ElfFile& elf) {
    LinkedImage linked;
    std::vector<const Section*> sections = elf.allocated_sections();
    std::stable_sort(
        sections.begin(), sections.end(), [](const Section* a, const Section* b) { return a->address < b->address; });
    for (const Section* section : sections) {
        linked.image.add_region(section->address, section->size,
            section->header_type == SHT_NOBITS ? std::string_view() : std::string_view(section->data));
    }

    std::set<std::string> ambiguous;
    for (const Symbol& symbol : elf.symbols) {
        bool is_function_or_object = symbol.symbol_type == STT_FUNC || symbol.symbol_type == STT_OBJECT;
        if (symbol.is_global() && symbol.is_defined() && is_function_or_object) {
            if (linked.global_addresses.count(symbol.name)) {
                ambiguous.insert(symbol.name);
            }
            linked.global_addresses[symbol.name] = symbol.value;
        }
    }
    for (const std::string& name : ambiguous) {
        linked.global_addresses.erase(name);
    }

    // a modified link shifts small data; MWLD records its r13/r2 as _SDA_BASE_ & _SDA2_BASE_
    for (const Symbol& symbol : elf.symbols) {
        if (symbol.name == "_SDA_BASE_") {
            linked.small_data_bases[13] = symbol.value;
        } else if (symbol.name == "_SDA2_BASE_") {
            linked.small_data_bases[2] = symbol.value;
        }
    }
    return linked;
}

// label branches are masked out of the byte comparison & checked by target instead
std::vector<std::string> check_patched(const MemImage& our_image, const PatchPlan& plan, const Bytes& our_bytes,
    const Bytes& linked_bytes, Bytes& masked_ours, Bytes& masked_linked) {
    std::vector<std::string> problems;
    for (const IntraBranch& branch : plan.branches) {
        uint32_t elf_word = read_u32_be(linked_bytes, size_t(branch.from * 4));
        uint32_t our_word = read_u32_be(our_bytes, size_t(branch.from * 4));
        uint32_t field = ppc::is_b_form(elf_word) ? ppc::B_FORM_MASK : ppc::I_FORM_MASK;
        if (branch.inverted) {
            field |= ppc::inversion_toggle(elf_word).value_or(0);
        }
        write_u32_be(masked_ours, size_t(branch.from * 4), our_word & ~field);
        write_u32_be(masked_linked, size_t(branch.from * 4), elf_word & ~field);

        std::string label = "branch +0x" + hex_upper(branch.from * 4) + " -> +0x" + hex_upper(branch.to * 4);
        std::optional<int64_t> elf_displacement = ppc::relative_displacement(elf_word);
        if (!elf_displacement || branch.from + *elf_displacement / 4 != branch.to) {
            problems.push_back(label + ": elf word " + hex_upper(elf_word, 8) + " targets something else");
            continue;
        }
        int64_t from_home = plan.home[size_t(branch.from)];
        int64_t to_home = plan.home[size_t(branch.to)];
        std::optional<int64_t> our_displacement = ppc::relative_displacement(our_word);
        if (!our_displacement || from_home < 0 || to_home < 0) {
            problems.push_back(label + ": our word " + hex_upper(our_word, 8) + " isn't a relative branch");
            continue;
        }
        if (branch.inverted) {
            uint32_t helper = our_image.word(from_home + 4);
            std::optional<int64_t> helper_displacement = ppc::relative_displacement(helper);
            if (*our_displacement != 8 || !ppc::is_i_form(helper) || !helper_displacement ||
                from_home + 4 + *helper_displacement != to_home) {
                problems.push_back(label + ": inverted pair " + hex_upper(our_word, 8) + " " + hex_upper(helper, 8) +
                    " doesn't reach " + hex_upper(to_home, 8));
            }
            continue;
        }
        std::optional<int64_t> expected =
            plan.branch_target(branch.to, ppc::is_b_form(our_word), plan.island_slot[size_t(branch.from)] >= 0);
        if (!expected || from_home + *our_displacement != *expected) {
            problems.push_back(label + ": our word " + hex_upper(our_word, 8) + " at " + hex_upper(from_home, 8) +
                " doesn't reach " + hex_upper(expected.value_or(-1), 8));
        }
    }

    auto lands_on = [&](int64_t site, int64_t target) {
        uint32_t word = our_image.word(site);
        std::optional<int64_t> displacement = ppc::relative_displacement(word);
        return ppc::is_i_form(word) && displacement && site + *displacement == target;
    };
    for (const Island& island : plan.islands) {
        if (!lands_on(island.hook_address, plan.island_base + int64_t(island.first_slot) * 4)) {
            problems.push_back("hook at " + hex_upper(island.hook_address, 8) + " doesn't enter its island");
        }
    }
    auto check_glue = [&](int64_t site, const Slot& slot) {
        bool glue = slot.kind == SlotKind::BranchOver || slot.kind == SlotKind::BranchBack ||
            slot.kind == SlotKind::InvertHelper;
        if (glue && !lands_on(site, plan.home[size_t(slot.fresh_index)])) {
            problems.push_back(
                "b at " + hex_upper(site, 8) + " doesn't reach word +0x" + hex_upper(slot.fresh_index * 4));
        }
    };
    for (const auto& [site, slot] : plan.in_place) {
        check_glue(site, slot);
    }
    for (size_t index = 0; index < plan.island.size(); index++) {
        check_glue(plan.island_base + int64_t(index) * 4, plan.island[index]);
    }
    return problems;
}

} // namespace

VerifyResult verify(const Layout& layout) {
    VerifyResult result;
    const std::filesystem::path& elf_path = layout.decomp().main_elf;
    if (!std::filesystem::exists(elf_path)) {
        result.problems.push_back(display_path(elf_path) + " missing; build the decomp first");
        return result;
    }

    ElfFile elf = read_elf(elf_path);
    LinkedImage linked = load_linked_image(elf);
    const MemImage& linked_image = linked.image;
    const std::map<int, int64_t>& linked_sda = linked.small_data_bases;
    if (!linked_sda.count(13) || !linked_sda.count(2)) {
        result.problems.push_back("could not determine SDA bases of main.elf");
        return result;
    }

    result.ran = true;
    std::map<int, int64_t> our_sda = {{13, layout.decomp().sda_base()}, {2, layout.decomp().sda2_base()}};
    const MemImage& our_image = layout.image();
    ReadWord read_linked_word = [&linked_image](int64_t address) { return linked_image.try_word(address); };
    ReadWord read_our_word = [&our_image](int64_t address) { return our_image.try_word(address); };

    // locals: follow references from chunks w/ a known ELF address
    std::unordered_map<const Chunk*, int64_t> linked_address;
    for (const Chunk* chunk : layout.chunks()) {
        for (const Symbol* symbol : chunk->symbols) {
            auto known = linked.global_addresses.find(symbol->name);
            if (symbol->is_global() && known != linked.global_addresses.end()) {
                linked_address[chunk] = known->second - (symbol->value - chunk->offset);
                break;
            }
        }
    }

    bool progress = true;
    while (progress) {
        progress = false;
        for (const Chunk* chunk : layout.chunks()) {
            auto known = linked_address.find(chunk);
            if (known == linked_address.end() || !chunk->kept || chunk->is_bss()) {
                continue;
            }
            int64_t chunk_linked = known->second;
            for (const Relocation* relocation : chunk->relocations) {
                RelocationTarget target = layout.relocation_target(*chunk, *relocation);
                if (target.chunk == nullptr || linked_address.count(target.chunk)) {
                    continue;
                }

                HighHalf high_half;
                if (relocation->reloc_type == R_PPC_ADDR16_LO) {
                    std::optional<std::pair<int64_t, int>> partner = find_high_half_relocation(*chunk, *relocation);
                    if (!partner) {
                        continue;
                    }
                    high_half = std::make_pair(chunk_linked + partner->first, partner->second);
                }
                std::optional<int64_t> encoded = decode_relocation_target(read_linked_word,
                    chunk_linked + relocation->offset - chunk->offset, relocation->reloc_type, linked_sda, high_half);
                if (!encoded) {
                    continue;
                }

                linked_address[target.chunk] = (*encoded - target.offset_in_chunk - target.addend) & 0xFFFFFFFF;
                progress = true;
            }
        }
    }

    for (const Chunk* chunk : layout.chunks()) {
        if (!chunk->kept || !chunk->changed || chunk->is_bss() || !chunk->placed_address) {
            continue;
        }
        auto known = linked_address.find(chunk);
        if (known == linked_address.end()) {
            result.skipped.push_back(chunk->label() + " (address in main.elf unknown)");
            continue;
        }
        int64_t chunk_linked = known->second;
        int64_t placed = *chunk->placed_address;
        result.checked += 1;

        const PatchPlan* plan = layout.patch_plan(*chunk);
        Bytes our_bytes = layout.logical_bytes(*chunk);
        std::optional<Bytes> linked_bytes = linked_image.try_read(chunk_linked, chunk->size);
        if (!linked_bytes) {
            result.problems.push_back(
                chunk->label() + ": main.elf address " + hex_upper(chunk_linked, 8) + " unreadable");
            continue;
        }

        Bytes masked_ours = our_bytes;
        Bytes masked_linked = *linked_bytes;
        for (const Relocation* relocation : chunk->relocations) {
            std::optional<std::pair<int64_t, int64_t>> span =
                relocation_field_span(relocation->reloc_type, relocation->offset - chunk->offset);
            if (!span) {
                continue;
            }
            auto [field_start, field_width] = *span;
            overwrite(masked_ours, field_start, Bytes(size_t(field_width), '\0'));
            overwrite(masked_linked, field_start, Bytes(size_t(field_width), '\0'));
        }
        std::vector<std::string> problems;
        if (plan != nullptr) {
            problems = check_patched(our_image, *plan, our_bytes, *linked_bytes, masked_ours, masked_linked);
        }

        if (masked_ours != masked_linked) {
            for (int64_t position = 0; position < chunk->size; position += 4) {
                if (slice(masked_ours, position, position + 4) != slice(masked_linked, position, position + 4)) {
                    problems.push_back("word +0x" + hex_upper(position) + ": ours " +
                        hex_lower(slice(our_bytes, position, position + 4)) + " elf " +
                        hex_lower(slice(*linked_bytes, position, position + 4)));
                    if (problems.size() >= 3) {
                        break;
                    }
                }
            }
        }

        for (const Relocation* relocation : chunk->relocations) {
            int64_t field_offset = relocation->offset - chunk->offset;
            std::string type_name = reloc_type_name_or_none(relocation->reloc_type);
            if (relocation->reloc_type == R_PPC_ADDR16_HA || relocation->reloc_type == R_PPC_ADDR16_HI) {
                continue; // checked together w/ the @l half
            }

            auto our_site = [&](int64_t offset) -> std::optional<int64_t> {
                std::optional<int64_t> word_home = layout.placed_address_of(*chunk, offset & ~int64_t(3));
                return word_home ? std::optional<int64_t>(*word_home + (offset & 3)) : std::nullopt;
            };
            HighHalf our_high_half, linked_high_half;
            if (relocation->reloc_type == R_PPC_ADDR16_LO) {
                std::optional<std::pair<int64_t, int>> partner = find_high_half_relocation(*chunk, *relocation);
                if (!partner || !our_site(partner->first)) {
                    continue;
                }
                our_high_half = std::make_pair(*our_site(partner->first), partner->second);
                linked_high_half = std::make_pair(chunk_linked + partner->first, partner->second);
            }
            if (!our_site(field_offset)) {
                continue;
            }
            std::optional<int64_t> our_target = decode_relocation_target(
                read_our_word, *our_site(field_offset), relocation->reloc_type, our_sda, our_high_half);
            std::optional<int64_t> linked_target = decode_relocation_target(
                read_linked_word, chunk_linked + field_offset, relocation->reloc_type, linked_sda, linked_high_half);
            if (!our_target || !linked_target) {
                continue;
            }

            RelocationTarget target = layout.relocation_target(*chunk, *relocation);
            const Symbol& symbol = chunk->object_file->symbols[size_t(relocation->symbol_index)];
            std::string field_label = type_name + " +0x" + hex_upper(field_offset) + " -> ";
            if (target.chunk != nullptr) {
                // unmasked on purpose: an address that overflowed 32 bits is a real problem
                std::optional<int64_t> expected_ours =
                    layout.placed_address_of(*target.chunk, target.offset_in_chunk + target.addend);
                if (!expected_ours || *our_target != *expected_ours) {
                    problems.push_back(field_label + target.chunk->name() + ": our field " + hex_upper(*our_target, 8) +
                        " != placed " + hex_upper(expected_ours.value_or(-1), 8));
                }

                // a folded duplicate: main.elf still names the original
                const Chunk* elf_target = layout.relocation_target(*chunk, *relocation, false).chunk;
                auto target_linked = linked_address.find(elf_target);
                if (target_linked == linked_address.end()) {
                    target_linked = linked_address.find(target.chunk);
                }
                if (target_linked != linked_address.end()) {
                    int64_t expected_linked = target_linked->second + target.offset_in_chunk + target.addend;
                    if (expected_linked != *linked_target) {
                        problems.push_back(field_label + target.chunk->name() + ": elf field " +
                            hex_upper(*linked_target, 8) + " != elf symbol " + hex_upper(expected_linked, 8));
                    }
                } else if (!target.chunk->is_bss() && target.chunk->relocations.empty() &&
                    layout.patch_plan(*target.chunk) == nullptr) {
                    // no ELF address, but a relocation-free target: its bytes must match at the decoded address
                    int64_t target_start = *linked_target - target.offset_in_chunk - target.addend;
                    std::optional<Bytes> linked_target_bytes = linked_image.try_read(target_start, target.chunk->size);
                    if (!linked_target_bytes) {
                        problems.push_back(field_label + target.chunk->name() + ": elf target " +
                            hex_upper(target_start, 8) + " unreadable");
                    } else if (*linked_target_bytes !=
                        our_image.read(*target.chunk->placed_address, target.chunk->size)) {
                        problems.push_back(field_label + target.chunk->name() + ": content at elf " +
                            hex_upper(target_start, 8) + " differs");
                    }
                }
            } else if (!target.unresolved_name && symbol.is_undefined() && linked.global_addresses.count(symbol.name)) {
                int64_t expected_linked = linked.global_addresses[symbol.name] + relocation->addend;
                if (expected_linked != *linked_target) {
                    problems.push_back(field_label + symbol.name + ": elf field " + hex_upper(*linked_target, 8) +
                        " != " + hex_upper(expected_linked, 8));
                }
            }
        }

        if (!problems.empty()) {
            result.problems.push_back(chunk->label() + " @ ours " + hex_upper(placed, 8) + " / elf " +
                hex_upper(chunk_linked, 8) + ":\n    " + join(problems, "\n    "));
        } else {
            result.ok += 1;
        }
    }

    return result;
}

} // namespace Decomp2Gecko
