#pragma once
// Where a title-window mapping sits and how much of a stream pool one mapping serves, without a device
// (docs/d3d12_vulkan.md section 3.3 "Title-chosen addresses").
//
// The device's resolve_title_memory makes the mapping; these decide its range first, so a test can count mappings
// without Vulkan. Two rules matter here:
//   - a window is page-aligned, capped at one request's maximum and charged to LEONIDA_TITLE_WINDOWS_MB; one that
//     does not fit is not made, and the address is backed another way or written as a zero-size view, never raw;
//   - a stream pool (the title's CPU view paired with a GPU-only alias, gfxgeom section 3) is one reservation of
//     16-24 GiB, so it cannot be one allocation: one mapping serves a page-aligned readable run of it, and every
//     request inside that run reuses it instead of asking for a 128 KiB window each.
#include "title_va_alias.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace leonida {

// A page-aligned mapping of the title's memory: [start, start + size).
struct TitleWindowRange {
    std::uint64_t start{};
    std::uint64_t size{};
};

// `size` bytes more fit in the title-window budget [used, cap).
inline bool title_window_fits(std::uint64_t cap, std::uint64_t used, std::uint64_t size) {
    return used <= cap && size <= cap - used;
}

// The page-aligned range a window made for [va, va + bytes) covers: inside the free span [lo, hi), aligned to
// `align`, at most `most` bytes of the request, and charged against [used, cap). False when the span holds no page
// for it or the budget has no room.
inline bool title_window_range(std::uint64_t va, std::uint64_t bytes, std::uint64_t lo, std::uint64_t hi,
                               std::uint64_t align, std::uint64_t most, std::uint64_t cap, std::uint64_t used,
                               TitleWindowRange& out) {
    if (!align || (align & (align - 1)) != 0 || !most || va < lo || hi <= va) return false;
    const std::uint64_t want = (std::min)(bytes ? bytes : std::uint64_t(1), most);
    const std::uint64_t start = (std::max)(va & ~(align - 1), lo);
    const std::uint64_t raw_end = want > UINT64_MAX - va ? UINT64_MAX : va + want;
    std::uint64_t end = raw_end > UINT64_MAX - (align - 1) ? UINT64_MAX : (raw_end + align - 1) & ~(align - 1);
    end = (std::min)(end, hi);
    if (end <= va || end <= start) return false;
    const TitleWindowRange range{start, end - start};
    if (!title_window_fits(cap, used, range.size)) return false;
    out = range;
    return true;
}

// The extent of a stream pool one mapping serves for the request at `va`: from its page to the end of the readable
// run (`readable_end`, exclusive), at most `most` bytes, inside the pool [pool_lo, pool_hi). The same extent comes
// back for every request inside it, so each extent costs one mapping, not one per request.
inline bool title_pool_extent(std::uint64_t pool_lo, std::uint64_t pool_hi, std::uint64_t va,
                              std::uint64_t readable_end, std::uint64_t align, std::uint64_t most,
                              TitleWindowRange& out) {
    if (!align || (align & (align - 1)) != 0 || pool_hi <= pool_lo || va < pool_lo || va >= pool_hi ||
        readable_end <= va)
        return false;
    const std::uint64_t start = (std::max)(va & ~(align - 1), pool_lo);
    std::uint64_t end = (std::min)(readable_end, pool_hi);
    end = end > UINT64_MAX - (align - 1) ? UINT64_MAX : (end + align - 1) & ~(align - 1);
    if (most) end = (std::min)(end, start > UINT64_MAX - most ? UINT64_MAX : start + most);
    end = (std::min)(end, pool_hi);
    if (end <= va || end <= start) return false;
    out = {start, end - start};
    return true;
}

inline bool clip_title_pool_extent(TitleWindowRange& extent, std::uint64_t source, std::uint64_t twin,
                                   std::uint64_t source_lo, std::uint64_t source_hi,
                                   std::uint64_t twin_lo, std::uint64_t twin_hi) {
    if (source < source_lo || source >= source_hi || twin < twin_lo || twin >= twin_hi ||
        source < extent.start || source - extent.start >= extent.size)
        return false;
    const auto prefix = source - extent.start;
    const auto before = std::min({prefix, source - source_lo, twin - twin_lo});
    const auto after = std::min({extent.size - prefix, source_hi - source, twin_hi - twin});
    extent = {source - before, before + after};
    return true;
}

// The one mapping a stream-pool request [va, end) gets when what would serve it stops short: the extent clipped to
// the free span (clip_title_pool_extent) ends at the next mapping, or `va` lies in a mapping that ends before `end`.
// It starts at `start` (that extent's start, or `va`) and covers the request and, whole, every range the result
// overlaps in either view (`twin`: va's byte in the pair's other view, `va` when there is none), page-aligned at its
// end. A range entered over it replaces those ranges whole (TitleVaAliases::replace_twin), so none of their bytes is
// left unanswered and no view reaches past the copy it was given. `owned(shader_va)` says the caller can retire the
// copy a range answers from (a title window); any other range in the way (a slot-133 buffer, a geometry copy) makes
// it false, as does a mapping past `most` bytes or outside [pool_lo, pool_hi) in its own view. The pool's other view
// is [twin_lo, twin_hi), `twin` being va's byte there; one that would not fit in 64 bits makes it false too, so no
// address is ever taken modulo 2^64. `out` is unchanged on false.
template <class Owned>
inline bool title_pool_cover(const TitleVaAliases& aliases, std::uint64_t pool_lo, std::uint64_t pool_hi,
                             std::uint64_t va, std::uint64_t twin, std::uint64_t start, std::uint64_t end,
                             std::uint64_t align, std::uint64_t most, Owned&& owned, TitleWindowRange& out) {
    if (!align || (align & (align - 1)) != 0 || start > va || va >= end || start < pool_lo || end > pool_hi)
        return false;
    if (twin < va - pool_lo || pool_hi - va > UINT64_MAX - twin) return false;
    const auto aligned_end = [&](std::uint64_t e) {
        return e > pool_hi - (align - 1) ? pool_hi : (std::min)(pool_hi, (e + align - 1) & ~(align - 1));
    };
    // Each view's bounds: a range at x in the twin view is at x - twin_lo + pool_lo in va's.
    const std::uint64_t base[2] = {pool_lo, twin - (va - pool_lo)}, top[2] = {pool_hi, twin + (pool_hi - va)};
    std::uint64_t lo = start, hi = aligned_end(end);
    std::vector<TitleVaAliases::Mapping> hits;
    for (bool grew = true; grew;) {
        grew = false;
        for (int view = 0; view < (twin != va ? 2 : 1); ++view) {
            aliases.overlapping(lo - pool_lo + base[view], hi - pool_lo + base[view], hits);
            for (const TitleVaAliases::Mapping& m : hits) {
                // Not ours, or not coverable: reaching past the pool in its own view.
                if (!owned(m.shader_va) || m.title_va < base[view] || m.end > top[view]) return false;
                const std::uint64_t b = m.title_va - base[view] + pool_lo, e = m.end - base[view] + pool_lo;
                if (b < lo) lo = b, grew = true;
                if (e > hi) hi = aligned_end(e), grew = true;
            }
        }
        if (lo < pool_lo || hi > pool_hi || hi - lo > most) return false;
    }
    out = {lo, hi - lo};
    return true;
}

}  // namespace leonida
