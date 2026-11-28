#pragma once
// Buffer addresses in the title's own memory and the console VA a translated shader reaches them by
// (docs/d3d12_vulkan.md section 3.3, "Title-chosen addresses").
//
// A translated shader computes heap_base + console_va, with heap_base the GPU address arena's device address. The
// layer chooses every console VA it hands out, so they all land in the arena. The title does not always use them:
//   - device slot 133 places a buffer at the CPU address of the title's own pages (a user-mode Windows address, far above 4 GiB);
//   - the title points V#s and root views at its own memory (a per-frame constant ring, #8862 section 3.5).
// Both are far past the arena, so heap_base + VA reads nothing the layer made. An address inside such a range is
// therefore replaced, wherever the layer can see it, by the console VA of a GPU copy of that range: its arena offset
// when the copy is an arena range (a slot-133 shadow, or a window of the title's pages copied before every
// submission), or its device address minus heap_base (mod 2^64) when it has its own memory (an imported slot-133
// buffer). Addresses that are not in the title's memory are passed through unchanged.
//
// The table is process-wide, like the title's address space. The device behind it registers two hooks: a resolver
// that makes a GPU copy for an address in the title's memory that has none yet, and a scanner that rewrites the
// buffer V#s of a bound descriptor table before a submission. Without hooks the table only translates what was
// added to it, so it is tested without a GPU (title_va_alias_test).
#include <algorithm>
#include "buffer_va_index.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace leonida {

class TitleVaAliases {
public:
    // Where the layer saw the address, for the counters.
    enum Kind : unsigned { root_view, created_view, table_view, vertex_view, index_view, bvh_address, kind_count };
    static constexpr const char* kKindNames[kind_count] = {"root view", "created view", "table V#", "vertex view",
                                                           "index view", "BVH build address"};

    // An address in the title's memory: true, with `shader_va` what a shader must be given (0 when no copy can be
    // made: the view is then written with a zero-size range, never with the raw address). False: not the title's
    // memory (an arena VA, a buffer of the layer's).
    using Resolver = bool (*)(void* context, std::uint64_t va, std::uint64_t bytes, std::uint64_t& shader_va);
    using TableScanner = void (*)(void* context, std::uint64_t table_va, std::uint32_t descriptors);
    // Every binding resolved to a copy of the title's memory: [va, va + bytes) as they are reached at (the device keeps
    // them for the copy back after the submission, see DeviceState::write_back_titles).
    using Noter = void (*)(void* context, std::uint64_t va, std::uint64_t bytes);

    // [title_va, title_va + bytes) is reached as [shader_va, shader_va + bytes). A range that overlaps a live one
    // replaces it (the title has placed a new buffer over pages it released). False for an empty or wrapping range.
    bool add(std::uint64_t title_va, std::uint64_t bytes, std::uint64_t shader_va) {
        if (!bytes || title_va > UINT64_MAX - bytes) return false;
        std::unique_lock lock(mutex_);
        erase_overlapping(title_va, title_va + bytes);
        notify(Change::added, title_va, bytes, shader_va);
        ranges_[title_va] = {title_va + bytes, shader_va};
        by_shader_.insert(shader_va, bytes, title_va);
        count_.store(ranges_.size(), std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // The two addresses of one stream-pool extent (a GPU-only alias and the CPU view of the same pages, gfxgeom
    // section 3): both name the same shader VA, so a title view carrying either resolves to the one mapping.
    bool add_twin(std::uint64_t va, std::uint64_t twin, std::uint64_t bytes, std::uint64_t shader_va) {
        if (!bytes || va > UINT64_MAX - bytes || twin > UINT64_MAX - bytes) return false;
        if (va != twin && va < twin + bytes && twin < va + bytes) return false;
        std::unique_lock lock(mutex_);
        if (overlaps(va, va + bytes) || (va != twin && overlaps(twin, twin + bytes))) return false;
        decltype(ranges_) pending;
        pending.try_emplace(va, Range{va + bytes, shader_va});
        pending.try_emplace(twin, Range{twin + bytes, shader_va});
        publish_pool_ranges(pending);
        return true;
    }

    // add_twin over live ranges: every range either address's bytes overlap is replaced whole (each is told as a
    // removal), then both are entered under one lock. For a stream-pool request that runs into the windows next to it
    // (title_pool_cover): the new range covers each of them whole, so none of their bytes is left unanswered.
    // `replaces(shader_va)` names the ranges the caller planned to replace; one it does not (a slot-133 buffer
    // entered since the plan, which takes no lock of the caller's) makes it false with nothing changed, checked
    // under the same lock as the replacement.
    template <class Replaces>
    bool replace_twin(std::uint64_t va, std::uint64_t twin, std::uint64_t bytes, std::uint64_t shader_va,
                      Replaces&& replaces) {
        if (!bytes || va > UINT64_MAX - bytes || twin > UINT64_MAX - bytes) return false;
        if (va != twin && va < twin + bytes && twin < va + bytes) return false;
        std::unique_lock lock(mutex_);
        const auto planned = [&](std::uint64_t begin, std::uint64_t end) {
            auto it = ranges_.lower_bound(begin);
            if (it != ranges_.begin() && std::prev(it)->second.end > begin) --it;
            for (; it != ranges_.end() && it->first < end; ++it)
                if (!replaces(it->second.shader_va)) return false;
            return true;
        };
        if (!planned(va, va + bytes) || (va != twin && !planned(twin, twin + bytes))) return false;
        erase_overlapping(va, va + bytes);
        if (va != twin) erase_overlapping(twin, twin + bytes);
        decltype(ranges_) pending;
        pending.try_emplace(va, Range{va + bytes, shader_va});
        pending.try_emplace(twin, Range{twin + bytes, shader_va});
        publish_pool_ranges(pending);
        return true;
    }

    bool reuse_pool_mapping(std::uint64_t va, std::uint64_t twin, std::uint64_t want, std::uint64_t max_size,
                            std::uint64_t& shader_va) {
        if (!want) return false;
        std::unique_lock lock(mutex_);
        const auto next = ranges_.upper_bound(va);
        if (next != ranges_.begin()) {
            const auto existing = std::prev(next);
            if (va < existing->second.end) {
                if (want > existing->second.end - va) return false;
                shader_va = existing->second.shader_va + (va - existing->first);
                return true;
            }
        }
        const auto other_next = ranges_.upper_bound(twin);
        if (other_next == ranges_.begin()) return false;
        const auto other = std::prev(other_next);
        if (twin >= other->second.end) return false;
        const auto end = next == ranges_.end() ? UINT64_MAX : next->first;
        const auto bytes = std::min({max_size, other->second.end - twin, end - va});
        if (bytes < want) return false;
        const auto shader = other->second.shader_va + (twin - other->first);
        decltype(ranges_) pending;
        pending.try_emplace(va, Range{va + bytes, shader});
        publish_pool_ranges(pending);
        shader_va = shader;
        return true;
    }

    // Forgets the range that starts at title_va, if it still maps to shader_va (a newer range at the same VA keeps
    // its own entry).
    void remove(std::uint64_t title_va, std::uint64_t shader_va) {
        std::unique_lock lock(mutex_);
        const auto it = ranges_.find(title_va);
        if (it != ranges_.end() && it->second.shader_va == shader_va) {
            notify(Change::removed, it->first, it->second.end - it->first, shader_va);
            by_shader_.erase(shader_va, title_va);
            note_removed(it->second.shader_va, it->second.end - it->first);
            ranges_.erase(it);
            generation_.fetch_add(1, std::memory_order_release);
        }
        count_.store(ranges_.size(), std::memory_order_release);
    }

    // A copy that answered for [title_va, title_va + bytes) at shader_va went, whether or not it had a range here (a
    // geometry copy that vsharp_address or a placed view reached through GeometryViews directly has none): what was
    // translated through it is stale. Told as a removal is: the console heaps fix the slots that resolved through
    // either side of it again (the listeners), and the GPU key table drops answers into shader_va (take_removed).
    void forget_copy(std::uint64_t title_va, std::uint64_t bytes, std::uint64_t shader_va) {
        if (!bytes) return;
        std::unique_lock lock(mutex_);
        for (auto it = ranges_.lower_bound(title_va); it != ranges_.end() && it->first - title_va < bytes;) {
            if (it->second.shader_va - shader_va != it->first - title_va) {
                ++it;
                continue;
            }
            by_shader_.erase(it->second.shader_va, it->first);
            notify(Change::removed, it->first, it->second.end - it->first, it->second.shader_va);
            it = ranges_.erase(it);
        }
        count_.store(ranges_.size(), std::memory_order_release);
        notify(Change::removed, title_va, bytes, shader_va);
        note_removed(shader_va, bytes);
        generation_.fetch_add(1, std::memory_order_release);
    }

    // The address a shader must be given for `va` from the ranges added so far: translated inside one, unchanged
    // elsewhere. No copy is made here.
    std::uint64_t shader_va(std::uint64_t va, std::uint64_t bytes = 1) const {
        if (!count_.load(std::memory_order_acquire)) return va;  // the common case: no title range at all
        std::shared_lock lock(mutex_);
        auto it = ranges_.upper_bound(va);
        if (it == ranges_.begin()) return va;
        --it;
        if (va >= it->second.end || bytes > it->second.end - va) return va;
        return it->second.shader_va + (va - it->first);
    }

    // The range that holds `va`: true with `shader_va` what `va` becomes and `available` the bytes from `va` to the
    // range's end, all reached through the one copy. False when no range holds `va`.
    bool range_of(std::uint64_t va, std::uint64_t& shader_va, std::uint64_t& available) const {
        if (!count_.load(std::memory_order_acquire)) return false;
        std::shared_lock lock(mutex_);
        auto it = ranges_.upper_bound(va);
        if (it == ranges_.begin()) return false;
        --it;
        if (va >= it->second.end) return false;
        shader_va = it->second.shader_va + (va - it->first);
        available = it->second.end - va;
        return true;
    }

    // Every range, in title-VA order (they never overlap): what a GPU pass that finds title addresses in memory
    // (the BLAS address of a TLAS build's instance descs) translates them with.
    struct Mapping {
        std::uint64_t title_va, end, shader_va;
    };
    void mappings(std::vector<Mapping>& out) const {
        out.clear();
        if (!count_.load(std::memory_order_acquire)) return;
        std::shared_lock lock(mutex_);
        out.reserve(ranges_.size());
        for (const auto& [start, range] : ranges_) out.push_back({start, range.end, range.shader_va});
    }
    // The ranges that overlap [begin, end), in title-VA order.
    void overlapping(std::uint64_t begin, std::uint64_t end, std::vector<Mapping>& out) const {
        out.clear();
        if (begin >= end || !count_.load(std::memory_order_acquire)) return;
        std::shared_lock lock(mutex_);
        auto it = ranges_.lower_bound(begin);
        if (it != ranges_.begin() && std::prev(it)->second.end > begin) --it;
        for (; it != ranges_.end() && it->first < end; ++it)
            out.push_back({it->first, it->second.end, it->second.shader_va});
    }

    // Whether `shader_va` lies in the copy of a range added so far: an address the layer itself wrote for the title
    // (a window, a geometry copy, a slot-133 shadow or import), which a later scan of the same descriptor sees again.
    // A binary search over the copies' addresses (source_of_vsharp walks every range).
    bool shader_side(std::uint64_t shader_va) const {
        if (!count_.load(std::memory_order_acquire)) return false;
        std::shared_lock lock(mutex_);
        BufferVaIndex<std::uint64_t>::Hit hit{};
        return by_shader_.find(shader_va, hit);
    }

    // Whether any range still answers from inside [shader_va, shader_va + bytes): a copy (a title window) that some
    // address still reaches. A range entered over a live one replaces it whole (add), so a copy can be left with none.
    bool answers_into(std::uint64_t shader_va, std::uint64_t bytes) const {
        if (!count_.load(std::memory_order_acquire)) return false;
        std::shared_lock lock(mutex_);
        return by_shader_.overlaps(shader_va, bytes);
    }

    // Debug-only inverse lookup. V#s retain 48 bits, so an import below heap_base wraps.
    bool source_of_vsharp(std::uint64_t base, std::uint64_t& title, std::uint64_t& available) const {
        constexpr std::uint64_t mask = (1ull << 48) - 1;
        std::shared_lock lock(mutex_);
        for (const auto& [start, range] : ranges_) {
            const auto delta = (base - range.shader_va) & mask;
            if (delta >= range.end - start) continue;
            title = start + delta;
            available = range.end - title;
            return true;
        }
        return false;
    }

    // The free span around `va` (not in any range): [lo, hi). A copy made for `va` must stay inside it, so it never
    // replaces a range another buffer still uses.
    void gap(std::uint64_t va, std::uint64_t& lo, std::uint64_t& hi) const {
        std::shared_lock lock(mutex_);
        lo = 0;
        hi = UINT64_MAX;
        auto it = ranges_.upper_bound(va);
        if (it != ranges_.end()) hi = it->first;
        if (it != ranges_.begin()) lo = std::prev(it)->second.end;
    }

    // shader_va, and when `va` is in the title's memory with no copy yet, a copy made by the device's resolver.
    // A resolver that cannot back the address (no window or geometry copy fits) answers 0: the view is then written
    // with a zero-size range, never with the raw title address (heap_base + a raw VA is no allocation a shader can
    // read or write). Counted by kind: translated, or in the title's memory and left as it was.
    std::uint64_t resolve(std::uint64_t va, std::uint64_t bytes, Kind kind) {
        if (!va) return va;
        std::uint64_t mapped = va, available = 0;
        const bool has_mapping = range_of(va, mapped, available) && mapped != va;
        if (has_mapping && bytes <= available) {
            translated_[kind].fetch_add(1, std::memory_order_relaxed);
            note_reached(va, bytes);
            return mapped;
        }
        Resolver resolver;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            resolver = resolver_;
            context = hook_context_;
        }
        std::uint64_t to = va;
        if (!resolver || !resolver(context, va, bytes, to)) {
            if (!has_mapping) return va;
            to = mapped;
        }
        (to != va ? translated_ : untranslated_)[kind].fetch_add(1, std::memory_order_relaxed);
        if (to && to != va) note_reached(va, bytes);
        return to;
    }

    // Counts an address that the layer resolves through its buffer index instead (vertex and index views): only
    // whether it is in a title range.
    void note(std::uint64_t va, Kind kind) {
        if (va && shader_va(va) != va) untranslated_[kind].fetch_add(1, std::memory_order_relaxed);
    }

    // [va, va + bytes) is reached as a copy of the title's memory: the device keeps it for the copy back.
    void note_reached(std::uint64_t va, std::uint64_t bytes) {
        Noter noter;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            noter = noter_;
            context = hook_context_;
        }
        if (noter) noter(context, va, bytes);
    }

    // Before a submission: the device rewrites the buffer V#s of the `descriptors` slots the root signature declares
    // for the table at `table_va` (a console VA in one of its shader-visible CBV/SRV/UAV heaps).
    void scan_table(std::uint64_t table_va, std::uint32_t descriptors) {
        TableScanner scanner;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            scanner = scanner_;
            context = hook_context_;
        }
        if (scanner) scanner(context, table_va, descriptors);
    }

    // The scan's cost, per ExecuteCommandLists that had tables to scan.
    void note_scan(std::uint64_t nanoseconds) {
        scans_.fetch_add(1, std::memory_order_relaxed);
        scan_ns_.fetch_add(nanoseconds, std::memory_order_relaxed);
        std::uint64_t seen = scan_max_ns_.load(std::memory_order_relaxed);
        while (nanoseconds > seen && !scan_max_ns_.compare_exchange_weak(seen, nanoseconds)) {
        }
    }
    std::uint64_t scans() const { return scans_.load(std::memory_order_relaxed); }
    std::uint64_t scan_ns() const { return scan_ns_.load(std::memory_order_relaxed); }
    std::uint64_t scan_max_ns() const { return scan_max_ns_.load(std::memory_order_relaxed); }

    // The title's own memory, as the xmem shims hand it out (XMemAlloc, XMemVirtualAlloc, the title's VirtualAlloc):
    // only an address in one of these ranges gets a copy. A base anywhere else (the runtime's own heap, a host DLL)
    // is left as it was and counted.
    void add_title_memory(std::uint64_t base, std::uint64_t bytes) {
        if (!base || !bytes || base > UINT64_MAX - bytes) return;
        std::uint64_t begin = base, end = base + bytes;
        std::unique_lock lock(memory_mutex_);
        // A commit inside a reserved range, or two that overlap, are one range.
        auto it = memory_.upper_bound(begin);
        if (it != memory_.begin() && std::prev(it)->second > begin) --it;
        while (it != memory_.end() && it->first < end) {
            begin = std::min(begin, it->first);
            end = std::max(end, it->second);
            it = memory_.erase(it);
        }
        memory_[begin] = end;
        memory_generation_.fetch_add(1, std::memory_order_release);
        notify(Change::memory_added, base, bytes, 0);
    }
    void remove_title_memory(std::uint64_t base) {
        std::unique_lock lock(memory_mutex_);
        memory_.erase(base);
        memory_generation_.fetch_add(1, std::memory_order_release);
    }
    // Bumped whenever title_memory_ranges() changes.
    std::uint64_t memory_generation() const { return memory_generation_.load(std::memory_order_acquire); }
    // LOCAL DIAGNOSTIC (mem probe): every title range, [begin, end), in address order.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> title_memory_ranges() const {
        std::shared_lock lock(memory_mutex_);
        return {memory_.begin(), memory_.end()};
    }
    // The title range that holds `va`: [lo, hi). False when none does.
    bool title_memory(std::uint64_t va, std::uint64_t& lo, std::uint64_t& hi) const {
        std::shared_lock lock(memory_mutex_);
        auto it = memory_.upper_bound(va);
        if (it == memory_.begin()) return false;
        --it;
        if (va >= it->second) return false;
        lo = it->first;
        hi = it->second;
        return true;
    }

    // A title that gives a descriptor heap its own backing memory (__XMemAllocationHooks__) writes descriptors there
    // itself, while shaders read the layer's heap. The layer that keeps that mapping registers a source: before a
    // bound table is scanned, it copies the slots the title wrote in its own memory over the layer's copy
    // ([real, real + bytes) is the layer heap's CPU memory). Returns how many slots it copied.
    using DescriptorSource = std::uint64_t (*)(void* context, std::uint8_t* real, std::size_t bytes);
    void set_descriptor_source(void* context, DescriptorSource source) {
        std::lock_guard lock(hook_mutex_);
        source_context_ = context;
        source_ = source;
    }
    void pull_descriptors(std::uint8_t* real, std::size_t bytes) {
        DescriptorSource source;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            source = source_;
            context = source_context_;
        }
        if (source) pulled_.fetch_add(source(context, real, bytes), std::memory_order_relaxed);
    }
    std::uint64_t pulled() const { return pulled_.load(std::memory_order_relaxed); }

    // The end of the first title range that overlaps [lo, hi), or 0 when none does. The GPU address arena keeps its
    // own allocations out of these ranges, so one number is never both an arena address and a title address.
    std::uint64_t title_overlap_end(std::uint64_t lo, std::uint64_t hi) const {
        std::shared_lock lock(memory_mutex_);
        auto it = memory_.upper_bound(lo);
        if (it != memory_.begin() && std::prev(it)->second > lo) return std::prev(it)->second;
        if (it != memory_.end() && it->first < hi) return it->second;
        return 0;
    }

    // A vertex or index buffer view at `va`. On the console every page of the title's memory is GPU memory, so a view
    // may name any address: the retail title binds vertex data that lives in its own CPU heap, with no D3D12 resource
    // over it. When `va` is in the title's memory the device's maker makes sure a buffer covers it (found with a
    // debugger: the address a draw could not resolve was a field of a vertex-buffer object in the title's heap,
    // docs/re/answers.md).
    using BufferMaker = void (*)(void* context, std::uint64_t va, std::uint64_t bytes);
    void set_buffer_maker(void* context, BufferMaker maker) {
        std::lock_guard lock(hook_mutex_);
        maker_context_ = context;
        maker_ = maker;
    }
    void clear_buffer_maker(void* context) {
        std::lock_guard lock(hook_mutex_);
        if (maker_context_ != context) return;
        maker_context_ = nullptr;
        maker_ = nullptr;
    }
    void want_buffer(std::uint64_t va, std::uint64_t bytes) {
        if (!va) return;
        std::uint64_t lo = 0, hi = 0;
        if (!title_memory(va, lo, hi)) return;
        BufferMaker maker;
        void* context;
        {
            std::lock_guard lock(hook_mutex_);
            maker = maker_;
            context = maker_context_;
        }
        if (maker) maker(context, va, bytes);
    }

    // The first device to register keeps the hooks until it clears them.
    void set_hooks(void* context, Resolver resolver, TableScanner scanner) {
        std::lock_guard lock(hook_mutex_);
        if (hook_context_) return;
        hook_context_ = context;
        resolver_ = resolver;
        scanner_ = scanner;
    }
    void clear_hooks(void* context) {
        std::lock_guard lock(hook_mutex_);
        if (hook_context_ != context) return;
        hook_context_ = nullptr;
        resolver_ = nullptr;
        scanner_ = nullptr;
        noter_ = nullptr;
    }
    // The resolver alone, for a device that scans its descriptor tables itself.
    void set_resolver(void* context, Resolver resolver) { set_hooks(context, resolver, nullptr); }
    void clear_resolver(void* context) { clear_hooks(context); }
    // Set after set_hooks, so the hook (and its context) stays the device's: what reached a copy of the title's memory.
    void set_noter(Noter noter) {
        std::lock_guard lock(hook_mutex_);
        noter_ = noter;
    }

    std::size_t size() const { return count_.load(std::memory_order_acquire); }

    // What changed, told as it happens to every listener (a device whose console heaps record what each descriptor
    // resolved to, src/console_heap_slot_records.h): [title_va, title_va + bytes) reached at shader_va was added, or
    // was removed or replaced (remove, an overlapping add, forget_copy: what resolved through it is stale), or title
    // memory was added (shader_va 0: a copy may now be made for an address that had none). Called with the table's
    // lock held, so a listener only records and never calls back into the table. remove_listener returns once no call
    // to that listener is in progress.
    enum class Change : unsigned { added, removed, memory_added };
    using Listener = void (*)(void* context, Change change, std::uint64_t title_va, std::uint64_t bytes,
                              std::uint64_t shader_va);
    void add_listener(void* context, Listener listener) {
        std::unique_lock lock(listener_mutex_);
        listeners_.emplace_back(context, listener);
    }
    void remove_listener(void* context) {
        std::unique_lock lock(listener_mutex_);
        std::erase_if(listeners_, [context](const auto& l) { return l.first == context; });
    }
    // Bumped by every add and every removal: a copy of mappings() is current while it has not moved.
    std::uint64_t generation() const { return generation_.load(std::memory_order_acquire); }
    // Diagnostics (fsdraw): the title VA a shader VA was made from, the reverse of translate; a linear scan.
    bool title_of_shader(std::uint64_t shader_va, std::uint64_t& title_va, std::uint64_t& range_va,
                         std::uint64_t& range_bytes) const {
        std::shared_lock lock(mutex_);
        for (const auto& [va, r] : ranges_)
            if (shader_va >= r.shader_va && shader_va - r.shader_va < r.end - va) {
                title_va = va + (shader_va - r.shader_va);
                range_va = va;
                range_bytes = r.end - va;
                return true;
            }
        return false;
    }
    // LEONIDA_CONSOLE_HEAP_GPU: the shader-VA ranges of the ranges removed (or replaced) since the last take, so a
    // GPU table translated through them can drop what it derived. Nothing is kept until recording is turned on.
    void record_removals(bool on) { record_removals_.store(on, std::memory_order_relaxed); }
    void take_removed(std::vector<std::pair<std::uint64_t, std::uint64_t>>& out) {
        out.clear();
        if (!record_removals_.load(std::memory_order_relaxed)) return;
        std::unique_lock lock(mutex_);
        out.swap(removed_);
    }
    void note_translation(Kind kind) { translated_[kind].fetch_add(1, std::memory_order_relaxed); }
    std::uint64_t translated(Kind kind) const { return translated_[kind].load(std::memory_order_relaxed); }
    std::uint64_t untranslated(Kind kind) const { return untranslated_[kind].load(std::memory_order_relaxed); }

private:
    struct Range {
        std::uint64_t end;        // exclusive
        std::uint64_t shader_va;  // what the range's first byte becomes
    };

    void publish_pool_ranges(std::map<std::uint64_t, Range>& pending) {
        try {
            for (const auto& [base, range] : pending) by_shader_.insert(range.shader_va, range.end - base, base);
        } catch (...) {
            for (const auto& [base, range] : pending) by_shader_.erase(range.shader_va, base);
            throw;
        }
        for (const auto& [base, range] : pending) notify(Change::added, base, range.end - base, range.shader_va);
        ranges_.merge(pending);
        count_.store(ranges_.size(), std::memory_order_release);
        generation_.fetch_add(1, std::memory_order_release);
    }

    bool overlaps(std::uint64_t begin, std::uint64_t end) const {
        const auto next = ranges_.lower_bound(begin);
        return (next != ranges_.end() && next->first < end) ||
               (next != ranges_.begin() && std::prev(next)->second.end > begin);
    }

    void erase_overlapping(std::uint64_t begin, std::uint64_t end) {
        auto it = ranges_.lower_bound(begin);
        if (it != ranges_.begin()) {
            auto previous = std::prev(it);
            if (previous->second.end > begin) it = previous;
        }
        while (it != ranges_.end() && it->first < end) {
            notify(Change::removed, it->first, it->second.end - it->first, it->second.shader_va);
            by_shader_.erase(it->second.shader_va, it->first);
            note_removed(it->second.shader_va, it->second.end - it->first);
            it = ranges_.erase(it);
        }
    }

    // mutex_ held exclusively.
    void note_removed(std::uint64_t shader_va, std::uint64_t bytes) {
        if (record_removals_.load(std::memory_order_relaxed)) removed_.push_back({shader_va, bytes});
    }

    void notify(Change change, std::uint64_t title_va, std::uint64_t bytes, std::uint64_t shader_va) const {
        std::shared_lock lock(listener_mutex_);
        for (const auto& [context, listener] : listeners_) listener(context, change, title_va, bytes, shader_va);
    }

    mutable std::shared_mutex listener_mutex_;  // taken inside mutex_ and memory_mutex_, never the other way
    std::vector<std::pair<void*, Listener>> listeners_;
    mutable std::shared_mutex mutex_;
    std::map<std::uint64_t, Range> ranges_;  // by title VA
    BufferVaIndex<std::uint64_t> by_shader_;  // the same ranges by shader VA (id: the title VA); copies may overlap
    std::atomic<std::size_t> count_{0};
    std::atomic<std::uint64_t> generation_{};
    std::atomic<bool> record_removals_{false};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> removed_;  // (shader VA, bytes), when recorded
    std::mutex hook_mutex_;
    void* hook_context_ = nullptr;
    void* maker_context_ = nullptr;
    BufferMaker maker_ = nullptr;
    Resolver resolver_ = nullptr;
    TableScanner scanner_ = nullptr;
    Noter noter_ = nullptr;
    void* source_context_ = nullptr;
    DescriptorSource source_ = nullptr;
    std::atomic<std::uint64_t> pulled_{0};
    std::array<std::atomic<std::uint64_t>, kind_count> translated_{};
    std::array<std::atomic<std::uint64_t>, kind_count> untranslated_{};
    std::atomic<std::uint64_t> scans_{0}, scan_ns_{0}, scan_max_ns_{0};
    mutable std::shared_mutex memory_mutex_;
    std::map<std::uint64_t, std::uint64_t> memory_;  // the title's memory: base -> end
    std::atomic<std::uint64_t> memory_generation_{};
};

// The process's table: the device's title ranges and hooks, read by the command lists and descriptor writers.
inline TitleVaAliases& title_va_aliases() {
    static TitleVaAliases aliases;
    return aliases;
}

// One diagnostic per descriptor-writing path. Called per descriptor write on the unbacked path, so the sites already
// told (string literals: their addresses) are looked up without the lock first.
inline void note_unbacked_view(const char* site, std::uint64_t va, std::uint64_t bytes) {
    static std::atomic<const char*> told[8] = {};
    for (const auto& t : told)
        if (t.load(std::memory_order_relaxed) == site) return;
    static std::mutex mutex;
    static std::map<std::string, bool> sites;
    {
        std::lock_guard lock(mutex);
        for (auto& t : told) {
            const char* none = nullptr;
            if (t.load(std::memory_order_relaxed) == site ||
                t.compare_exchange_strong(none, site, std::memory_order_relaxed))
                break;
        }
    }
    {
        std::lock_guard lock(mutex);
        if (!sites.emplace(site, true).second) return;
    }
    std::fprintf(stderr, "[rt] d3d12: %s: address 0x%llx (%llu bytes) has no GPU copy; zero records\n",
                 site, static_cast<unsigned long long>(va), static_cast<unsigned long long>(bytes));
}

}  // namespace leonida
