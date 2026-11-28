#pragma once
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "title_page_watch.h"

namespace leonida {

// Keep negative probes until the title changes the map or a writer reports this footprint. With cache_success,
// positive ones too: a caller that is told of every map change over the footprint (retry or sync_map) and reads it
// under a guard (a page gone since faults into a defer) need not walk the pages before every read.
class TextureReadability {
public:
    template<class Probe>
    bool check(std::uint64_t generation, bool cache_failure, Probe probe, bool cache_success = false) {
        if (cache_failure && pending_ && !retry_ && generation_ == generation) return false;
        if (cache_success && probed_ && !pending_ && !retry_ && generation_ == generation) return true;
        generation_ = generation;  // snapshot before the probe so a concurrent map change is noticed
        map_generation_ = title_memory_map::generation();
        retry_ = false;
        probed_ = true;
        pending_ = !probe();
        return !pending_;
    }
    // Between refresh passes, check whether the map journal recorded a change over [cpu, cpu + size) since both
    // this texture's last probe and the last pass's sweep.
    void sync_map(std::uint64_t pass_generation, std::uintptr_t cpu, std::uint64_t size) {
        if (!probed_ || retry_) return;
        const std::uint64_t since = (std::max)(map_generation_, pass_generation);
        if (title_memory_map::generation() == since) return;
        const std::uintptr_t end = size > (std::numeric_limits<std::uintptr_t>::max)() - cpu
                                       ? (std::numeric_limits<std::uintptr_t>::max)()
                                       : cpu + static_cast<std::uintptr_t>(size);
        std::uint64_t now = 0;
        bool overlapped = false;
        if (!title_memory_map::changes_since(
                since, now, cpu, end, [&](std::uintptr_t, std::uintptr_t) { overlapped = true; }) ||
            overlapped) {
            retry_ = true;
        } else {
            map_generation_ = now;
        }
    }
    void defer() { pending_ = true; }  // mapping disappeared between the probe and the read
    void retry() { retry_ = true; }
    bool pending() const { return pending_; }
private:
    std::uint64_t generation_ = 0;
    std::uint64_t map_generation_ = 0;
    bool pending_ = false, retry_ = false, probed_ = false;
};

using TexturePageCommitter = bool (*)(const void*, std::size_t);

// How many of the bytes from `base` are readable, up to the first page that is not (at most `size`).
inline std::uint64_t texture_memory_readable_bytes(std::uintptr_t base, std::uint64_t size,
                                                   TexturePageCommitter commit = nullptr) {
    if (!base || !size || size > (std::numeric_limits<std::uintptr_t>::max)() - base) return 0;
    const auto end = base + size;
    for (auto at = base; at < end;) {
        MEMORY_BASIC_INFORMATION region{};
        const auto* address = reinterpret_cast<const void*>(at);
        if (!VirtualQuery(address, &region, sizeof(region))) return at - base;
        // A persistent Map can name an xmem section whose physical pages commit on first touch.
        // Only xmem may commit them. A bare reservation or an inaccessible page must still defer.
        if (commit && region.State == MEM_RESERVE && region.Type == MEM_MAPPED) {
            if (!commit(address, end - at) || !VirtualQuery(address, &region, sizeof(region))) return at - base;
        }
        if (region.State != MEM_COMMIT || (region.Protect & PAGE_GUARD)) return at - base;
        switch (region.Protect & 0xff) {
        case PAGE_READONLY: case PAGE_READWRITE: case PAGE_WRITECOPY:
        case PAGE_EXECUTE_READ: case PAGE_EXECUTE_READWRITE: case PAGE_EXECUTE_WRITECOPY: break;
        default: return at - base;
        }
        const auto start = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
        if (at < start || at - start >= region.RegionSize) return at - base;
        const auto available = region.RegionSize - (at - start);
        if (available >= end - at) return size;
        at += available;
    }
    return size;
}

// Check the whole footprint. A committed first page says nothing about a streamed mip farther on.
inline bool texture_memory_readable(std::uintptr_t base, std::uint64_t size, TexturePageCommitter commit = nullptr) {
    return size && texture_memory_readable_bytes(base, size, commit) == size;
}

}  // namespace leonida
