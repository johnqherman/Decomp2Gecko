#pragma once

#include <unordered_set>
#include <vector>

#include "Decomp2Gecko/chunks.h"

namespace Decomp2Gecko {

const char* action_label(ChunkAction action);

bool map_by_name(Chunk& chunk, const Decomp& decomp);

std::optional<std::pair<int64_t, int>> find_high_half_relocation(const Chunk& chunk, const Relocation& low_half);

using ClaimedSymbols = std::unordered_set<const VanillaSymbol*>;

int map_through_references(
    const std::vector<Chunk*>& chunks, const ChunkLocator& locator, const Decomp& decomp, ClaimedSymbols& claimed);

bool map_by_content(Chunk& chunk, const Decomp& decomp, ClaimedSymbols& claimed);

} // namespace Decomp2Gecko
