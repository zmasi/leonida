// The compile workers of PipelineCompiler (shims/d3d12_pipeline_compile.h, CompilePool), without a device: the jobs are
// functions that wait at gates the test opens, so each rule is seen while the jobs it is about are still running.
//   - the pool size and heavy count from LEONIDA_COMPILE_THREADS, LEONIDA_COMPILE_HEAVY_MAX and LEONIDA_SERIAL_BIG_COMPILES;
//   - a long large build does not hold up a small build a submission waits for (with one worker it still does);
//   - large builds keep within the heavy count, and large and speculative ones together leave one worker free;
//   - demanded jobs go first, in the order they came, then warm-ups by size, and a checkpoint only when nothing runs;
//   - a checkpoint's second look sees a demanded build that started on another worker meanwhile;
//   - one job of a PSO at a time, also with one worker plus the graphics-only one;
//   - the last release with jobs running and queued: the queued ones are dropped, the running ones finish, the
//     workers are joined; a device's cancel waits for its running jobs; stop_for_exit does not wait past its limit.
#include "../shims/d3d12_pipeline_compile.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
#define EXPECT(cond)                                                                       \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

using rt::d3d12::CompilePool;
using rt::d3d12::PipelineCompiler;
using namespace std::chrono_literals;

// A gate a job waits at until the test opens it.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    void wait() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return open; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
};

// What the jobs did: the names in the order they started and finished, and how many ran at once.
struct Log {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> started;
    std::vector<std::string> finished;
    int running = 0, large_running = 0, most = 0, most_large = 0;

    void start(const std::string& name, bool large) {
        std::lock_guard lock(mutex);
        started.push_back(name);
        ++running;
        large_running += large;
        most = (std::max)(most, running);
        most_large = (std::max)(most_large, large_running);
        changed.notify_all();
    }
    void finish(const std::string& name, bool large) {
        std::lock_guard lock(mutex);
        finished.push_back(name);
        --running;
        large_running -= large;
        changed.notify_all();
    }
    bool has_started(const std::string& name) {
        std::lock_guard lock(mutex);
        return std::find(started.begin(), started.end(), name) != started.end();
    }
    // Waits up to `limit` for `count` jobs to have started (finished); false when they have not.
    bool wait_started(std::size_t count, std::chrono::milliseconds limit = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, [&] { return started.size() >= count; });
    }
    bool wait_finished(std::size_t count, std::chrono::milliseconds limit = 10s) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, [&] { return finished.size() >= count; });
    }
};

// Owners only need distinct addresses.
struct Owners {
    char slots[64]{};
    const void* operator[](std::size_t i) const { return &slots[i]; }
};
const char device_slot = 0;
const void* const device = &device_slot;

// A pool of `threads` workers and `heavy` large builds at once, never destroyed: a worker detached by stop_for_exit may
// still be returning from its job.
PipelineCompiler& make_pool(unsigned threads, unsigned heavy) {
    auto* pool = new PipelineCompiler;
    CompilePool config;
    config.threads = threads;
    config.heavy = heavy;
    EXPECT(pool->configure(config));
    EXPECT(!pool->configure(config));  // once
    pool->retain();
    return *pool;
}

void submit(PipelineCompiler& pool, Log& log, Gate* gate, const void* owner, const std::string& name,
            std::size_t priority = 0, bool large = false) {
    EXPECT(pool.submit(owner, device, priority, [&log, gate, name, large] {
        log.start(name, large);
        if (gate) gate->wait();
        log.finish(name, large);
    }, false, large));
}

void test_pool_settings() {
    auto pool = rt::d3d12::compile_pool(nullptr, nullptr, 16, false);
    EXPECT(pool.threads == 8 && pool.heavy == 7);
    pool = rt::d3d12::compile_pool(nullptr, nullptr, 32, false);
    EXPECT(pool.threads == 8);
    pool = rt::d3d12::compile_pool(nullptr, nullptr, 6, false);
    EXPECT(pool.threads == 3 && pool.heavy == 2);
    pool = rt::d3d12::compile_pool(nullptr, nullptr, 2, false);
    EXPECT(pool.threads == 2 && pool.heavy == 1);
    pool = rt::d3d12::compile_pool(nullptr, nullptr, 0, false);  // hardware_concurrency may answer 0
    EXPECT(pool.threads == 2);
    pool = rt::d3d12::compile_pool(L"1", nullptr, 16, false);
    EXPECT(pool.threads == 1 && pool.heavy == 1);
    pool = rt::d3d12::compile_pool(L"0", nullptr, 16, false);
    EXPECT(pool.threads == 1);
    pool = rt::d3d12::compile_pool(L"200", nullptr, 16, false);
    EXPECT(pool.threads == 64);
    pool = rt::d3d12::compile_pool(L"6", L"2", 16, false);
    EXPECT(pool.threads == 6 && pool.heavy == 2);
    pool = rt::d3d12::compile_pool(L"6", L"9", 16, false);
    EXPECT(pool.heavy == 5);  // one worker stays free
    pool = rt::d3d12::compile_pool(L"6", L"0", 16, false);
    EXPECT(pool.heavy == 1);
    pool = rt::d3d12::compile_pool(L"6", nullptr, 16, true);  // LEONIDA_SERIAL_BIG_COMPILES=1
    EXPECT(pool.heavy == 1);
    pool = rt::d3d12::compile_pool(L"6", L"3", 16, true);  // an explicit count wins
    EXPECT(pool.heavy == 3);
    pool = rt::d3d12::compile_pool(L"six", L"x", 16, false);
    EXPECT(!pool.threads_valid && !pool.heavy_valid && pool.threads == 8 && pool.heavy == 7);
}

void test_long_build_does_not_hold_up_a_small_demanded_one() {
    auto& pool = make_pool(2, 1);
    Log log;
    Gate long_build;
    Owners owner;
    submit(pool, log, &long_build, owner[0], "large", 0, true);
    EXPECT(log.wait_started(1));
    submit(pool, log, nullptr, owner[1], "small");
    EXPECT(log.wait_finished(1));  // while the large one still runs
    EXPECT(log.has_started("small"));
    // A second large one waits for the first (heavy count 1); a small one after it does not.
    submit(pool, log, nullptr, owner[2], "large 2", 0, true);
    submit(pool, log, nullptr, owner[3], "small 2");
    EXPECT(log.wait_finished(2));
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("large 2"));
    long_build.release();
    EXPECT(log.wait_finished(4));
    pool.wait_idle();
    EXPECT(log.most_large == 1);
    EXPECT(pool.stats().threads == 2 && pool.stats().most_running == 2);
    pool.release();
}

void test_one_worker_keeps_the_old_order() {
    auto& pool = make_pool(1, 1);  // LEONIDA_COMPILE_THREADS=1
    Log log;
    Gate long_build;
    Owners owner;
    submit(pool, log, &long_build, owner[0], "large", 0, true);
    EXPECT(log.wait_started(1));
    submit(pool, log, nullptr, owner[1], "small");
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("small"));  // behind the large one, as before the pool
    long_build.release();
    EXPECT(log.wait_finished(2));
    EXPECT(pool.stats().threads == 1 && pool.stats().most_running == 1);
    pool.release();
}

// One general worker plus the graphics-only one (LEONIDA_COMPILE_THREADS=1, the layout of main): two variants of one
// graphics PSO must not build at once, one on each worker. A PSO's last release attaches its deletion to the job of
// that PSO that is running (after_running); a second one still in build() on the other worker would use the deleted
// PSO (qhukz13, #2233). Another PSO's graphics build still runs beside it on the graphics worker.
void test_one_worker_and_the_graphics_worker_build_one_job_of_a_pso() {
    auto& pool = make_pool(1, 1);
    Log log;
    Gate first;
    Owners owner;
    const auto graphics = [&](const void* who, const std::string& name, Gate* gate) {
        EXPECT(pool.submit(who, device, 0, [&log, gate, name] {
            log.start(name, false);
            if (gate) gate->wait();
            log.finish(name, false);
        }, true));
    };
    graphics(owner[0], "variant 1", &first);
    EXPECT(log.wait_started(1));
    graphics(owner[0], "variant 2", nullptr);
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("variant 2"));  // the other worker is free, but this PSO has a build running
    graphics(owner[1], "other pso", nullptr);
    EXPECT(log.wait_finished(1));            // the free worker takes another PSO's build meanwhile
    EXPECT(log.has_started("other pso") && !log.has_started("variant 2"));
    // The PSO's last release while "variant 1" runs: "variant 2" is dropped, and the deletion runs after "variant 1".
    std::atomic<bool> deleted{false}, variant_running_at_delete{false};
    EXPECT(pool.after_running(owner[0], [&] {
        std::lock_guard lock(log.mutex);
        variant_running_at_delete = log.running != 0;
        deleted = true;
    }));
    first.release();
    EXPECT(log.wait_finished(2));
    pool.wait_idle();
    EXPECT(deleted.load() && !variant_running_at_delete.load());
    EXPECT(!log.has_started("variant 2"));
    pool.release();
}

void test_heavy_count_and_the_free_worker() {
    auto& pool = make_pool(4, 2);
    Log log;
    Gate hold;
    Owners owner;
    for (int i = 0; i < 4; ++i) submit(pool, log, &hold, owner[i], "large " + std::to_string(i), 0, true);
    EXPECT(log.wait_started(2));
    std::this_thread::sleep_for(100ms);
    EXPECT(log.most_large == 2);  // the heavy count
    // No warm-up while demanded builds are running or held back.
    submit(pool, log, &hold, owner[4], "warm a", 1000);
    submit(pool, log, &hold, owner[5], "warm b", 2000);
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard lock(log.mutex);
        EXPECT(log.started.size() == 2);
        EXPECT(log.running == 2);
    }
    // The last worker is free for a small demanded build.
    Gate small;
    submit(pool, log, &small, owner[6], "small");
    EXPECT(log.wait_started(3));
    EXPECT(log.has_started("small"));
    small.release();
    hold.release();
    EXPECT(log.wait_finished(7));
    pool.wait_idle();
    EXPECT(log.most_large <= 2);
    EXPECT(log.most <= 4);
    pool.release();
}

// Warm-ups one at a time, as on the single worker (each holds the compile gate for its whole create), while the other
// workers take demanded builds.
void test_one_warm_up_at_a_time() {
    auto& pool = make_pool(4, 3);
    Log log;
    Gate hold;
    Owners owner;
    submit(pool, log, &hold, owner[0], "warm a", 100);
    submit(pool, log, &hold, owner[1], "warm b", 200);
    EXPECT(log.wait_started(1));
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("warm b"));  // four workers, nothing else to do, still one warm-up
    submit(pool, log, &hold, owner[2], "demand");
    EXPECT(log.wait_started(2));
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("warm b"));
    hold.release();
    EXPECT(log.wait_finished(3));
    pool.wait_idle();
    std::lock_guard lock(log.mutex);
    EXPECT(log.started.front() == "warm a");
    pool.release();
}

// A warm-up already running does not keep a demanded build waiting, and the next warm-up waits for it.
void test_demanded_builds_go_before_warm_ups() {
    auto& pool = make_pool(3, 2);
    Log log;
    Gate warm, demand;
    Owners owner;
    submit(pool, log, &warm, owner[0], "warm 1", 100);
    EXPECT(log.wait_started(1));
    submit(pool, log, nullptr, owner[1], "warm 2", 50);  // smaller, but not before the first warm-up ends
    submit(pool, log, &demand, owner[2], "demand", 0, true);
    EXPECT(log.wait_started(2));
    EXPECT(log.has_started("demand"));
    warm.release();
    EXPECT(log.wait_finished(1));
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("warm 2"));  // the demanded build still runs
    demand.release();
    EXPECT(log.wait_finished(3));
    pool.release();
}

// A running warm-up takes a heavy place too: with two workers a large demanded build then waits, and the other worker
// stays free for a small one.
void test_a_warm_up_and_a_large_build_leave_a_worker_free() {
    auto& pool = make_pool(2, 1);
    Log log;
    Gate warm, small;
    Owners owner;
    submit(pool, log, &warm, owner[0], "warm", 100);
    EXPECT(log.wait_started(1));
    submit(pool, log, nullptr, owner[1], "large", 0, true);
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("large"));
    submit(pool, log, &small, owner[2], "small");
    EXPECT(log.wait_started(2));
    EXPECT(log.has_started("small"));
    small.release();
    warm.release();
    EXPECT(log.wait_finished(3));
    pool.release();
}

void test_order() {
    auto& pool = make_pool(2, 1);
    Log log;
    Gate first, second;
    Owners owner;
    submit(pool, log, &first, owner[0], "busy 1");
    submit(pool, log, &second, owner[1], "busy 2");
    EXPECT(log.wait_started(2));
    // Queued while both workers are busy.
    submit(pool, log, nullptr, device, "checkpoint", SIZE_MAX);
    submit(pool, log, nullptr, owner[2], "warm 200", 200);
    submit(pool, log, nullptr, owner[3], "warm 100", 100);
    submit(pool, log, nullptr, owner[4], "demand x");
    submit(pool, log, nullptr, owner[5], "demand y");
    EXPECT(pool.has_compile_waiter());
    second.release();  // one worker takes the demanded ones; "busy 1" keeps the other
    EXPECT(log.wait_finished(3));
    std::this_thread::sleep_for(100ms);
    // No warm-up while a demanded build ("busy 1") runs: the compile gate would hold submissions behind it.
    EXPECT(!log.has_started("warm 100") && !log.has_started("warm 200"));
    EXPECT(!log.has_started("checkpoint"));
    EXPECT(pool.has_compile_waiter());  // the warm-ups
    first.release();
    EXPECT(log.wait_finished(7));
    std::lock_guard lock(log.mutex);
    const std::vector<std::string> want{"busy 1", "busy 2", "demand x", "demand y", "warm 100", "warm 200",
                                        "checkpoint"};
    EXPECT(log.started == want);
    pool.release();
}

// The checkpoint starts only when nothing runs, but a demanded build can start on another worker while the checkpoint
// asks the driver for the cache size. Its second look (save_session_pipeline_cache) must see that build running, or it
// goes on to read the cache blob while the build holds the driver.
void test_a_checkpoint_sees_a_demand_that_started_during_it() {
    auto& pool = make_pool(2, 1);
    Log log;
    Gate query;
    Owners owner;
    std::atomic<int> second_look{-1};
    EXPECT(pool.submit(device, device, SIZE_MAX, [&] {
        log.start("checkpoint", false);
        EXPECT(!pool.has_compile_waiter());  // the first look: nothing else queued or running
        query.wait();                         // the size query
        second_look = pool.has_compile_waiter();
        log.finish("checkpoint", false);
    }));
    EXPECT(log.wait_started(1));
    Gate build;
    submit(pool, log, &build, owner[0], "demand");
    EXPECT(log.wait_started(2));  // on the second worker, while the checkpoint runs
    query.release();
    EXPECT(log.wait_finished(1));
    EXPECT(second_look == 1);
    build.release();
    pool.wait_idle();  // the job leaves the running list after its function returns
    EXPECT(!pool.has_compile_waiter());
    pool.release();
}

void test_one_job_of_a_pso_at_a_time() {
    auto& pool = make_pool(4, 3);
    Log log;
    Gate hold;
    Owners owner;
    submit(pool, log, &hold, owner[0], "variant 0");
    EXPECT(log.wait_started(1));
    submit(pool, log, nullptr, owner[0], "variant 1");
    submit(pool, log, nullptr, owner[1], "other");
    EXPECT(log.wait_finished(1));  // "other" passes the second job of owner 0
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("variant 1"));
    hold.release();
    EXPECT(log.wait_finished(3));
    pool.release();
}

// One finished job lets two held-back ones go: the worker that ran it takes one, an idle worker the other.
void test_a_finished_job_wakes_the_idle_workers() {
    auto& pool = make_pool(3, 1);
    Log log;
    Gate first, second;
    Owners owner;
    submit(pool, log, &first, owner[0], "large a", 0, true);
    EXPECT(log.wait_started(1));
    submit(pool, log, &second, owner[1], "large b", 0, true);  // held: heavy count 1
    submit(pool, log, &second, owner[0], "a again");           // held: owner 0 runs
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard lock(log.mutex);
        EXPECT(log.started.size() == 1);
    }
    first.release();
    EXPECT(log.wait_started(3, 3s));  // both, while each still waits at `second`
    second.release();
    EXPECT(log.wait_finished(3));
    pool.release();
}

void test_release_with_jobs_running_and_queued() {
    auto& pool = make_pool(3, 2);
    Log log;
    Gate hold;
    Owners owner;
    submit(pool, log, &hold, owner[0], "running 1");
    submit(pool, log, &hold, owner[1], "running 2", 0, true);
    EXPECT(log.wait_started(2));
    for (int i = 0; i < 5; ++i) submit(pool, log, &hold, owner[2], "queued " + std::to_string(i));  // one PSO
    EXPECT(log.wait_started(3));  // the first of them takes the third worker; the rest wait for it
    std::atomic<bool> released{false};
    std::thread releaser([&] {
        pool.release();  // the last user: drops the queue, waits for the running jobs and joins the workers
        released = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT(!released);
    hold.release();
    releaser.join();
    EXPECT(released);
    EXPECT(!pool.worker_running());
    const auto stats = pool.stats();
    EXPECT(stats.completed == 3);
    EXPECT(stats.dropped == 4);
    // Submitting again starts the workers again.
    pool.retain();
    Log again;
    submit(pool, again, nullptr, owner[3], "after");
    EXPECT(again.wait_finished(1));
    pool.release();
}

void test_cancel_waits_for_a_devices_running_jobs() {
    auto& pool = make_pool(3, 2);
    Log log;
    Gate hold;
    Owners owner;
    submit(pool, log, &hold, owner[0], "a");
    submit(pool, log, &hold, owner[1], "b");
    EXPECT(log.wait_started(2));
    submit(pool, log, &hold, owner[1], "b again");  // queued behind "b" (same PSO)
    std::atomic<bool> cancelled{false};
    std::thread canceller([&] {
        pool.cancel(device);
        cancelled = true;
    });
    std::this_thread::sleep_for(100ms);
    EXPECT(!cancelled);
    hold.release();
    canceller.join();
    {
        std::lock_guard lock(log.mutex);
        EXPECT(log.finished.size() == 2);
        EXPECT(log.running == 0);
    }
    EXPECT(!log.has_started("b again"));
    EXPECT(pool.stats().dropped == 1);
    pool.release();
}

void test_stop_for_exit_does_not_wait_past_its_limit() {
    auto& pool = make_pool(2, 1);
    Log log;
    auto hold = std::make_shared<Gate>();
    Owners owner;
    EXPECT(pool.submit(owner[0], device, 0, [&log, hold] {
        log.start("stuck", false);
        hold->wait();
        log.finish("stuck", false);
    }));
    EXPECT(log.wait_started(1));
    submit(pool, log, nullptr, owner[0], "queued");  // same PSO: waits behind "stuck"
    const auto start = std::chrono::steady_clock::now();
    EXPECT(!pool.stop_for_exit(100ms));
    EXPECT(std::chrono::steady_clock::now() - start < 5s);
    EXPECT(pool.closed());
    EXPECT(!pool.submit(owner[2], device, 0, [] {}));
    EXPECT(pool.stats().refused == 1);
    hold->release();  // the detached worker returns
    EXPECT(log.wait_finished(1));
    std::this_thread::sleep_for(50ms);
    EXPECT(!log.has_started("queued"));
}
}  // namespace

int main() {
    test_pool_settings();
    test_long_build_does_not_hold_up_a_small_demanded_one();
    test_one_worker_keeps_the_old_order();
    test_one_worker_and_the_graphics_worker_build_one_job_of_a_pso();
    test_heavy_count_and_the_free_worker();
    test_one_warm_up_at_a_time();
    test_demanded_builds_go_before_warm_ups();
    test_a_warm_up_and_a_large_build_leave_a_worker_free();
    test_order();
    test_a_checkpoint_sees_a_demand_that_started_during_it();
    test_one_job_of_a_pso_at_a_time();
    test_a_finished_job_wakes_the_idle_workers();
    test_release_with_jobs_running_and_queued();
    test_cancel_waits_for_a_devices_running_jobs();
    test_stop_for_exit_does_not_wait_past_its_limit();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("PASS d3d12_compile_pool\n");
    return 0;
}
