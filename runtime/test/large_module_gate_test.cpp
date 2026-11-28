// vk_pipeline_check's memory budget for large modules (large_module_gate.h), without a Vulkan device.
#include "large_module_gate.h"

#include <atomic>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

using leonida::LargeModuleGate;
using leonida::LargeModuleQueue;

static int budget_and_count() {
    LargeModuleGate gate(10, 3);
    CHECK(gate.admits(4));
    gate.enter(4);
    CHECK(gate.admits(4));
    gate.enter(4);
    CHECK(!gate.admits(4));   // 12 > 10
    CHECK(gate.admits(2));    // 10 fits exactly
    gate.enter(2);
    CHECK(!gate.admits(0));   // three in: max_inflight
    gate.leave(4);
    CHECK(gate.admits(4));
    gate.leave(4);
    gate.leave(2);
    CHECK(gate.inflight() == 0);
    return 0;
}

static int oversized_module_still_gets_in_alone() {
    // Before: a module whose estimate exceeds the whole budget would wait forever.
    LargeModuleGate gate(10, 4);
    CHECK(gate.admits(100));
    gate.enter(100);
    CHECK(!gate.admits(1));   // nothing joins it
    gate.leave(100);
    CHECK(gate.admits(1));
    return 0;
}

static int max_inflight_zero_means_one() {
    LargeModuleGate gate(100, 0);
    CHECK(gate.max_inflight() == 1);
    gate.enter(1);
    CHECK(!gate.admits(1));
    gate.leave(1);
    return 0;
}

static int ticket_releases() {
    LargeModuleGate gate(10, 2);
    {
        LargeModuleGate::Ticket a(gate, 6);
        CHECK(gate.inflight() == 1);
        LargeModuleGate::Ticket moved = std::move(a);   // the moved-from ticket must not leave twice
        CHECK(gate.inflight() == 1);
        LargeModuleGate::Ticket b;
        b = LargeModuleGate::Ticket(gate, 4);
        CHECK(gate.inflight() == 2);
        b.release();
        CHECK(gate.inflight() == 1);
    }
    CHECK(gate.inflight() == 0);
    return 0;
}

static int threads_never_exceed_the_budget() {
    constexpr uint64_t kBudget = 10;
    LargeModuleGate gate(kBudget, 3);
    std::atomic<uint64_t> used{0};
    std::atomic<unsigned> in{0};
    std::atomic<bool> over{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < 200; ++i) {
                const uint64_t est = 1 + uint64_t((t + i) % 5);   // 1..5
                LargeModuleGate::Ticket ticket(gate, est);
                const uint64_t u = used += est;
                const unsigned n = ++in;
                if (n > 3 || (n > 1 && u > kBudget)) over = true;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                --in;
                used -= est;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(!over);
    CHECK(gate.inflight() == 0);
    return 0;
}

// A worker of vk_pipeline_check over modules given by their estimate (0: a small one): enter(k) is module k going
// into the driver, while its claim (and ticket) is held.
template <class Enter>
static void run_worker(LargeModuleQueue& queue, const std::vector<uint64_t>& estimates, Enter enter) {
    while (auto claim = queue.take()) {
        const uint64_t estimate = estimates[claim->index];
        if (estimate && !queue.admit(*claim, estimate)) continue;
        enter(claim->index);
    }
}

static int a_waiting_large_module_frees_its_worker() {
    // Before: the worker took module 0 and waited in the gate behind another worker's large module, and the small
    // modules 1 and 2 waited untaken until that one was done.
    LargeModuleGate gate(10, 4);
    LargeModuleGate::Ticket other(gate, 8);   // another worker's large module, in the driver
    const std::vector<uint64_t> estimates{6, 0, 0};   // 8 + 6 > 10
    LargeModuleQueue queue(gate, estimates.size());
    std::mutex mu;
    std::condition_variable cv;
    std::vector<size_t> entered, while_other_in;
    std::thread worker([&] {
        run_worker(queue, estimates, [&](size_t k) {
            {
                std::lock_guard<std::mutex> g(mu);
                entered.push_back(k);
            }
            cv.notify_all();
        });
    });
    {
        std::unique_lock<std::mutex> l(mu);
        cv.wait_for(l, std::chrono::seconds(10), [&] { return entered.size() == 2; });
        while_other_in = entered;
    }
    other.release();
    worker.join();
    CHECK((while_other_in == std::vector<size_t>{1, 2}));
    CHECK((entered == std::vector<size_t>{1, 2, 0}));   // and module 0 once the other one was done
    CHECK(gate.inflight() == 0);
    return 0;
}

static int the_oldest_module_put_back_goes_first() {
    LargeModuleGate gate(10, 4);
    LargeModuleQueue queue(gate, 4);
    LargeModuleGate::Ticket other(gate, 8);
    auto a = queue.take();
    CHECK(a && a->index == 0 && !queue.admit(*a, 6));   // 8 + 6 > 10: put back
    a.reset();
    auto b = queue.take();
    CHECK(b && b->index == 1 && !queue.admit(*b, 2));   // 8 + 2 fits, but module 0 waits ahead of it
    b.reset();
    auto c = queue.take();
    CHECK(c && c->index == 2 && !c->ticket);           // a small module meanwhile
    c.reset();
    other.release();
    a = queue.take();                                  // before the new module 3
    CHECK(a && a->index == 0 && a->ticket && gate.inflight() == 1);
    CHECK(queue.admit(*a, 6) && gate.inflight() == 1);   // it came back with its place: not counted twice
    b = queue.take();
    CHECK(b && b->index == 1 && b->ticket && gate.inflight() == 2);   // 6 + 2 fits
    c = queue.take();
    CHECK(c && c->index == 3 && !c->ticket);
    CHECK(!queue.admit(*c, 4) && gate.inflight() == 2);   // 6 + 2 + 4 > 10
    c.reset();
    a.reset();
    c = queue.take();
    CHECK(c && c->index == 3 && c->ticket && gate.inflight() == 2);   // 2 + 4 fits
    b.reset();
    c.reset();
    CHECK(!queue.take());
    CHECK(gate.inflight() == 0);
    return 0;
}

static int nothing_is_put_back_while_the_gate_admits() {
    // One worker: no other large module is in when one comes, so even one over the budget gets in and the order is
    // the old one.
    LargeModuleGate gate(10, 2);
    const std::vector<uint64_t> estimates{6, 0, 100, 6, 0, 9};
    LargeModuleQueue queue(gate, estimates.size());
    std::vector<size_t> entered;
    run_worker(queue, estimates, [&](size_t k) { entered.push_back(k); });
    CHECK((entered == std::vector<size_t>{0, 1, 2, 3, 4, 5}));
    // --large-parallel 2 (a count, no memory budget) with a place left beside another worker's large module.
    LargeModuleGate counted(UINT64_MAX, 2);
    LargeModuleGate::Ticket other(counted, 8);
    LargeModuleQueue roomy(counted, 3);
    entered.clear();
    run_worker(roomy, {6, 0, 6}, [&](size_t k) { entered.push_back(k); });
    CHECK((entered == std::vector<size_t>{0, 1, 2}));
    other.release();
    // The gate disabled (--large-module-bytes 0) or --large-parallel 1 (its exclusive lock): admit() is never asked,
    // and the queue hands out 0, 1, 2 as the old counter did.
    LargeModuleQueue counter(gate, 3);
    for (size_t k = 0; k < 3; ++k) {
        auto claim = counter.take();
        CHECK(claim && claim->index == k && !claim->ticket);
    }
    CHECK(!counter.take());
    CHECK(gate.inflight() == 0 && counted.inflight() == 0);
    return 0;
}

static int threads_check_every_module_once() {
    constexpr uint64_t kBudget = 10;
    LargeModuleGate gate(kBudget, 3);
    std::vector<uint64_t> estimates(300);
    for (size_t k = 0; k < estimates.size(); ++k)
        estimates[k] = k % 3 ? 0 : 1 + (k * 7) % 12;   // a third large, 1..12: some over the budget alone
    LargeModuleQueue queue(gate, estimates.size());
    std::mutex mu;
    std::vector<int> entered(estimates.size());
    std::atomic<uint64_t> used{0};
    std::atomic<unsigned> in{0};
    std::atomic<bool> over{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&] {
            run_worker(queue, estimates, [&](size_t k) {
                {
                    std::lock_guard<std::mutex> g(mu);
                    ++entered[k];
                }
                const uint64_t est = estimates[k];
                if (!est) return;
                const uint64_t u = used += est;
                const unsigned n = ++in;
                if (n > 3 || (n > 1 && u > kBudget)) over = true;
                std::this_thread::sleep_for(std::chrono::microseconds(50));
                --in;
                used -= est;
            });
        });
    }
    for (auto& th : threads) th.join();
    for (const int n : entered) CHECK(n == 1);   // every module once, none lost while put back
    CHECK(!over);
    CHECK(gate.inflight() == 0);
    return 0;
}

static int estimate_and_memory_query() {
    CHECK(leonida::large_module_estimate(4u << 20) == (uint64_t(4) << 20) * leonida::kBytesPerSpirvByte);
#if defined(__linux__) || defined(_WIN32)
    CHECK(leonida::available_memory_bytes() > 0);   // Linux: MemAvailable; Windows: ullAvailPhys
#endif
    leonida::release_free_memory();   // must be callable anywhere
    return 0;
}

#if defined(__linux__)
#include <filesystem>
#include <fstream>

static void write_file(const std::filesystem::path& p, const char* text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

static int cgroup_limits_are_read_up_the_tree() {
    // Before: only MemAvailable was read, so under systemd-run -p MemoryMax=... (or in a container) the budget was
    // the whole machine's and the checker could be killed by its own cgroup.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / ("lmg_test_" + std::to_string(::getpid()));
    fs::remove_all(root);
    write_file(root / "self", "0::/user.slice/app.scope\n");
    write_file(root / "cg/user.slice/memory.max", "10000\n");
    write_file(root / "cg/user.slice/memory.current", "4000\n");          // room 6000
    write_file(root / "cg/user.slice/app.scope/memory.max", "max\n");     // no limit of its own
    write_file(root / "cg/user.slice/app.scope/memory.current", "3000\n");
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == 6000);
    write_file(root / "cg/user.slice/app.scope/memory.max", "5000\n");    // room 2000, the tighter one
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == 2000);
    write_file(root / "cg/user.slice/app.scope/memory.current", "7000\n"); // over its limit: no room
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == 0);
    // A mount rooted at a constrained cgroup still exposes its budget at the mount root.
    write_file(root / "cg/user.slice/app.scope/memory.max", "max\n");
    write_file(root / "cg/memory.max", "5000\n");
    write_file(root / "cg/memory.current", "3000\n");
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == 2000);
    write_file(root / "self", "0::/\n");
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == 2000);
    write_file(root / "cg/memory.max", "max\n");
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == UINT64_MAX);
    write_file(root / "self", "12:memory:/legacy\n");                      // cgroup v1 only: no limit read
    CHECK(leonida::cgroup_memory_room((root / "self").string(), (root / "cg").string()) == UINT64_MAX);
    CHECK(leonida::cgroup_memory_room((root / "missing").string(), (root / "cg").string()) == UINT64_MAX);
    fs::remove_all(root);
    return 0;
}
#endif

int main() {
#if defined(__linux__)
    if (int r = cgroup_limits_are_read_up_the_tree()) return r;
#endif
    if (int r = budget_and_count()) return r;
    if (int r = oversized_module_still_gets_in_alone()) return r;
    if (int r = max_inflight_zero_means_one()) return r;
    if (int r = ticket_releases()) return r;
    if (int r = threads_never_exceed_the_budget()) return r;
    if (int r = a_waiting_large_module_frees_its_worker()) return r;
    if (int r = the_oldest_module_put_back_goes_first()) return r;
    if (int r = nothing_is_put_back_while_the_gate_admits()) return r;
    if (int r = threads_check_every_module_once()) return r;
    if (int r = estimate_and_memory_query()) return r;
    std::puts("large_module_gate: ok");
    return 0;
}
