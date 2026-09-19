// iteration order decides tie-breaks (which vanilla address a chunk maps to, which unit a slippi
// note names), so every iterated map keeps insertion order. assigning to a key keeps its position
#pragma once

#include <cstddef>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Decomp2Gecko {

template <class Key, class Value> class OrderedMap {
public:
    using Entry = std::pair<Key, Value>;
    using iterator = typename std::vector<Entry>::iterator;
    using const_iterator = typename std::vector<Entry>::const_iterator;

    size_t size() const { return entries_.size(); }
    iterator begin() { return entries_.begin(); }
    iterator end() { return entries_.end(); }
    const_iterator begin() const { return entries_.begin(); }
    const_iterator end() const { return entries_.end(); }

    bool contains(const Key& key) const { return index_.count(key) != 0; }

    Value* find(const Key& key) {
        auto found = index_.find(key);
        return found == index_.end() ? nullptr : &entries_[found->second].second;
    }
    const Value* find(const Key& key) const {
        auto found = index_.find(key);
        return found == index_.end() ? nullptr : &entries_[found->second].second;
    }

    Value& operator[](const Key& key) {
        auto found = index_.find(key);
        if (found != index_.end()) {
            return entries_[found->second].second;
        }
        index_.emplace(key, entries_.size());
        entries_.emplace_back(key, Value());
        return entries_.back().second;
    }

    Value& setdefault(const Key& key, Value fallback) {
        auto found = index_.find(key);
        if (found != index_.end()) {
            return entries_[found->second].second;
        }
        index_.emplace(key, entries_.size());
        entries_.emplace_back(key, std::move(fallback));
        return entries_.back().second;
    }

    void clear() {
        entries_.clear();
        index_.clear();
    }

private:
    std::vector<Entry> entries_;
    std::unordered_map<Key, size_t> index_;
};

} // namespace Decomp2Gecko
