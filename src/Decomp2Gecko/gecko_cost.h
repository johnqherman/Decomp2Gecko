#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "Decomp2Gecko/vanilla.h"

namespace Decomp2Gecko {

using SpanCheck = std::function<bool(int64_t, int64_t)>; // one write may cover [start, end)

inline bool always_spans(int64_t, int64_t) { return true; }

inline int64_t write_lines(int64_t start, int64_t end) {
    int64_t length = end - start;
    return (length == 4 && (start & 3) == 0) ? 1 : 1 + (length + 7) / 8;
}

// a merge saves at most the one header line, so a gap this wide never pays
inline constexpr int64_t MAX_USEFUL_GAP = 16;

// adjacent ranges never split: k words cost k as 04s, at most 1 + ceil(k/2) as one 06
inline std::vector<AddressRange> segment_writes(
    const std::vector<AddressRange>& changed, const SpanCheck& can_span = always_spans) {
    std::vector<AddressRange> runs;
    for (const auto& [start, end] : changed) {
        if (!runs.empty() && runs.back().second == start && can_span(runs.back().first, end)) {
            runs.back().second = end;
        } else {
            runs.emplace_back(start, end);
        }
    }

    size_t count = runs.size();
    std::vector<int64_t> best(count + 1, 0);
    std::vector<size_t> cut(count + 1, 0); // best[j] ends w/ a write that starts at run cut[j]
    for (size_t j = 1; j <= count; j++) {
        best[j] = INT64_MAX;
        for (size_t i = j; i >= 1; i--) {
            if (i < j &&
                (runs[i].first - runs[i - 1].second >= MAX_USEFUL_GAP ||
                    !can_span(runs[i - 1].first, runs[j - 1].second))) {
                break;
            }
            int64_t lines = best[i - 1] + write_lines(runs[i - 1].first, runs[j - 1].second);
            if (lines < best[j]) {
                best[j] = lines;
                cut[j] = i - 1;
            }
        }
    }

    std::vector<AddressRange> segments;
    for (size_t j = count; j > 0; j = cut[j]) {
        segments.emplace_back(runs[cut[j]].first, runs[j - 1].second);
    }
    std::reverse(segments.begin(), segments.end());
    return segments;
}

inline int64_t estimate_lines(const std::vector<AddressRange>& changed, const SpanCheck& can_span = always_spans) {
    int64_t total = 0;
    for (const auto& [start, end] : segment_writes(changed, can_span)) {
        total += write_lines(start, end);
    }
    return total;
}

inline int64_t estimate_lines(const std::vector<int64_t>& words, const SpanCheck& can_span = always_spans) {
    std::vector<AddressRange> changed;
    for (int64_t word : words) {
        changed.emplace_back(word, word + 4);
    }
    return estimate_lines(changed, can_span);
}

} // namespace Decomp2Gecko
