// Portable CPU-only unit tests for runtime/src/title_texture_refresh.h (pure C++20 stdlib, no GPU, no game files).
// Covers merge_title_writes (empty, degenerate, sorted, unsorted, adjacent, nested), title_write_overlaps (O(1)
// envelope / single-range fast path and multi-range binary search), title_texture_snapshot, and poll_title_textures
// (resume cursor, mapped/stale skip, budget exhaustion, and explicit UINT64_MAX wrap).
#include "../src/title_texture_refresh.h"

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <vector>

#define CHECK(cond) do {                                                        \
    if (!(cond)) {                                                              \
        std::fprintf(stderr, "line %d: CHECK failed: %s\n", __LINE__, #cond);   \
        return 1;                                                               \
    }                                                                           \
} while (0)

namespace {

struct FakeTexture {
    std::uint64_t id = 0;
    bool mapped = false;
    bool stale = false;
};

}  // namespace

int main() {
    using namespace leonida;

    // 1. merge_title_writes: empty, single valid/degenerate, sorted, unsorted, adjacent, nested.
    {
        TitleWriteRanges empty;
        merge_title_writes(empty);
        CHECK(empty.empty());

        TitleWriteRanges single_bad{{0x2000, 0x2000}};
        merge_title_writes(single_bad);
        CHECK(single_bad.empty());

        TitleWriteRanges single_inv{{0x3000, 0x1000}};
        merge_title_writes(single_inv);
        CHECK(single_inv.empty());

        TitleWriteRanges single_ok{{0x1000, 0x2000}};
        merge_title_writes(single_ok);
        CHECK(single_ok.size() == 1 && single_ok[0] == std::make_pair(std::uintptr_t{0x1000}, std::uintptr_t{0x2000}));

        // Already-sorted input with adjacent & nested ranges.
        TitleWriteRanges sorted{{0x1000, 0x2000}, {0x2000, 0x3000}, {0x2500, 0x2800}, {0x5000, 0x5000}, {0x6000, 0x7000}};
        merge_title_writes(sorted);
        CHECK(sorted.size() == 2);
        CHECK(sorted[0] == std::make_pair(std::uintptr_t{0x1000}, std::uintptr_t{0x3000}));
        CHECK(sorted[1] == std::make_pair(std::uintptr_t{0x6000}, std::uintptr_t{0x7000}));

        // Unsorted overlapping input.
        TitleWriteRanges unsorted{{0x8000, 0x9000}, {0x1000, 0x4000}, {0x2000, 0x3000}, {0x3500, 0x5000}, {0x9000, 0x8000}};
        merge_title_writes(unsorted);
        CHECK(unsorted.size() == 2);
        CHECK(unsorted[0] == std::make_pair(std::uintptr_t{0x1000}, std::uintptr_t{0x5000}));
        CHECK(unsorted[1] == std::make_pair(std::uintptr_t{0x8000}, std::uintptr_t{0x9000}));
    }

    // 2. title_write_overlaps: empty, degenerate query, single-range fast path, envelope reject, multi-range boundaries.
    {
        const TitleWriteRanges empty;
        CHECK(!title_write_overlaps(empty, 0x1000, 0x2000));

        const TitleWriteRanges one{{0x4000, 0x6000}};
        CHECK(!title_write_overlaps(one, 0x4000, 0x4000));  // empty query
        CHECK(!title_write_overlaps(one, 0x5000, 0x4000));  // inverted query
        CHECK(!title_write_overlaps(one, 0x1000, 0x4000));  // half-open touching left
        CHECK(!title_write_overlaps(one, 0x6000, 0x7000));  // half-open touching right
        CHECK(title_write_overlaps(one, 0x3fff, 0x4001));   // overlaps left edge
        CHECK(title_write_overlaps(one, 0x5fff, 0x6001));   // overlaps right edge
        CHECK(title_write_overlaps(one, 0x4500, 0x5500));   // inside
        CHECK(title_write_overlaps(one, 0x1000, 0x9000));   // encloses

        const TitleWriteRanges multi{{0x1000, 0x2000}, {0x4000, 0x5000}, {0x8000, 0x9000}};
        CHECK(!title_write_overlaps(multi, 0x0, 0x1000));
        CHECK(!title_write_overlaps(multi, 0x9000, 0x10000));
        CHECK(!title_write_overlaps(multi, 0x2000, 0x4000));
        CHECK(!title_write_overlaps(multi, 0x5000, 0x8000));
        CHECK(title_write_overlaps(multi, 0x2000, 0x4001));
        CHECK(title_write_overlaps(multi, 0x4fff, 0x8000));
        CHECK(title_write_overlaps(multi, 0x2500, 0x8500));
    }

    // 3. title_texture_snapshot & poll_title_textures: cursor wrap, mapped/stale skips, budget exhaustion, UINT64_MAX.
    {
        std::map<std::uint64_t, std::shared_ptr<FakeTexture>> reg;
        reg[0x1000] = std::make_shared<FakeTexture>(FakeTexture{1, false, false});
        reg[0x2000] = std::make_shared<FakeTexture>(FakeTexture{2, true, false});   // mapped -> skipped
        reg[0x3000] = std::make_shared<FakeTexture>(FakeTexture{3, false, true});   // stale -> skipped
        reg[0x4000] = std::make_shared<FakeTexture>(FakeTexture{4, false, false});
        reg[UINT64_MAX] = std::make_shared<FakeTexture>(FakeTexture{5, false, false});

        const auto snap = title_texture_snapshot(reg);
        CHECK(snap.size() == 5);

        std::uint64_t cursor = 0;
        std::vector<std::uint64_t> visited;
        // Budget stops after visiting 1st entry (advances at least one entry even when exhausted immediately).
        poll_title_textures(snap, cursor, [&](FakeTexture& t) { visited.push_back(t.id); }, [] { return true; });
        CHECK(visited.size() == 1 && visited[0] == 1 && cursor == 0x1001);

        // Next pass starts at 0x2000 (skips mapped, skips stale, visits 4 and 5, wraps cursor at UINT64_MAX -> 0).
        visited.clear();
        poll_title_textures(snap, cursor, [&](FakeTexture& t) { visited.push_back(t.id); }, [] { return false; });
        // Visits 0x4000, UINT64_MAX, then wraps inside the loop to 0x1000 (leaving cursor at 0x1001).
        CHECK(visited.size() == 3 && visited[0] == 4 && visited[1] == 5 && visited[2] == 1 && cursor == 0x1001);

        // Stopping right on UINT64_MAX sets cursor explicitly to 0 so the next pass starts at 0x1000.
        cursor = UINT64_MAX;
        visited.clear();
        poll_title_textures(snap, cursor, [&](FakeTexture& t) { visited.push_back(t.id); }, [] { return true; });
        CHECK(visited.size() == 1 && visited[0] == 5 && cursor == 0u);

        visited.clear();
        poll_title_textures(snap, cursor, [&](FakeTexture& t) { visited.push_back(t.id); }, [] { return true; });
        CHECK(visited.size() == 1 && visited[0] == 1 && cursor == 0x1001);
    }

    std::printf("title_texture_refresh_test: all checks passed\n");
    return 0;
}
