// The mapping a title address gets, without a GPU: src/title_window_plan.h (a window's range and what fits the
// LEONIDA_TITLE_WINDOWS_MB budget; one mapping per readable run of a stream pool) and src/title_va_alias.h with a
// resolver that runs out of budget (no raw address may come back) and with a pool's two addresses (src/title_page_watch.h,
// title_gpu_views) sharing one mapping. The title's stream pool is a 16-24 GiB reservation whose GPU-only alias and CPU view name the same
// frames, and a title address that gets no copy must not reach a shader as itself (heap_base + it is no allocation).
#include "../src/title_page_watch.h"
#include "../src/title_va_alias.h"
#include "../src/title_window_plan.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

constexpr std::uint64_t kAlign = 65536, kMiB = 1ull << 20, kArenaCap = 1ull << 32;

// A resolver that makes a window when the budget has room and answers "no backing" when it does not, as the device's
// resolve_title_memory does (d3d12_vulkan_device.cpp).
struct Budget {
    std::uint64_t cap = 64 * kMiB;
    std::uint64_t used = 0;
    std::uint64_t next = 0x10000;  // synthetic arena offsets
    std::uint64_t windows = 0, refusals = 0;
    leonida::TitleVaAliases* table = nullptr;
};

bool budget_resolver(void* context, std::uint64_t va, std::uint64_t bytes, std::uint64_t& out) {
    auto& budget = *static_cast<Budget*>(context);
    leonida::TitleWindowRange range{};
    if (!leonida::title_window_range(va, bytes, 0, UINT64_MAX, kAlign, 16 * kMiB, budget.cap, budget.used, range)) {
        ++budget.refusals;
        out = 0;  // title memory, no backing: the view is written with a zero-size range
        return true;
    }
    budget.used += range.size;
    ++budget.windows;
    const std::uint64_t shader = budget.next;
    budget.next += range.size;
    budget.table->add(range.start, range.size, shader);
    out = shader + (va - range.start);
    return true;
}

int check_window_budget() {
    Budget budget;
    leonida::TitleVaAliases table;
    budget.table = &table;
    table.set_hooks(&budget, &budget_resolver, nullptr);
    constexpr std::uint64_t title = 0x5800e0000ull;  // a stream-pool alias on the console, past the arena
    std::uint64_t backed = 0, zeroed = 0;
    for (unsigned i = 0; i < 800; ++i) {
        const std::uint64_t va = title + std::uint64_t(i) * 0x20000;
        const std::uint64_t to = table.resolve(va, 0x20000, leonida::TitleVaAliases::vertex_view);
        // Never the raw title address: a window (an offset below the arena's range), or 0 for a zero-size view.
        EXPECT(to == 0 || to < kArenaCap);
        EXPECT(to != va);
        if (to) ++backed;
        else ++zeroed;
    }
    EXPECT(backed != 0 && zeroed != 0);
    EXPECT(budget.windows == budget.cap / 0x20000);  // 512 aligned 128 KiB windows fill 64 MiB exactly
    EXPECT(budget.used == budget.cap);
    EXPECT(budget.refusals >= zeroed);
    // A window that does not fit is refused outright: no range and no charge.
    leonida::TitleWindowRange range{};
    EXPECT(!leonida::title_window_range(title, 0x20000, 0, UINT64_MAX, kAlign, 16 * kMiB, budget.cap, budget.cap,
                                        range));
    EXPECT(leonida::title_window_range(title, 0x10000, 0, UINT64_MAX, kAlign, 16 * kMiB, budget.cap,
                                       budget.cap - 0x10000, range));
    EXPECT(range.size == 0x10000);
    // A span that ends before the request backs nothing.
    EXPECT(!leonida::title_window_range(title, 0x20000, 0, title, kAlign, 16 * kMiB, budget.cap, 0, range));
    table.clear_hooks(&budget);
    std::printf("title window budget: %llu windows backed 800 requests, %llu zero-size views, %llu KiB of a %llu MiB "
                "budget, no raw address\n",
                static_cast<unsigned long long>(budget.windows), static_cast<unsigned long long>(zeroed),
                static_cast<unsigned long long>(budget.used >> 10), static_cast<unsigned long long>(budget.cap >> 20));
    return 0;
}

// One stream pool's pages: an RW CPU view (the pages the title commits and the host reads) and a GPU-only alias of
// the same size, both uncommitted at the alias, as the title's xmem reservations are (protect 0x801).
int check_stream_pool() {
    constexpr std::uint64_t pool = 1 * kMiB;
    void* cpu_pages = VirtualAlloc(nullptr, pool, MEM_TOP_DOWN | MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    void* gpu_alias = VirtualAlloc(nullptr, pool, MEM_TOP_DOWN | MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(cpu_pages && gpu_alias);
    const auto cpu = reinterpret_cast<std::uintptr_t>(cpu_pages);
    const auto gpu = reinterpret_cast<std::uintptr_t>(gpu_alias);
    EXPECT(!leonida::title_gpu_views::note_reservation(cpu, pool, false));  // remembered as the CPU twin
    EXPECT(leonida::title_gpu_views::note_reservation(gpu, pool, true));    // paired: true

    // The pair is found from either address, and rule B sends the GPU-only pages to the CPU view the host reads.
    leonida::title_gpu_views::Range view{};
    EXPECT(leonida::title_gpu_views::view_at(gpu + 0x20000, 0x20000, view) && view.gpu == gpu && view.cpu == cpu);
    EXPECT(leonida::title_gpu_views::view_at(cpu + 0x20000, 0x20000, view) && view.gpu == gpu && view.cpu == cpu);
    EXPECT(leonida::title_gpu_views::to_host_view(gpu + 0x20000, 0x20000) == cpu + 0x20000);
    EXPECT(leonida::title_gpu_views::to_host_view(cpu + 0x20000, 0x20000) == cpu + 0x20000);
    EXPECT(leonida::title_gpu_views::other_view(gpu + 0x20000) == cpu + 0x20000);

    // One mapping for the readable run: two vertex-buffer requests inside it compute the same extent, so they cost
    // one window, not one 128 KiB window each.
    const std::uint64_t readable_end = cpu + pool;
    leonida::TitleWindowRange first{}, second{};
    EXPECT(leonida::title_pool_extent(cpu, cpu + pool, cpu + 0x20000, readable_end, kAlign, 1 * kMiB, first));
    EXPECT(leonida::title_pool_extent(cpu, cpu + pool, cpu + 0x28000, readable_end, kAlign, 1 * kMiB, second));
    EXPECT(first.start == cpu + 0x20000 && first.size == 0xE0000);
    EXPECT(second.start == first.start && second.size == first.size);  // one mapping for both requests
    // The extent stops at the first page the title has not committed.
    leonida::TitleWindowRange partial{};
    EXPECT(leonida::title_pool_extent(cpu, cpu + pool, cpu + 0x20000, cpu + 0x30000, kAlign, 1 * kMiB, partial));
    EXPECT(partial.start == cpu + 0x20000 && partial.size == 0x10000);
    EXPECT(!leonida::title_pool_extent(cpu, cpu + pool, cpu + 0x20000, cpu + 0x20000, kAlign, 1 * kMiB, partial));

    // The mapping the device makes: one shader VA for the extent, entered for both addresses of the pair.
    leonida::TitleVaAliases table;
    constexpr std::uint64_t shader = 0x3000000;
    const auto twin_start = static_cast<std::uint64_t>(leonida::title_gpu_views::other_view(first.start));
    EXPECT(table.add_twin(first.start, twin_start, first.size, shader));
    for (std::uint64_t off = 0; off < pool; off += 0x40000) {
        EXPECT(table.shader_va(first.start + off) == shader + off);      // the CPU view
        EXPECT(table.shader_va(twin_start + off) == shader + off);       // the GPU-only alias: the same backing
        EXPECT(table.resolve(first.start + off, 0x20000, leonida::TitleVaAliases::vertex_view) == shader + off);
        EXPECT(table.resolve(twin_start + off, 0x20000, leonida::TitleVaAliases::vertex_view) == shader + off);
    }
    // One mapping, two ranges: 8 lookups on both sides called no resolver and made no second mapping.
    EXPECT(table.translated(leonida::TitleVaAliases::vertex_view) == 8);
    EXPECT(table.size() == 2);

    leonida::title_gpu_views::state().views.clear();
    leonida::title_gpu_views::state().count.store(0);
    VirtualFree(gpu_alias, 0, MEM_RELEASE);
    VirtualFree(cpu_pages, 0, MEM_RELEASE);
    std::printf("stream pool: alias 0x%llx and CPU view 0x%llx share one %llu KiB mapping; 8 lookups, no second "
                "mapping\n",
                static_cast<unsigned long long>(gpu), static_cast<unsigned long long>(cpu),
                static_cast<unsigned long long>(first.size >> 10));
    return 0;
}

// A request whose bytes run into the next mapping gets one mapping for all of them, not a zero-size view: the
// extent made for it stops at that mapping (clip_title_pool_extent), and a view given less than its bytes would reach
// past its copy. The cover takes the windows it reaches whole, in either view, and nothing else. Made-up addresses:
// nothing is mapped here.
int check_pool_cover() {
    constexpr std::uint64_t cpu = 0x7ff612340000ull, gpu = 0x5612340000ull, pool = 4 * kMiB;
    const auto window = [](std::uint64_t shader_va) { return shader_va >= 0x100000 && shader_va < 0x800000; };
    leonida::TitleVaAliases table;
    leonida::TitleWindowRange cover{}, unused{};
    // An extent 512 KiB in, 256 KiB long, at both of the pair's addresses.
    EXPECT(table.add_twin(cpu + 0x80000, gpu + 0x80000, 0x40000, 0x100000));
    // A request 128 KiB before it for 192 KiB: from its own page, over the first extent, whole, in both views.
    EXPECT(leonida::title_pool_cover(table, cpu, cpu + pool, cpu + 0x60000, gpu + 0x60000, cpu + 0x60000,
                                     cpu + 0x90000, kAlign, kMiB, window, cover));
    EXPECT(cover.start == cpu + 0x60000 && cover.size == 0x60000);
    // A request that starts inside the first extent and ends past it: the same, from the extent's start, and an end
    // that is not page-aligned is rounded up.
    EXPECT(leonida::title_pool_cover(table, cpu, cpu + pool, cpu + 0xB0000, gpu + 0xB0000, cpu + 0xB0000,
                                     cpu + 0xD0010, kAlign, kMiB, window, cover));
    EXPECT(cover.start == cpu + 0x80000 && cover.size == 0x60000);
    // add_twin refuses to enter it over the first extent; replace_twin replaces that one's ranges whole, and every
    // byte they answered for is answered again.
    EXPECT(!table.add_twin(cover.start, gpu + 0x80000, cover.size, 0x200000));
    // A buffer the title places in the cover's span in either view after the plan (slot 133 takes no lock of the
    // planner's) is not erased with the windows: replace_twin refuses and leaves every range as it was.
    for (const std::uint64_t placed : {cpu + 0xC0000, gpu + 0xC8000}) {
        leonida::TitleVaAliases raced;
        EXPECT(raced.add_twin(cpu + 0x80000, gpu + 0x80000, 0x40000, 0x100000));
        EXPECT(leonida::title_pool_cover(raced, cpu, cpu + pool, cpu + 0xB0000, gpu + 0xB0000, cpu + 0xB0000,
                                         cpu + 0xD0010, kAlign, kMiB, window, cover));
        EXPECT(raced.add(placed, 0x4000, 0x900000));
        EXPECT(!raced.replace_twin(cover.start, gpu + 0x80000, cover.size, 0x200000, window));
        EXPECT(raced.size() == 3 && raced.shader_va(placed + 0x10) == 0x900010);
        EXPECT(raced.shader_va(cpu + 0x80000) == 0x100000 && raced.shader_va(gpu + 0xBFFFF) == 0x13FFFF);
    }
    EXPECT(table.replace_twin(cover.start, gpu + 0x80000, cover.size, 0x200000, window));
    EXPECT(table.size() == 2);
    EXPECT(table.shader_va(cpu + 0xBFFFF) == 0x23FFFF && table.shader_va(gpu + 0xD000F) == 0x25000F);
    // Past `most`, or into a range that is not a window (a buffer placed in the GPU-only view), there is no cover.
    EXPECT(!leonida::title_pool_cover(table, cpu, cpu + pool, cpu + 0x70000, gpu + 0x70000, cpu + 0x70000,
                                      cpu + 0x90000, kAlign, 0x40000, window, unused));
    EXPECT(table.add(gpu + 0xE0000, 0x8000, 0x900000));
    EXPECT(!leonida::title_pool_cover(table, cpu, cpu + pool, cpu + 0xD8000, gpu + 0xD8000, cpu + 0xD8000,
                                      cpu + 0xE4000, kAlign, kMiB, window, unused));
    std::printf("stream pool: a request crossing into the next extent gets one 384 KiB mapping over it, whole; none "
                "over a buffer or past the cap\n");
    return 0;
}

// The twin view is bounded like the pool's own, and never computed modulo 2^64. Made-up addresses.
int check_pool_cover_bounds() {
    const auto window = [](std::uint64_t shader_va) { return shader_va >= 0x100000 && shader_va < 0x800000; };
    leonida::TitleWindowRange cover{};
    {
        // Pool [0x100000, 0x500000), twin view [0x400000, 0x800000). A window at [0x200000, 0x440000) reaches the
        // twin view from below it: covering it would erase it whole and leave [0x200000, 0x420000) unanswered.
        leonida::TitleVaAliases table;
        EXPECT(table.add(0x200000, 0x240000, 0x100000));
        EXPECT(!leonida::title_pool_cover(table, 0x100000, 0x500000, 0x120000, 0x420000, 0x120000, 0x130000, kAlign,
                                          kMiB, window, cover));
        // And at the top: a window at [0x7F0000, 0x810000) runs past the twin view's end.
        leonida::TitleVaAliases top;
        EXPECT(top.add(0x7F0000, 0x20000, 0x100000));
        EXPECT(!leonida::title_pool_cover(top, 0x100000, 0x500000, 0x4E0000, 0x7E0000, 0x4E0000, 0x4F8000, kAlign,
                                          kMiB, window, cover));
    }
    {
        // A twin view that would start below 0: the pool's first 3 MiB sit before address 0 there, so the request's
        // span in it wraps. Refused, not looked up as an empty range past the window at [0, 0x20000).
        leonida::TitleVaAliases table;
        EXPECT(table.add(0, 0x20000, 0x100000));
        EXPECT(!leonida::title_pool_cover(table, 0x10000000, 0x10400000, 0x10300000, 0x100000, 0x10000000,
                                          0x10310000, kAlign, 4 * kMiB, window, cover));
    }
    {
        // One that would end past 2^64: the request's span there wraps, and a buffer in it (not a window) was missed.
        leonida::TitleVaAliases table;
        constexpr std::uint64_t twin = 0xFFFFFFFFFFF00000ull;
        EXPECT(table.add(twin + 0x80000, 0x10000, 0x900000));
        EXPECT(!leonida::title_pool_cover(table, 0x100000, 0x500000, 0x100000, twin, 0x100000, 0x200000, kAlign, kMiB,
                                          window, cover));
    }
    std::printf("stream pool: no cover over a mapping past the twin view, nor over a twin view past 64 bits\n");
    return 0;
}

}  // namespace

int main() {
    if (check_window_budget()) return 1;
    if (check_stream_pool()) return 1;
    if (check_pool_cover()) return 1;
    if (check_pool_cover_bounds()) return 1;
    std::printf("title_window_plan: a refused window gives no raw base, and a stream pool's two views share one "
                "mapping\n");
    return 0;
}
