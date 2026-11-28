// Portable boundary, alignment, and overflow tests for src/title_window_plan.h.
// No Windows headers, no Vulkan, no game data.
#include "../src/title_window_plan.h"

#include <cstdint>
#include <cstdio>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

constexpr std::uint64_t kAlign = 0x10000;  // 64 KiB
constexpr std::uint64_t kMiB = 1ull << 20;

int test_title_window_fits() {
    EXPECT(leonida::title_window_fits(100, 0, 100));
    EXPECT(leonida::title_window_fits(100, 40, 60));
    EXPECT(!leonida::title_window_fits(100, 40, 61));
    EXPECT(!leonida::title_window_fits(100, 101, 0));
    EXPECT(!leonida::title_window_fits(UINT64_MAX, UINT64_MAX - 10, 11));
    EXPECT(leonida::title_window_fits(UINT64_MAX, UINT64_MAX - 10, 10));
    return 0;
}

int test_title_window_range_bounds() {
    leonida::TitleWindowRange range{};
    constexpr std::uint64_t va = 0x5800e0000ull;

    // va < lo must be rejected: returning [lo, end) when lo > va would not contain va and would underflow
    // va - range.start in resolve_title_memory.
    EXPECT(!leonida::title_window_range(va, 0x20000, va + 0x1000, va + 0x40000, kAlign, 16 * kMiB, 64 * kMiB, 0,
                                        range));
    EXPECT(!leonida::title_window_range(va, 0x10000, va + 0x10000, va + 0x40000, kAlign, 16 * kMiB, 64 * kMiB, 0,
                                        range));

    // most == 0 must be rejected cleanly (not passed to std::clamp(want, 1, 0)).
    EXPECT(!leonida::title_window_range(va, 0x10000, 0, UINT64_MAX, kAlign, 0, 64 * kMiB, 0, range));

    // Zero or non-power-of-two align must be rejected.
    EXPECT(!leonida::title_window_range(va, 0x10000, 0, UINT64_MAX, 0, 16 * kMiB, 64 * kMiB, 0, range));
    EXPECT(!leonida::title_window_range(va, 0x10000, 0, UINT64_MAX, 3, 16 * kMiB, 64 * kMiB, 0, range));
    EXPECT(!leonida::title_window_range(va, 0x10000, 0, UINT64_MAX, 0x30000, 16 * kMiB, 64 * kMiB, 0, range));

    // Large want + align near UINT64_MAX must saturate end to UINT64_MAX without underflowing UINT64_MAX - want - align.
    const std::uint64_t high_va = UINT64_MAX - 0x1000;
    EXPECT(leonida::title_window_range(high_va, UINT64_MAX, 0, UINT64_MAX, kAlign, UINT64_MAX, UINT64_MAX, 0, range));
    EXPECT(range.start == (high_va & ~(kAlign - 1)));
    EXPECT(range.start <= high_va && high_va < range.start + range.size);
    EXPECT(range.start + range.size == UINT64_MAX);

    // Normal aligned and clipped spans still cover va and charge the budget accurately.
    EXPECT(leonida::title_window_range(va + 0x1234, 0, va, va + 0x20000, kAlign, 16 * kMiB, 64 * kMiB, 0, range));
    EXPECT(range.start == va && range.size == kAlign);
    EXPECT(!leonida::title_window_range(va, 0x10000, 0, UINT64_MAX, kAlign, 16 * kMiB, 64 * kMiB, 64 * kMiB, range));
    return 0;
}

int test_title_pool_extent_bounds() {
    leonida::TitleWindowRange extent{};
    constexpr std::uint64_t pool_lo = 0x100000000ull;
    constexpr std::uint64_t pool_hi = pool_lo + kMiB;

    // Unreadable va (readable_end <= va) must be rejected even when rounding readable_end up to align would pass va.
    EXPECT(!leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x28000, pool_lo + 0x21000, kAlign, kMiB, extent));
    EXPECT(!leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x20000, pool_lo + 0x20000, kAlign, kMiB, extent));

    // Non-power-of-two align must be rejected.
    EXPECT(!leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x20000, pool_hi, 0, kMiB, extent));
    EXPECT(!leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x20000, pool_hi, 0x30000, kMiB, extent));

    // When `most` is smaller than `align` (or pool_lo is unaligned), the returned extent must still respect `most`
    // and contain `va`.
    EXPECT(leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x20000, pool_hi, kAlign, 0x4000, extent));
    EXPECT(extent.start == pool_lo + 0x20000 && extent.size == 0x4000);
    // If start + most cannot even reach va, refuse instead of returning a range excluding va.
    EXPECT(!leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x28000, pool_hi, kAlign, 0x4000, extent));

    // Two requests in the same aligned page and readable run produce the exact same extent.
    leonida::TitleWindowRange first{}, second{};
    EXPECT(leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x20000, pool_hi, kAlign, kMiB, first));
    EXPECT(leonida::title_pool_extent(pool_lo, pool_hi, pool_lo + 0x28000, pool_hi, kAlign, kMiB, second));
    EXPECT(first.start == pool_lo + 0x20000 && first.size == 0xE0000);
    EXPECT(second.start == first.start && second.size == first.size);

    const auto source = pool_lo + 0x28000;
    const auto twin = source + 2 * kMiB;
    EXPECT(leonida::clip_title_pool_extent(first, source, twin, source - 0x4000, source + 0x60000,
                                          twin - 0x10000, twin + 0x10000));
    EXPECT(first.start == source - 0x4000 && first.size == 0x14000);
    EXPECT(!leonida::clip_title_pool_extent(second, source, twin, source + 1, pool_hi,
                                           twin - 0x10000, twin + 0x10000));
    EXPECT(!leonida::clip_title_pool_extent(second, source, twin, pool_lo, pool_hi, twin, twin));
    EXPECT(second.start == pool_lo + 0x20000 && second.size == 0xE0000);
    return 0;
}

}  // namespace

int main() {
    if (int r = test_title_window_fits()) return r;
    if (int r = test_title_window_range_bounds()) return r;
    if (int r = test_title_pool_extent_bounds()) return r;
    std::printf("title_window_plan_bounds_test: ok\n");
    return 0;
}
