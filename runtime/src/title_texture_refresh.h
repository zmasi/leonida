#pragma once
#include <algorithm>
#include <bit>
#include <map>
#include <cstdint>
#include <utility>
#include <vector>

namespace leonida {

using TitleWriteRanges = std::vector<std::pair<std::uintptr_t, std::uintptr_t>>;

// Store each texture once, in the smallest aligned power-of-two page containing its CPU range.
// A journal interval needs only the overlapping pages at each occupied size. Aliases keep distinct keys.
class TitleTextureRangeIndex {
    using Address = std::uint64_t;
    struct Range { Address first, last; };
    using PageKey = std::pair<Address, Address>;  // page, registry key
    std::map<unsigned, std::map<PageKey, Range>> pages_;
    std::map<Address, std::pair<unsigned, Address>> location_;
    static Address page(Address at, unsigned shift) { return shift == 64 ? 0 : at >> shift; }

public:
    void erase(Address key) {
        const auto where = location_.find(key);
        if (where == location_.end()) return;
        const auto [shift, number] = where->second;
        auto level = pages_.find(shift);
        level->second.erase({number, key});
        if (level->second.empty()) pages_.erase(level);
        location_.erase(where);
    }
    void set(Address key, Address first, Address bytes) {
        erase(key);
        if (!bytes) return;
        const Address last = first + std::min(bytes - 1, UINT64_MAX - first);
        const unsigned shift = unsigned(std::bit_width(first ^ last));
        const Address number = page(first, shift);
        pages_[shift][{number, key}] = {first, last};
        location_[key] = {shift, number};
    }
    template<class Visit>
    void overlapping(Address from, Address to, Visit visit) const {
        if (from >= to) return;
        for (const auto& [shift, entries] : pages_) {
            const Address last = page(to - 1, shift);
            for (auto it = entries.lower_bound({page(from, shift), 0});
                 it != entries.end() && it->first.first <= last; ++it) {
                if (it->second.first < to && from <= it->second.last) visit(it->first.second);
            }
        }
    }
};

// Sorted, disjoint half-open intervals let each texture find its first possible writer by end address.
inline void merge_title_writes(TitleWriteRanges& writes) {
    if (writes.empty()) return;
    if (writes.size() == 1) {
        if (writes[0].first >= writes[0].second) writes.clear();
        return;
    }
    if (!std::is_sorted(writes.begin(), writes.end())) std::sort(writes.begin(), writes.end());
    std::size_t count = 0;
    for (const auto [begin, end] : writes) {
        if (begin >= end) continue;
        if (count && begin <= writes[count - 1].second)
            writes[count - 1].second = std::max(writes[count - 1].second, end);
        else
            writes[count++] = {begin, end};
    }
    writes.resize(count);
}

inline bool title_write_overlaps(const TitleWriteRanges& writes, std::uintptr_t begin, std::uintptr_t end) {
    if (begin >= end || writes.empty()) return false;
    if (end <= writes.front().first || begin >= writes.back().second) return false;
    if (writes.size() == 1) return true;
    const auto it = std::upper_bound(writes.begin(), writes.end(), begin,
                                     [](auto address, const auto& range) { return address < range.second; });
    return it != writes.end() && it->first < end;
}

// The registry's entries as one pass will visit them, in placement order. A pass cannot walk the registry itself: a
// fill it makes can submit, a submission destroys a retired image, and that image's destructor erases its entry
// (DeviceState::title_texture_gone). Windows' mutex is recursive for its owner, so the erase lands inside the pass and
// the walk would step through a freed node. The list holds a reference to each entry, so an entry erased during the
// pass is still alive when its turn comes.
template<class Registry>
std::vector<std::pair<std::uint64_t, typename Registry::mapped_type>> title_texture_snapshot(
    const Registry& textures, std::uint64_t start = 0, std::size_t limit = SIZE_MAX) {
    std::vector<std::pair<std::uint64_t, typename Registry::mapped_type>> entries;
    const auto count = std::min(textures.size(), limit);
    entries.reserve(count);
    for (auto it = textures.lower_bound(start); it != textures.end() && entries.size() < count; ++it)
        entries.emplace_back(it->first, it->second);
    return entries;
}

// Poll unmarked textures of a snapshot in placement order. A time or byte limit resumes the next pass at
// the following address. The cursor also survives insertion or removal between passes.
template<class Entry, class Visit, class Exhausted>
void poll_title_textures(const std::vector<Entry>& entries, std::uint64_t& cursor, Visit visit, Exhausted exhausted) {
    if (entries.empty()) return;
    // A snapshot of a std::map comes out sorted by address.
    const auto first = std::lower_bound(entries.begin(), entries.end(), cursor,
                                        [](const Entry& entry, std::uint64_t at) { return entry.first < at; });
    auto index = std::size_t(first - entries.begin());
    if (index == entries.size()) index = 0;  // the cursor is past the last address: resume at the first
    for (auto left = entries.size(); left; --left) {
        const auto& [address, texture] = entries[index];
        cursor = address == UINT64_MAX ? 0u : address + 1u;
        if (!texture->mapped && !texture->stale) visit(*texture);
        if (exhausted()) break;  // advance at least one registry entry even if a deadline already passed
        index = index + 1 == entries.size() ? 0 : index + 1;
    }
}

// Visit the marked textures of a snapshot (mapped by the title, or known stale) under a time limit
// (LEONIDA_TITLE_EVERY_BUDGET_US). Those `urgent` answers true for (a mapped texture never filled yet) are all
// visited first, in placement order, whatever the limit. The others are visited
// from `cursor`, as poll_title_textures does for the unmarked ones, until `exhausted()` answers true after a visit;
// the next pass resumes at the entry after the last one visited, so every marked texture gets its turn however many
// there are. An entry skipped as unmarked costs no check.
template<class Entry, class Visit, class Urgent, class Exhausted>
void visit_marked_title_textures(const std::vector<Entry>& entries, std::uint64_t& cursor, Visit visit, Urgent urgent,
                                 Exhausted exhausted) {
    if (entries.empty()) return;
    // Which entries are urgent is decided before any visit: a visit fills the texture, and it must not come round
    // again in the rotation of the same pass.
    std::vector<char> was_urgent(entries.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& texture = entries[i].second;
        was_urgent[i] = (texture->mapped || texture->stale) && urgent(*texture);
    }
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (was_urgent[i]) visit(*entries[i].second);
    const auto first = std::lower_bound(entries.begin(), entries.end(), cursor,
                                        [](const Entry& entry, std::uint64_t at) { return entry.first < at; });
    auto index = std::size_t(first - entries.begin());
    if (index == entries.size()) index = 0;
    for (auto left = entries.size(); left; --left) {
        const auto& [address, texture] = entries[index];
        cursor = address + 1;
        if ((texture->mapped || texture->stale) && !was_urgent[index]) {
            visit(*texture);
            if (exhausted()) break;
        }
        index = index + 1 == entries.size() ? 0 : index + 1;
    }
}

}  // namespace leonida
