// Timing and scheduling for Vulkan pipeline creation on the D3D12 device.
#pragma once

#include "../src/async_pipeline_size.h"
#include "../src/vulkan_device_fault.h"
#include "../src/vulkan_replay.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <string>
#include <cstring>
#include <vector>

namespace rt::d3d12 {
inline bool trace_pipeline_compile() {
    static const bool on = [] {
        wchar_t v[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_TRACE_PIPELINE_COMPILE", v, 2) == 1 && v[0] == L'1';
    }();
    return on;
}

inline bool async_replay_compile() {
    static const bool on = [] {
        wchar_t v[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_ASYNC_PIPELINES", v, 2) == 1 && v[0] == L'1';
    }();
    return on && !leonida::strict_replay();
}

// LEONIDA_ASYNC_MIN_BYTES=<n> (with LEONIDA_ASYNC_PIPELINES=1): only pipelines with n bytes of SPIR-V or more are built
// asynchronously; a smaller one is built where it is needed, as with asynchronous builds off. Unset or 0: every
// pipeline, as before the variable (src/async_pipeline_size.h). LEONIDA_ASYNC_MIN_BYTES_GRAPHICS=<n> gives graphics and
// mesh pipelines their own threshold (0: every one asynchronous); unset, they take LEONIDA_ASYNC_MIN_BYTES's.
inline const leonida::async_pipelines::MinBytes& async_min_bytes() {
    static const leonida::async_pipelines::MinBytes thresholds = [] {
        // Text of a variable, or null when it is unset or empty; `valid` false when it is too long to be a number.
        const auto read = [](const char* name, char (&text)[32], bool& valid) -> const char* {
            const DWORD n = GetEnvironmentVariableA(name, text, sizeof(text));
            valid = n < sizeof(text);
            return valid && n ? text : nullptr;
        };
        char all_text[32]{}, graphics_text[32]{};
        bool all_fits = true, graphics_fits = true, all_valid = true, graphics_valid = true;
        const char* all = read("LEONIDA_ASYNC_MIN_BYTES", all_text, all_fits);
        const char* graphics = read("LEONIDA_ASYNC_MIN_BYTES_GRAPHICS", graphics_text, graphics_fits);
        // Text that is not a number counts as unset: 0 for LEONIDA_ASYNC_MIN_BYTES, compute's for the graphics one.
        const auto value = leonida::async_pipelines::parse_thresholds(all, graphics, &all_valid, &graphics_valid);
        if (!all_fits || !all_valid)
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_ASYNC_MIN_BYTES: not a number of bytes, ignored (every "
                                 "asynchronous pipeline stays asynchronous)\n");
        if (!graphics_fits || !graphics_valid)
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_ASYNC_MIN_BYTES_GRAPHICS: not a number of bytes, ignored "
                                 "(graphics pipelines take LEONIDA_ASYNC_MIN_BYTES)\n");
        if ((value.compute || value.graphics_set) && async_replay_compile())
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_ASYNC_MIN_BYTES: compute pipelines under %llu bytes of SPIR-V and "
                                 "graphics pipelines under %llu (%s) are built where they are needed; larger ones "
                                 "asynchronously (0: every one)\n",
                         (unsigned long long)value.compute, (unsigned long long)value.graphics,
                         value.graphics_set ? "LEONIDA_ASYNC_MIN_BYTES_GRAPHICS"
                                            : "LEONIDA_ASYNC_MIN_BYTES_GRAPHICS unset, the same");
        return value;
    }();
    return thresholds;
}

// Whether a pipeline of `spirv_bytes` (compute: its module; graphics and mesh: all its stages) is built asynchronously.
inline bool async_replay_compile(bool graphics, std::uint64_t spirv_bytes) {
    return async_replay_compile() &&
           leonida::async_pipelines::asynchronous(true, async_min_bytes(), graphics, spirv_bytes);
}

// LOCAL FIX (post-load stalls): LEONIDA_FAST_PIPELINES=graphics, compute, or all (1 = all) creates those pipelines with
// VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT. After the story load, graphics creates measured about 4.5 s each, and
// every submission waits at the compile gate while one is in the driver (frames stopped for 130 s); the title's large
// compute programs take 30-220 s each. Slower shaders, much shorter builds: the trade for reaching the world before
// it looks right. The flag is part of the compute journal key, so optimized and fast builds keep separate files.
inline VkPipelineCreateFlags fast_pipeline_flags(bool compute) {
    static const unsigned mask = [] {
        wchar_t v[16]{};
        const DWORD n = GetEnvironmentVariableW(L"LEONIDA_FAST_PIPELINES", v, 16);
        unsigned m = 0;
        if (n > 0 && n < 16) {
            if (!wcscmp(v, L"1") || !_wcsicmp(v, L"all")) m = 3;
            else if (!_wcsicmp(v, L"graphics")) m = 1;
            else if (!_wcsicmp(v, L"compute")) m = 2;
            else std::fprintf(stderr, "[rt] d3d12: LEONIDA_FAST_PIPELINES: unknown value, ignored (graphics, compute, all)\n");
        }
        if (m)
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_FAST_PIPELINES: %s pipelines are built without driver optimization\n",
                         m == 3 ? "graphics and compute" : m == 1 ? "graphics" : "compute");
        return m;
    }();
    return (mask & (compute ? 2u : 1u)) ? VkPipelineCreateFlags(VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT) : 0;
}

// Absolute monotonic timestamps let trace readers distinguish time in the ICD from time queued for the worker.
using CompileClock = std::chrono::steady_clock;
inline long long compile_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(CompileClock::now().time_since_epoch()).count();
}

inline long long compile_wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

template<class Build> VkPipeline replay_pipeline(const char* trigger, Build&& get) {
    if (!trace_pipeline_compile()) return get();
    const auto start = compile_us();
    auto handle = get();
    auto elapsed = compile_us() - start;
    if (elapsed >= 1000)
        std::fprintf(stderr, "[rt] pipeline-use trigger=%s tid=%lu us=%lld ready=%d end_ms=%lld\n",
                     trigger, GetCurrentThreadId(), elapsed, int(handle != VK_NULL_HANDLE), compile_wall_ms());
    return handle;
}

struct CompileTrace {
    VkPipelineCreationFeedback feedback{};
    VkPipelineCreationFeedbackCreateInfo chain{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               nullptr, &feedback, 0, nullptr};
    long long started = 0;
    const char* key = "-";
    bool linked = false;  // chain is in a create info's pNext chain

    template<class Info> void begin(Info& info, bool supported, const char* trigger, const char* shaders,
                                    std::uint32_t variant, std::size_t bytes, const std::string& description_key) {
        if (!trace_pipeline_compile()) return;
        if (!description_key.empty()) key = description_key.c_str();
        if (supported) request_feedback(info);
        std::fprintf(stderr, "[rt] pipeline-compile-begin trigger=%s tid=%lu bytes=%zu variant=%u begin_ms=%lld key=%s shaders=%s\n",
                     trigger, GetCurrentThreadId(), bytes, variant, compile_wall_ms(), key, shaders);
        started = compile_us();
    }

    // Chains the feedback struct ahead of info.pNext, once. begin() does when tracing; the pipeline journal does on a
    // Vulkan 1.3 device (feedback is core there) to learn whether a pipeline came out of its cache.
    template<class Info> void request_feedback(Info& info) {
        if (linked) return;
        chain.pNext = info.pNext;
        info.pNext = &chain;
        linked = true;
    }

    // 1 when the driver reports the pipeline was found in the application's VkPipelineCache, 0 when it reports it was
    // not, -1 when it reported nothing (no feedback chained, or not VALID).
    int hit() const {
        return !(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) ? -1 :
               int((feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0);
    }

    void end(const char* trigger, const char* shaders, std::uint32_t variant, std::size_t bytes, VkResult result) const {
        if (!started) return;
        const auto ended = compile_us();
        const int hit = this->hit();
        std::fprintf(stderr, "[rt] pipeline-compile trigger=%s tid=%lu start_us=%lld us=%lld hit=%d bytes=%zu "
                             "variant=%u result=%d end_ms=%lld key=%s shaders=%s\n", trigger, GetCurrentThreadId(), started,
                             ended - started, hit, bytes, variant, int(result), compile_wall_ms(), key, shaders);
    }
};

// The triggers of builds on the compile worker (PipelineCompiler): "warmup", "dispatch-worker", "draw-worker".
inline bool compile_worker_trigger(const char* trigger) {
    if (!trigger) return false;
    const std::size_t n = std::strlen(trigger);
    return !std::strcmp(trigger, "warmup") || (n > 7 && !std::strcmp(trigger + n - 7, "-worker"));
}

// LOCAL FIX (early device loss): one driver call that builds or drops shader code, kept out of the way of queue
// submissions (leonida::SubmitMutex). Before the call:
//   - let_submit_finish: a submit in progress finishes first. The compile worker also lets the submitters queued
//     behind it go first, and does so before its vkCreateShaderModule and vkDestroyShaderModule too; a title thread
//     waits only for the holder, and only before a pipeline create (it may wait out a vkDeviceWaitIdle or a present:
//     waits of 50 ms or more are logged).
//   - a pipeline create passes the compile gate (SubmitMutex::CompileTurn) for the length of the call, as its kind
//     (compute, or graphics with mesh): the adaptive gate keeps a verdict per kind.
//   - the compile worker, which runs below normal priority, runs the call at normal priority, so a preempted worker
//     does not sit on a driver lock a submit waits for.
class DriverTurn {
public:
    DriverTurn(leonida::SubmitMutex& submits, const char* trigger, const char* call, bool pipeline,
               const char* name = nullptr,
               leonida::SubmitMutex::CreateKind kind = leonida::SubmitMutex::CreateKind::compute, std::size_t bytes = 0)
        : worker_(compile_worker_trigger(trigger)) {
        // Normal priority first: a below-normal worker that waits its turn at normal priority cannot keep losing the
        // race to the next submit, and holds the gate (below) only at normal priority.
        if (worker_) {
            const HANDLE self = GetCurrentThread();
            const int priority = GetThreadPriority(self);
            if (priority != THREAD_PRIORITY_ERROR_RETURN && priority < THREAD_PRIORITY_NORMAL &&
                SetThreadPriority(self, THREAD_PRIORITY_NORMAL))
                restore_ = priority;
        }
        if (worker_ || pipeline) {
            const std::int64_t waited = submits.let_submit_finish(worker_);
            if (waited >= 50) note("waited for the submit lock", trigger, call, name, waited);
        }
        if (pipeline) {
            gate_.emplace(submits, leonida::SubmitMutex::CreateInfo{kind, name, trigger, bytes});
            if (gate_->waited_ms() >= 50) note("waited at the compile gate", trigger, call, name, gate_->waited_ms());
        }
    }
    ~DriverTurn() {
        // Leave the gate before dropping back to below normal: a worker preempted while still counted in the gate
        // would hold every submitter waiting there (each with the submit lock held) until the scheduler's starvation
        // boost runs it again.
        gate_.reset();
        if (restore_ != THREAD_PRIORITY_ERROR_RETURN) SetThreadPriority(GetCurrentThread(), restore_);
    }
    DriverTurn(const DriverTurn&) = delete;
    DriverTurn& operator=(const DriverTurn&) = delete;

private:
    // Every wait of a second or more, and the first 32 of 50 ms or more.
    static void note(const char* what, const char* trigger, const char* call, const char* name, std::int64_t ms) {
        static std::atomic<unsigned> logged{0};
        if (ms < 1000 && logged.fetch_add(1, std::memory_order_relaxed) >= 32) return;
        std::fprintf(stderr, "[rt] submit-handoff: %s (%s, %s) on thread %lu %s %lld ms at t=%.3f s\n", call,
                     trigger ? trigger : "-", name ? name : "-", GetCurrentThreadId(), what, static_cast<long long>(ms),
                     double(leonida::diagnostic_ms()) / 1000.0);
    }

    bool worker_;
    std::optional<leonida::SubmitMutex::CompileTurn> gate_;
    int restore_ = THREAD_PRIORITY_ERROR_RETURN;
};


// How many compile workers PipelineCompiler runs, and how many of them may build a large module at once.
//   LEONIDA_COMPILE_THREADS=<n>: n workers (1..64). Unset: half the logical processors, 2 at least and 8 at most. 1: one
//     worker for every job plus the graphics-only one (LEONIDA_GRAPHICS_COMPILE_WORKER), the layout before the pool.
//   LEONIDA_COMPILE_HEAVY_MAX=<n>: at most n large builds (compute modules over 128 KiB, graphics pipelines of
//     PipelineCompiles::large_spirv_bytes or more) at once, 1..threads-1. Unset: threads-1, or 1 with
//     LEONIDA_SERIAL_BIG_COMPILES=1. Large builds still take their place at the memory gate (big_compile_gate) too.
// With two workers or more, large and speculative builds (warm-ups) together take at most threads-1 of them, so one
// worker is always free for a small build a submission waits for. Warm-ups run one at a time, as on the single worker,
// and none starts while a demanded build is queued or running: each holds the compile gate for its whole create, and a
// submission waits there for every create in flight (MoonEyes, #1608: with the gate on, submissions waited 142 s in all
// behind warm-ups of about 800 KB, 5-13.5 s each).
struct CompilePool {
    unsigned threads = 1;
    unsigned heavy = 1;          // large builds at once
    bool threads_valid = true;   // false: LEONIDA_COMPILE_THREADS was not a number (the default is used)
    bool heavy_valid = true;
};

inline bool parse_compile_count(const wchar_t* text, unsigned& out) {
    if (!text || !*text) return false;
    unsigned value = 0;
    for (const wchar_t* c = text; *c; ++c) {
        if (*c < L'0' || *c > L'9') return false;
        value = (std::min)(1000u, value * 10 + unsigned(*c - L'0'));
    }
    out = value;
    return true;
}

// Null or empty text: the setting is unset.
inline CompilePool compile_pool(const wchar_t* threads_text, const wchar_t* heavy_text, unsigned logical_processors,
                                bool serial_big) {
    CompilePool pool;
    pool.threads = std::clamp(logical_processors / 2, 2u, 8u);
    unsigned value = 0;
    if (threads_text && *threads_text) {
        if (parse_compile_count(threads_text, value)) pool.threads = std::clamp(value, 1u, 64u);
        else pool.threads_valid = false;
    }
    const unsigned most = pool.threads > 1 ? pool.threads - 1 : 1;
    pool.heavy = serial_big ? 1 : most;
    if (heavy_text && *heavy_text) {
        if (parse_compile_count(heavy_text, value)) pool.heavy = std::clamp(value, 1u, most);
        else pool.heavy_valid = false;
    }
    return pool;
}

inline CompilePool compile_pool_from_environment() {
    wchar_t threads[16]{}, heavy[16]{}, serial[2]{};
    const DWORD t = GetEnvironmentVariableW(L"LEONIDA_COMPILE_THREADS", threads, 16);
    const DWORD h = GetEnvironmentVariableW(L"LEONIDA_COMPILE_HEAVY_MAX", heavy, 16);
    const DWORD s = GetEnvironmentVariableW(L"LEONIDA_SERIAL_BIG_COMPILES", serial, 2);
    if (t >= 16) threads[0] = L'x';  // too long to be a count
    if (h >= 16) heavy[0] = L'x';
    const bool serial_big = s == 1 && serial[0] != L'0';
    const CompilePool pool = compile_pool(t ? threads : nullptr, h ? heavy : nullptr,
                                          std::thread::hardware_concurrency(), serial_big);
    if (!pool.threads_valid) std::fprintf(stderr, "[rt] d3d12: LEONIDA_COMPILE_THREADS is not a number; not used\n");
    if (!pool.heavy_valid) std::fprintf(stderr, "[rt] d3d12: LEONIDA_COMPILE_HEAVY_MAX is not a number; not used\n");
    return pool;
}

// The compile workers: replay's asynchronous builds (LEONIDA_ASYNC_PIPELINES=1), the compute warm-up and the startup
// cache checkpoint. Jobs are taken most urgent first: demanded work has priority zero (in the order it came), speculative
// work is ordered by module size, and a checkpoint (SIZE_MAX) runs last. No Vulkan call runs with the queue lock held.
// With two workers or more (CompilePool), a worker takes the most urgent job that
//   - is not a checkpoint while any worker is busy (it saves the cache only when nothing compiles);
//   - has no other job of its owner running (one build of a PSO at a time);
//   - when it is large or speculative, leaves one worker for the others (threads-1 of these at once); when it is
//     large, keeps within the heavy count; when it is speculative, runs alone among speculative ones and only while
//     no demanded build is queued or running.
// So a long driver compile holds up only the jobs of its own PSO, and a small build a submission asked for gets a free
// worker while large ones run. With one worker the order is the old one: the next job, whatever it is, except that
// with the graphics-only worker beside it one build of a PSO at a time holds there too (a PSO's last Release waits only
// for the build of it that runs: two at once, one on each worker, left the other using the deleted PSO).
//
// Lifetime. A job names its owner (the PSO it builds, or the device for a cache checkpoint) and the device whose
// Vulkan objects it uses, and holds no reference to either: a reference kept the device alive past the title's last
// Release until the worker reached the job, so a released device's builds ran on (a test that then destroyed its
// VkDevice crashed).
//   - A PSO's last Release does not wait for its running build, which can be a dispatch-worker build of a module the
//     driver takes minutes over: after_running() drops its queued jobs and hands the PSO's deletion to the worker,
//     to run when that build returns (PipelineState::last_release). The PSO lets its device reference go at once.
//   - A device's teardown waits: cancel() drops its queued jobs (by owner or device) and waits for its running ones
//     and what runs after them, so an in-flight vkCreate*Pipelines finishes before the device's objects go.
//   - The workers run while a device is alive (retain/release): the last device's release stops and joins them.
//   - stop_for_exit() is for a process that ends with its device alive (main.cpp: the title's exit, a console Ctrl+C
//     or close). After it nothing more is queued: submit() refuses, and the callers build in place or not at all.
class PipelineCompiler {
public:
    static PipelineCompiler& instance() {
        static auto* pool = new PipelineCompiler;  // never destroyed: static destruction must not race the workers
        return *pool;
    }

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;  // jobs the workers ran
        std::uint64_t dropped = 0;    // queued jobs thrown away by cancel, after_running, release or stop_for_exit
        std::uint64_t refused = 0;    // submissions after stop_for_exit
        std::uint64_t waited = 0;     // cancel calls that waited for a running job
        std::uint64_t deferred = 0;   // after_running calls that left something to run after a running job
        unsigned threads = 0;         // the pool's workers (CompilePool), 0 before the first job
        unsigned heavy = 0;           // its large builds at once
        std::size_t most_running = 0; // the most jobs that ran at once
    };

    // Sets the pool size before the first job (tests); otherwise it is read from the environment at the first submit.
    // False when it was set already.
    bool configure(CompilePool pool) {
        std::lock_guard lock(mutex_);
        if (pool_) return false;
        pool_ = pool;
        return true;
    }

    // False, with nothing queued, after stop_for_exit. `graphics`: a graphics pipeline build; with one worker it may also
    // run on a second worker that takes only graphics jobs, so a compute build the driver takes minutes over does not
    // hold up later graphics pipelines (LEONIDA_GRAPHICS_COMPILE_WORKER=0: no second worker). `large`: a build of a
    // large module (CompilePool's heavy count).
    bool submit(const void* owner, const void* device, std::size_t priority, std::function<void()> work,
                bool graphics = false, bool large = false) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            ++stats_.refused;
            return false;
        }
        if (!pool_) {
            pool_ = compile_pool_from_environment();
            if (pool_->threads > 1)
                std::fprintf(stderr, "[rt] d3d12: %u pipeline compile workers, at most %u large builds at once "
                                     "(LEONIDA_COMPILE_THREADS, LEONIDA_COMPILE_HEAVY_MAX)\n",
                             pool_->threads, pool_->heavy);
        }
        stats_.threads = pool_->threads;
        stats_.heavy = pool_->heavy;
        while (workers_.size() < pool_->threads) {
            const std::uint64_t generation = generation_;
            workers_.emplace_back([this, generation] { run(generation, false); });
        }
        if (graphics && pool_->threads == 1 && graphics_worker_enabled() && !graphics_worker_.joinable()) {
            const std::uint64_t generation = generation_;
            graphics_worker_ = std::thread([this, generation] { run(generation, true); });
        }
        jobs_.emplace(priority, Job{owner, device, std::move(work), graphics, large});
        ++stats_.submitted;
        wake_.notify_all();
        return true;
    }

    // Lock-free: replay asks at every draw and dispatch whose pipeline is not built yet.
    bool closed() const { return closed_flag_.load(std::memory_order_acquire); }

    // True while a build other than the checkpoint is queued, or a demanded build runs. The checkpoint asks twice: before
    // and after the size query, during which a demanded build can start on another worker (no warm-up can: one runs
    // only when no other speculative job does, and the checkpoint counts as one).
    bool has_compile_waiter() {
        std::lock_guard lock(mutex_);
        if (!jobs_.empty() && jobs_.begin()->first != SIZE_MAX) return true;
        return std::any_of(running_.begin(), running_.end(),
                           [](const Running& r) { return !r.speculative && !r.finished; });
    }
    // Returns when no job is queued or running. A job owns its pipeline state, and through it the device, until it
    // finishes; a caller that counts references (rt::d3d12_xs::device_proxy_ref_count) waits here first.
    void wait_idle() {
        std::unique_lock wait(mutex_);
        done_.wait(wait, [this] { return jobs_.empty() && running_.empty(); });
    }

    // Drops the queued jobs whose owner or device is `key` and waits for the running ones, with what runs after them
    // (after_running), unless one is on this thread. Jobs a running one queues for `key` meanwhile are dropped too.
    // Returns how many were dropped.
    std::size_t cancel(const void* key) {
        std::vector<Job> dropped;
        {
            std::unique_lock lock(mutex_);
            const auto me = std::this_thread::get_id();
            bool counted = false;
            for (;;) {
                drop_locked(dropped, [key](const Job& job) { return job.owner == key || job.device == key; });
                const bool busy = std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
                    return (r.owner == key || r.device == key) && r.thread != me;
                });
                if (!busy) break;
                if (!counted) ++stats_.waited;
                counted = true;
                done_.wait(lock);
            }
        }
        return dropped.size();  // the dropped jobs' captures are destroyed here, outside the lock
    }

    // Drops the queued jobs filed under `owner`. When one of its jobs is running, `last` is run on the worker after it
    // returns, before the job counts as done (a device's cancel waits for both), and the answer is true; otherwise
    // false, and `last` is not kept. On the worker inside that job itself, `last` runs after the job returns too.
    bool after_running(const void* owner, std::function<void()> last) {
        std::vector<Job> dropped;
        std::lock_guard lock(mutex_);
        drop_locked(dropped, [owner](const Job& job) { return job.owner == owner; });
        for (Running& r : running_) {
            if (r.owner != owner || r.finished) continue;
            r.after = r.after ? [first = std::move(r.after), then = std::move(last)] { first(); then(); }
                              : std::move(last);
            ++stats_.deferred;
            return true;
        }
        return false;
    }

    // True on a worker while it runs a job whose owner or device is `key` (not what runs after it).
    bool running_here(const void* key) {
        std::lock_guard lock(mutex_);
        const auto me = std::this_thread::get_id();
        return std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
            return r.thread == me && !r.finished && (r.owner == key || r.device == key);
        });
    }

    // A device holds the workers while it lives. The last release drops whatever is still queued, stops the workers
    // and joins them; a release on a worker itself lets it return after the job it is in.
    void retain() {
        std::lock_guard lock(mutex_);
        ++users_;
    }
    void release() {
        std::vector<Job> dropped;
        Workers workers;
        {
            std::lock_guard lock(mutex_);
            if (users_ && --users_) return;
            workers = stop_locked(dropped);
        }
        finish(std::move(workers), true);
    }

    // Process exit with a device still alive: drops the queue, refuses new jobs and waits up to `wait` for the running
    // ones, so ExitProcess does not end a worker inside the driver. False when a job was still running.
    bool stop_for_exit(std::chrono::milliseconds wait) {
        std::vector<Job> dropped;
        Workers workers;
        bool idle = true;
        {
            std::unique_lock lock(mutex_);
            closed_ = true;
            closed_flag_.store(true, std::memory_order_release);
            workers = stop_locked(dropped);
            const auto me = std::this_thread::get_id();
            idle = done_.wait_for(lock, wait, [&] {
                return std::none_of(running_.begin(), running_.end(), [&](const Running& r) { return r.thread != me; });
            });
        }
        finish(std::move(workers), idle);
        return idle;
    }

    Stats stats() {
        std::lock_guard lock(mutex_);
        return stats_;
    }
    bool worker_running() {
        std::lock_guard lock(mutex_);
        return graphics_worker_.joinable() ||
               std::any_of(workers_.begin(), workers_.end(), [](const std::thread& t) { return t.joinable(); });
    }
    // Tests: called on the worker with each job's owner, once the job counts as running and before its work.
    void set_job_probe(std::function<void(const void* owner)> probe) {
        std::lock_guard lock(mutex_);
        probe_ = std::move(probe);
    }

private:
    struct Job {
        const void* owner = nullptr;
        const void* device = nullptr;
        std::function<void()> work;
        bool graphics = false;  // a graphics pipeline build: the graphics worker may take it too
        bool large = false;     // a large module's build (CompilePool::heavy)
    };
    static bool graphics_worker_enabled() {
        static const bool on = [] {
            wchar_t v[2]{};
            return !(GetEnvironmentVariableW(L"LEONIDA_GRAPHICS_COMPILE_WORKER", v, 2) == 1 && v[0] == L'0');
        }();
        return on;
    }
    // The pool's workers, and the graphics-only one when it was started (one worker only).
    struct Workers {
        std::vector<std::thread> general;
        std::thread graphics;
    };
    struct Running {
        const void* owner;
        const void* device;
        std::thread::id thread;
        bool heavy = false;           // large or speculative: counts against threads-1
        bool large = false;           // counts against the heavy count
        bool speculative = false;     // a warm-up: one at a time
        std::function<void()> after;  // after_running
        bool finished = false;        // the job returned; `after` may be running
    };

    template<class Match> void drop_locked(std::vector<Job>& dropped, const Match& match) {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (match(it->second)) {
                dropped.push_back(std::move(it->second));
                it = jobs_.erase(it);
                ++stats_.dropped;
            } else {
                ++it;
            }
        }
    }

    Workers stop_locked(std::vector<Job>& dropped) {
        drop_locked(dropped, [](const Job&) { return true; });
        ++generation_;  // the workers return when they next look: after the job each is running, if any
        wake_.notify_all();
        Workers workers{std::move(workers_), std::move(graphics_worker_)};
        workers_.clear();
        return workers;
    }

    static void finish(std::thread worker, bool join) {
        if (!worker.joinable()) return;
        if (join && worker.get_id() != std::this_thread::get_id()) worker.join();
        else worker.detach();
    }
    static void finish(Workers workers, bool join) {
        for (std::thread& worker : workers.general) finish(std::move(worker), join);
        finish(std::move(workers.graphics), join);
    }

    Running& mine(std::thread::id me) {
        return *std::find_if(running_.begin(), running_.end(), [me](const Running& r) { return r.thread == me; });
    }

    // The first job this worker may take, most urgent first (see the class comment). With one worker: any job for the
    // general worker, a graphics job for the graphics one, but not one whose PSO has a build running on the other.
    std::multimap<std::size_t, Job>::iterator next_locked(bool graphics_only) {
        // As on main with #2361 (qhukz13, RT-COMPILE-OWNER-RACE): a job whose PSO has an entry running waits, so
        // after_running never deletes a PSO while another build of it runs on the other worker.
        if (!pool_ || pool_->threads < 2) {
            return std::find_if(jobs_.begin(), jobs_.end(), [this, graphics_only](const auto& entry) {
                if (graphics_only && !entry.second.graphics) return false;
                const void* owner = entry.second.owner;
                // A finished entry may still be running what comes after its job (the PSO's deletion): it counts too.
                return std::none_of(running_.begin(), running_.end(), [owner](const Running& r) { return r.owner == owner; });
            });
        }
        unsigned heavy = 0, large = 0, speculative = 0, demanded = 0;
        for (const Running& r : running_) {
            heavy += r.heavy;
            large += r.large;
            speculative += r.speculative;
            demanded += !r.speculative;
        }
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            const Job& job = it->second;
            if (it->first == SIZE_MAX) {
                if (running_.empty()) return it;
                continue;
            }
            if (std::any_of(running_.begin(), running_.end(), [&](const Running& r) { return r.owner == job.owner; }))
                continue;
            if ((job.large || it->first != 0) && heavy >= pool_->threads - 1) continue;
            if (job.large && large >= pool_->heavy) continue;
            if (it->first != 0 && (speculative || demanded)) continue;
            return it;
        }
        return jobs_.end();
    }

    void run(std::uint64_t generation, bool graphics_only) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto me = std::this_thread::get_id();
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return generation_ != generation || next_locked(graphics_only) != jobs_.end(); });
            if (generation_ != generation) return;
            auto first = next_locked(graphics_only);
            const bool speculative = first->first != 0;
            Job job = std::move(first->second);
            jobs_.erase(first);
            running_.push_back({job.owner, job.device, me, job.large || speculative, job.large, speculative});
            stats_.most_running = (std::max)(stats_.most_running, running_.size());
            const auto probe = probe_;
            lock.unlock();
            try {
                if (probe) probe(job.owner);
                job.work();
            } catch (...) {
                std::fprintf(stderr, "[rt] pipeline compile worker: a job threw\n");
            }
            job.work = nullptr;  // its captures go before the job counts as done
            lock.lock();
            ++stats_.completed;
            Running& self = mine(me);
            self.finished = true;
            if (std::function<void()> after = std::move(self.after)) {
                lock.unlock();
                try {
                    after();
                } catch (...) {
                    std::fprintf(stderr, "[rt] pipeline compile worker: work after a job threw\n");
                }
                after = nullptr;
                lock.lock();
            }
            running_.erase(std::find_if(running_.begin(), running_.end(), [me](const Running& r) {
                return r.thread == me;
            }));
            done_.notify_all();
            wake_.notify_all();  // a job this one held back (its PSO's, a heavy one, a checkpoint) may go now
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::multimap<std::size_t, Job> jobs_;
    std::vector<Running> running_;  // one per busy worker; a stopped worker may still be finishing one
    std::vector<std::thread> workers_;  // CompilePool::threads, started at the first job
    std::thread graphics_worker_;  // one worker only: graphics jobs (submit's `graphics`), started at the first one
    std::optional<CompilePool> pool_;  // read at the first submit unless configure() came first
    std::uint64_t generation_ = 0;  // a worker runs while this is the value it started with
    unsigned users_ = 0;            // live devices (retain/release)
    bool closed_ = false;           // stop_for_exit: nothing more is queued
    std::atomic<bool> closed_flag_{false};  // closed_, for closed()
    Stats stats_;
    std::function<void(const void*)> probe_;
};
}  // namespace rt::d3d12
