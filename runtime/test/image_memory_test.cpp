// ImageMemoryPool and BlockRanges (runtime/src/image_memory.h): textures sharing blocks of device memory. CPU only, with
// counted fake allocations: fixed cases for the block policy, then random requests checked against a list of the live
// ranges (no overlap, aligned, inside the block, and all of it usable again when everything is released).

#include "../src/image_memory.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace {

using leonida::BlockRanges;
using Pool = leonida::ImageMemoryPool<std::uintptr_t>;
constexpr std::uint64_t kMiB = std::uint64_t(1) << 20;

int failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

// A driver: hands out handles 1, 2, 3..., refuses when told, and notes what it freed. The pool frees a block outside
// its own lock, so the driver takes one, as vkAllocateMemory and vkFreeMemory may run on several threads.
struct Driver {
    std::mutex mutex;
    std::uintptr_t next = 1;
    std::set<std::uintptr_t> live;
    int allocations = 0;
    int frees = 0;
    bool full = false;

    Pool pool() {
        return Pool([this](std::uint32_t, std::uint64_t) -> std::uintptr_t {
            std::lock_guard lock(mutex);
            ++allocations;
            if (full) return 0;
            live.insert(next);
            return next++;
        }, [this](std::uintptr_t memory) {
            std::lock_guard lock(mutex);
            ++frees;
            check(live.erase(memory) == 1, "a block freed that was not live");
        });
    }
};

void block_ranges() {
    BlockRanges ranges(1000);
    std::uint64_t a = 0, b = 0, c = 0, d = 0;
    bool reused = true;
    check(ranges.allocate(100, 1, a, reused) && a == 0 && !reused, "the first range starts at 0 and is fresh");
    check(ranges.allocate(1, 256, b, reused) && b == 256, "a range starts at its alignment");
    check(ranges.allocate(700, 1, c, reused) && c == 257, "the best fitting free piece is used");
    check(!ranges.allocate(500, 1, d, reused), "a range larger than any free piece is refused");
    check(!ranges.allocate(0, 1, d, reused) && !ranges.allocate(1001, 1, d, reused),
          "a zero or an oversized range is refused");
    ranges.release(a, 100);
    ranges.release(c, 700);
    ranges.release(b, 1);
    ranges.release(0, 0);  // zero-size release is a safe no-op
    check(ranges.empty() && ranges.largest() == 1000, "released ranges merge back into one piece");
    check(ranges.allocate(1000, 1, d, reused) && d == 0 && reused, "a range over earlier bytes is marked as reused");

    // Alignment overflow safety: huge alignment on a non-zero piece must not wrap uint64_t below piece.
    BlockRanges big(1000);
    std::uint64_t p0 = 0, p1 = 0;
    check(big.allocate(10, 1, p0, reused) && p0 == 0, "occupy offset 0 so remaining free piece starts at 10");
    check(!big.allocate(10, std::numeric_limits<std::uint64_t>::max(), p1, reused),
          "UINT64_MAX alignment on non-zero piece refuses cleanly without wraparound");
    check(!big.allocate(10, (std::uint64_t(1) << 63) + 1u, p1, reused),
          "large non-power-of-two alignment on non-zero piece refuses cleanly");
    check(!big.allocate(10, 0, p1, reused), "zero alignment refused");
    check(big.allocate(10, 16, p1, reused) && p1 == 16, "normal alignment after overflow attempts still succeeds");
}

void shared_blocks() {
    Driver driver;
    {
        Pool pool = driver.pool();
        std::vector<std::unique_ptr<Pool::Range>> ranges;
        for (int i = 0; i < 64; ++i) ranges.push_back(pool.allocate(2, kMiB, 65536, 64 * kMiB));
        check(driver.allocations == 1 && pool.stats().blocks == 1 && pool.stats().ranges == 64 &&
                  pool.stats().block_bytes == 64 * kMiB,
              "64 one-MiB textures fill one block");
        auto extra = pool.allocate(2, kMiB, 65536, 64 * kMiB);
        check(driver.allocations == 2, "the 65th makes a second block");
        check(extra && !extra->reused(), "a range of a new block is fresh");
        const auto other = pool.allocate(3, kMiB, 65536, 64 * kMiB);
        check(driver.allocations == 3 && other->memory() != extra->memory(), "memory types do not share blocks");
        extra.reset();
        check(driver.frees == 0 && pool.stats().blocks == 3, "an emptied block is kept for the next textures");
        ranges.clear();
        check(driver.frees == 1 && pool.stats().blocks == 2, "only one empty block is kept per memory type");
        const auto again = pool.allocate(2, kMiB, 65536, 64 * kMiB);
        check(driver.allocations == 3 && again && again->reused(), "the kept block is used again, and marked as reused");
    }
    check(driver.live.empty(), "the pool frees its blocks");
}

void a_full_heap() {
    Driver driver;
    Pool pool = driver.pool();
    driver.full = true;
    check(!pool.allocate(0, kMiB, 1024, 64 * kMiB), "a driver without room gives no range");
    const int asked = driver.allocations;
    for (int i = 0; i < 64; ++i) check(!pool.allocate(0, kMiB, 1024, 64 * kMiB), "and goes on giving none");
    check(driver.allocations == asked, "a full heap is not asked again at every texture");
    check(!pool.allocate(0, kMiB, 1024, 64 * kMiB) && driver.allocations == asked + 1, "but is asked again later");
    check(!pool.allocate(0, 2 * kMiB, 1024, kMiB) && driver.allocations == asked + 1,
          "a texture larger than a block is the caller's own allocation");

    Driver other;
    Pool second = other.pool();
    auto held = second.allocate(0, kMiB, 1024, 2 * kMiB);
    other.full = true;
    auto filling = second.allocate(0, kMiB, 1024, 2 * kMiB);
    check(filling && other.allocations == 1, "a full heap still serves the block's own free space");
    check(!second.allocate(0, kMiB, 1024, 2 * kMiB) && other.allocations == 2, "and refuses once it is used up");
    filling.reset();
    check(second.allocate(0, kMiB, 1024, 2 * kMiB) != nullptr, "a released range is usable again at once");
    held.reset();
}

void random_requests() {
    std::mt19937 random(1274);
    for (int round = 0; round < 20; ++round) {
        Driver driver;
        Pool pool = driver.pool();
        struct Live {
            std::unique_ptr<Pool::Range> range;
            std::uint64_t size;
            std::uint64_t alignment;
        };
        std::vector<Live> live;
        for (int step = 0; step < 4000; ++step) {
            if (live.empty() || random() % 100 < 55) {
                const std::uint64_t alignment = std::uint64_t(1) << (8 + random() % 9);
                const std::uint64_t size = 1 + random() % (2 * kMiB);
                auto range = pool.allocate(1, size, alignment, 16 * kMiB);
                check(range != nullptr, "a driver with room serves a small range");
                if (!range) return;
                check(range->offset() % alignment == 0 && range->offset() + size <= 16 * kMiB, "aligned, inside the block");
                for (const auto& l : live)
                    check(l.range->memory() != range->memory() || l.range->offset() >= range->offset() + size ||
                              range->offset() >= l.range->offset() + l.size,
                          "live ranges do not overlap");
                live.push_back({std::move(range), size, alignment});
            } else {
                const std::size_t i = random() % live.size();
                live[i] = std::move(live.back());
                live.pop_back();
            }
        }
        check(pool.stats().ranges == live.size(), "the range count follows");
        live.clear();
        check(pool.stats().ranges == 0 && pool.stats().blocks == 1, "everything released leaves one spare block");
        const auto whole = pool.allocate(1, 16 * kMiB, 1, 16 * kMiB);
        check(whole && whole->offset() == 0, "the whole block is free again");
    }
}

void threads() {
    Driver driver;
    {
        Pool pool = driver.pool();
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < 4; ++t)
            workers.emplace_back([&pool, t] {
                std::mt19937 random(t);
                std::vector<std::unique_ptr<Pool::Range>> mine;
                for (int i = 0; i < 2000; ++i) {
                    if (mine.size() < 16) mine.push_back(pool.allocate(0, 1 + random() % kMiB, 4096, 16 * kMiB));
                    else mine.erase(mine.begin() + random() % mine.size());
                }
            });
        for (auto& worker : workers) worker.join();
        check(pool.stats().ranges == 0, "concurrent use leaves no range behind");
    }
    check(driver.live.empty(), "nor a block");
}

// Verify mutex_ is NOT held across allocate_() so concurrent stats(), release(), and allocations on existing
// blocks can complete while a slow driver allocation is in flight (would deadlock if mutex_ were held).
void allocate_runs_unlocked() {
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool in_alloc2 = false;
    bool allow_alloc2 = false;
    std::atomic<int> allocs{0};

    Pool pool(
        [&](std::uint32_t, std::uint64_t) -> std::uintptr_t {
            const int n = ++allocs;
            if (n == 2) {
                std::unique_lock lock(gate_mu);
                in_alloc2 = true;
                gate_cv.notify_all();
                gate_cv.wait(lock, [&] { return allow_alloc2; });
            }
            return static_cast<std::uintptr_t>(n);
        },
        [](std::uintptr_t) {});

    // Populate memory type 0 with one half-full block.
    auto r1 = pool.allocate(0, kMiB, 4096, 4 * kMiB);
    check(r1 != nullptr && allocs.load() == 1, "first block allocated");

    // Start a blocking driver allocation on memory type 1 in a background thread.
    std::unique_ptr<Pool::Range> r_other;
    std::thread slow_thread([&] { r_other = pool.allocate(1, kMiB, 4096, 4 * kMiB); });
    {
        std::unique_lock lock(gate_mu);
        gate_cv.wait(lock, [&] { return in_alloc2; });
    }

    // While slow_thread is inside allocate_(), another thread can allocate from existing block, read stats(), and release.
    auto r2 = pool.allocate(0, kMiB, 4096, 4 * kMiB);
    check(r2 != nullptr && r2->memory() == r1->memory(), "existing block serves allocations while driver alloc runs");
    const auto st = pool.stats();
    check(st.blocks == 1 && st.ranges == 2, "stats() completes while driver alloc is in flight");
    r2.reset();
    check(pool.stats().ranges == 1, "release() completes while driver alloc is in flight");

    {
        std::lock_guard lock(gate_mu);
        allow_alloc2 = true;
    }
    gate_cv.notify_all();
    slow_thread.join();
    check(r_other != nullptr && pool.stats().blocks == 2 && pool.stats().ranges == 2,
          "slow driver allocation finishes and registers its block");
    r_other.reset();
    r1.reset();
}

}  // namespace

int main() {
    block_ranges();
    shared_blocks();
    a_full_heap();
    random_requests();
    threads();
    allocate_runs_unlocked();
    if (!failures) std::printf("image memory: textures share blocks, and released ranges are used again\n");
    return failures ? 1 : 0;
}
