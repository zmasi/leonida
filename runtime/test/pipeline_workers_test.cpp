// PipelineWorkers (runtime/src/pipeline_workers.h): every job runs once and before run() returns, on the workers and on
// the caller; with no workers, after stop(), with several callers at once and when jobs throw. CPU only, no Vulkan.

#include "../src/pipeline_workers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Thread-local allocation failure injector so only the thread calling run() hits injected OOMs without disturbing
// worker threads or concurrent tests.
thread_local int g_fail_alloc_after = -1;
thread_local bool g_fail_all_allocs = false;

struct OomScope {
    explicit OomScope(int fail_after) {
        g_fail_alloc_after = fail_after;
        g_fail_all_allocs = false;
    }
    struct FailAll {};
    explicit OomScope(FailAll) {
        g_fail_alloc_after = -1;
        g_fail_all_allocs = true;
    }
    ~OomScope() {
        g_fail_alloc_after = -1;
        g_fail_all_allocs = false;
    }
    OomScope(const OomScope&) = delete;
    OomScope& operator=(const OomScope&) = delete;
};

}  // namespace

void* operator new(std::size_t size) {
    if (g_fail_all_allocs) throw std::bad_alloc();
    if (g_fail_alloc_after == 0) {
        g_fail_alloc_after = -1;
        throw std::bad_alloc();
    }
    if (g_fail_alloc_after > 0) --g_fail_alloc_after;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {

using leonida::PipelineWorkers;
using namespace std::chrono_literals;

std::atomic<int> failures = 0;
void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("FAIL: %s\n", what);
    ++failures;
}

// Waits until `ready`, at most 10 s: a job that never comes fails the test instead of hanging it.
template<class Ready>
bool wait_for(Ready ready) {
    const auto end = std::chrono::steady_clock::now() + 10s;
    while (!ready()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// Time spent in a job. Not a sleep: those end on the system's timer tick, up to 16 ms later.
void spend(std::chrono::microseconds time) {
    const auto end = std::chrono::steady_clock::now() + time;
    while (std::chrono::steady_clock::now() < end) std::this_thread::yield();
}

// No workers, or one job: on the calling thread, in order, and no thread is started.
void inline_runs() {
    for (const unsigned threads : {0u, 4u}) {
        PipelineWorkers pool(threads);
        std::vector<std::size_t> order;
        std::set<std::thread::id> ids;
        const auto job = [&](std::size_t i) {
            order.push_back(i);
            ids.insert(std::this_thread::get_id());
        };
        pool.run(0, job);
        check(order.empty(), "run(0) called the job");
        pool.run(1, job);
        if (!threads) pool.run(5, job);
        check(order == (threads ? std::vector<std::size_t>{0} : std::vector<std::size_t>{0, 0, 1, 2, 3, 4}),
              "inline jobs out of order, missing or repeated");
        check(ids == std::set{std::this_thread::get_id()}, "an inline job ran on another thread");
        check(pool.stats().batches == 0 && pool.stats().started == 0, "an inline run was a batch or started a worker");
        check(pool.threads() == threads, "threads()");
    }
    PipelineWorkers pool(0);
    int ran = 0;
    try {
        pool.run(3, [&](std::size_t i) {
            ++ran;
            if (i == 1) throw std::runtime_error("inline");
        });
        check(false, "an inline exception was lost");
    } catch (const std::runtime_error&) {
        check(ran == 2, "inline jobs went on after an exception");
    }
}

// Each job once, all of them done when run() returns. Run often: a lost job or an early return shows up as a race.
void each_once() {
    PipelineWorkers pool(4);
    for (int round = 0; round < 500; ++round) {
        const std::size_t count = 2 + std::size_t(round % 37);
        std::vector<std::atomic<int>> calls(count);
        pool.run(count, [&](std::size_t i) {
            if (round % 8 == 0) spend(50us);
            calls[i].fetch_add(1);
        });
        for (auto& c : calls) check(c == 1, "a job ran zero or several times, or after run() returned");
    }
    const auto stats = pool.stats();
    check(stats.batches == 500 && stats.threads == 4 && stats.started == 4 && stats.running == 0, "stats after the runs");
    PipelineWorkers small(4);
    small.run(3, [](std::size_t) {});
    check(small.stats().started == 2, "a batch of three starts two workers beside its caller");
}

// The last job is still running when all the others are done: run() has to wait for it.
void waits_for_the_last() {
    PipelineWorkers pool(3);
    for (int round = 0; round < 20; ++round) {
        std::atomic<int> finished = 0;
        std::atomic<bool> worker_has_one = false;
        const auto caller = std::this_thread::get_id();
        pool.run(8, [&](std::size_t) {
            if (std::this_thread::get_id() != caller) {
                worker_has_one = true;
                std::this_thread::sleep_for(20ms);
            } else {
                wait_for([&] { return worker_has_one.load(); });
            }
            ++finished;
        });
        check(finished == 8, "run() returned while a worker was in a job");
    }
}

// The jobs do run at the same time, on the workers and the caller together, and never on more threads than that.
void in_parallel() {
    PipelineWorkers pool(3);
    std::atomic<int> inside = 0, most = 0;
    std::mutex mutex;
    std::set<std::thread::id> ids;
    pool.run(4, [&](std::size_t) {
        most = std::max(most.load(), ++inside);
        check(wait_for([&] { return inside.load() == 4; }), "four jobs, three workers and the caller did not meet");
        most = std::max(most.load(), inside.load());
        std::lock_guard lock(mutex);
        ids.insert(std::this_thread::get_id());
    });
    check(ids.size() == 4 && ids.count(std::this_thread::get_id()) == 1, "the caller and each worker took one job");
    ids.clear();
    inside = most = 0;
    pool.run(64, [&](std::size_t) {
        const int now = ++inside;
        int seen = most;
        while (now > seen && !most.compare_exchange_weak(seen, now)) {}
        spend(1ms);
        --inside;
    });
    check(most >= 2 && most <= 4, "more jobs at once than workers and caller, or no two at once");
}

void exceptions() {
    PipelineWorkers pool(4);
    std::atomic<int> ran = 0;
    for (int round = 0; round < 50; ++round) {
        ran = 0;
        std::string what;
        try {
            pool.run(16, [&](std::size_t i) {
                ++ran;
                if (i == 3 || i == 11) {
                    // the later one first: the lowest is still the one rethrown
                    if (i == 3) spend(2ms);
                    throw std::runtime_error(std::to_string(i));
                }
            });
        } catch (const std::runtime_error& error) {
            what = error.what();
        }
        check(what == "3", "run() rethrew no exception, or not the lowest job's");
        check(ran == 16, "an exception kept other jobs from running");
    }
    int after = 0;
    pool.run(1, [&](std::size_t) { ++after; });
    std::atomic<int> more = 0;
    pool.run(8, [&](std::size_t) { ++more; });
    check(after == 1 && more == 8, "the pool does not work after an exception");
}

// Several threads submit at once (several queues): each gets its own batch done, and no batch waits for another to end.
void several_callers() {
    PipelineWorkers pool(2);
    std::atomic<int> wrong = 0;
    std::vector<std::thread> callers;
    for (int c = 0; c < 6; ++c)
        callers.emplace_back([&] {
            for (int round = 0; round < 100; ++round) {
                std::vector<std::atomic<int>> calls(5);
                pool.run(calls.size(), [&](std::size_t i) { ++calls[i]; });
                for (auto& n : calls) wrong += n != 1;
            }
        });
    for (auto& c : callers) c.join();
    check(wrong == 0, "a concurrent run() lost or repeated a job");
    check(pool.stats().running == 0, "jobs still counted as running");

    // One batch whose jobs hold every worker; another caller still gets its batch done.
    std::atomic<bool> release = false;
    std::atomic<int> held = 0;
    std::thread blocked([&] {
        pool.run(3, [&](std::size_t) {
            ++held;
            wait_for([&] { return release.load(); });
        });
    });
    check(wait_for([&] { return held == 3; }), "the first batch did not take both workers and its caller");
    std::atomic<int> done = 0;
    pool.run(4, [&](std::size_t) { ++done; });
    check(done == 4, "a batch did not complete while the workers were busy");
    release = true;
    blocked.join();
}

// A job that itself submits a batch (a pipeline that needs another): the caller's own work keeps it from waiting forever.
void nested() {
    PipelineWorkers pool(2);
    std::atomic<int> inner = 0;
    pool.run(6, [&](std::size_t) { pool.run(6, [&](std::size_t) { ++inner; }); });
    check(inner == 36, "nested runs");
}

// stop() during a run(): the jobs nobody took yet are the caller's, none is lost, and stop() waits for the workers.
void stopped_while_queued() {
    for (int round = 0; round < 30; ++round) {
        PipelineWorkers pool(3);
        std::vector<std::atomic<int>> calls(40);
        std::atomic<bool> started = false, stopped = false;
        std::atomic<int> in_job = 0;
        std::thread runner([&] {
            const auto self = std::this_thread::get_id();
            pool.run(calls.size(), [&](std::size_t i) {
                started = true;
                if (std::this_thread::get_id() != self) ++in_job;
                spend(500us);
                ++calls[i];
                if (std::this_thread::get_id() != self) --in_job;
            });
        });
        wait_for([&] { return started.load(); });
        pool.stop();
        stopped = true;
        check(in_job == 0, "stop() returned while a worker was in a job");
        runner.join();
        for (auto& c : calls) check(c == 1, "a queued job was lost or repeated by stop()");
        // And later batches still complete, on this thread alone, with no worker started for them.
        const std::size_t workers = pool.stats().started;
        std::set<std::thread::id> ids;
        int count = 0;
        pool.run(5, [&](std::size_t) {
            ++count;
            ids.insert(std::this_thread::get_id());
        });
        check(count == 5 && ids == std::set{std::this_thread::get_id()} && pool.stats().started == workers,
              "a run() after stop()");
        pool.stop();  // twice is fine
    }
}

// The destructor stops too, with the workers idle or never started.
void destroyed() {
    for (int i = 0; i < 50; ++i) {
        PipelineWorkers pool(4);
        std::atomic<int> n = 0;
        if (i & 1) pool.run(6, [&](std::size_t) { ++n; });
        check(n == (i & 1 ? 6 : 0), "jobs before the destructor");
    }
}

// OOM while growing the worker pool (start()) or pushing onto queue_ must never leave a dangling stack Batch*
// in queue_ (stack use-after-free on the next worker wakeup) and must still complete every job.
void oom_during_start_and_enqueue() {
    // 1. Existing worker already running; next larger batch hits bad_alloc inside start() while growing threads_.
    for (int fail_at = 0; fail_at < 6; ++fail_at) {
        PipelineWorkers pool(4);
        std::vector<std::atomic<int>> warmup(2);
        pool.run(warmup.size(), [&](std::size_t i) { warmup[i].fetch_add(1); });
        check(pool.stats().started == 1, "oom warmup started one worker");

        std::vector<std::atomic<int>> calls(8);
        const std::function<void(std::size_t)> job = [&calls](std::size_t i) { calls[i].fetch_add(1); };
        bool threw = false;
        try {
            OomScope oom(fail_at);
            pool.run(calls.size(), job);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        check(!threw, "run() threw bad_alloc instead of completing on existing workers/caller");
        for (auto& c : calls) check(c == 1, "oom batch lost or repeated a job");

        // Follow-up batch wakes existing workers and scales to the full pool; a dangling Batch* in queue_ would UAF.
        std::vector<std::atomic<int>> after(5);
        pool.run(after.size(), [&](std::size_t i) { after[i].fetch_add(1); });
        for (auto& c : after) check(c == 1, "post-oom batch lost or repeated a job");
        check(pool.stats().started == 4 && pool.stats().running == 0, "pool recovered full worker count after oom");
    }

    // 2. Hard OOM on the very first batch (both start() and queue_.push_back() fail): falls back inline cleanly.
    {
        PipelineWorkers pool(4);
        std::vector<std::atomic<int>> calls(6);
        const std::function<void(std::size_t)> job = [&calls](std::size_t i) { calls[i].fetch_add(1); };
        bool threw = false;
        try {
            OomScope oom(OomScope::FailAll{});
            pool.run(calls.size(), job);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        check(!threw, "hard oom on first batch threw instead of running inline");
        for (auto& c : calls) check(c == 1, "hard oom inline fallback lost or repeated a job");
        const auto during = pool.stats();
        check(during.started == 0 && during.batches == 0 && during.jobs == 0 && during.running == 0,
              "hard oom inline fallback kept stats clean");

        std::vector<std::atomic<int>> after(5);
        pool.run(after.size(), [&](std::size_t i) { after[i].fetch_add(1); });
        for (auto& c : after) check(c == 1, "batch after hard oom lost or repeated a job");
        check(pool.stats().started == 4 && pool.stats().batches == 1 && pool.stats().running == 0,
              "pool works normally once memory is available again");
    }
}

void setting() {
    using leonida::pipeline_threads;
    check(pipeline_threads(nullptr, 16).count == 8 && pipeline_threads(nullptr, 16).valid, "unset on 16 processors");
    check(pipeline_threads("", 32).count == 8, "empty on 32 processors");
    check(pipeline_threads(nullptr, 6).count == 3 && pipeline_threads(nullptr, 1).count == 0 &&
              pipeline_threads(nullptr, 0).count == 0,
          "unset on few processors");
    check(pipeline_threads("0", 16).count == 0 && pipeline_threads("0", 16).valid, "0 turns it off");
    check(pipeline_threads("3", 16).count == 3, "a number");
    check(pipeline_threads("64", 16).count == 64 && pipeline_threads("65", 16).count == 64 &&
              pipeline_threads("99999999999999999999", 16).count == 64,
          "at most kMaxThreads, without wrapping");
    for (const char* text : {"-1", "two", "4x", " 4", "0x4"})
        check(!pipeline_threads(text, 16).valid && pipeline_threads(text, 16).count == 8, "not a number counts as unset");
}

}  // namespace

int main() {
    inline_runs();
    each_once();
    waits_for_the_last();
    in_parallel();
    exceptions();
    several_callers();
    nested();
    stopped_while_queued();
    destroyed();
    oom_during_start_and_enqueue();
    setting();
    if (failures) return 1;
    std::printf("pipeline_workers_test: passed\n");
    return 0;
}
