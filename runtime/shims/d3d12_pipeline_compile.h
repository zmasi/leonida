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
// pipeline, as before the variable (src/async_pipeline_size.h).
inline std::uint64_t async_min_bytes() {
    static const std::uint64_t bytes = [] {
        char text[32]{};
        const DWORD n = GetEnvironmentVariableA("LEONIDA_ASYNC_MIN_BYTES", text, sizeof(text));
        bool valid = n < sizeof(text);
        const std::uint64_t value = valid ? leonida::async_pipelines::parse_min_bytes(text, &valid) : 0;
        if (!valid)
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_ASYNC_MIN_BYTES: not a number of bytes, ignored (every "
                                 "asynchronous pipeline stays asynchronous)\n");
        else if (value && async_replay_compile())
            std::fprintf(stderr, "[rt] d3d12: LEONIDA_ASYNC_MIN_BYTES: pipelines under %llu bytes of SPIR-V are built "
                                 "where they are needed; larger ones asynchronously\n", (unsigned long long)value);
        return value;
    }();
    return bytes;
}

// Whether a pipeline of `spirv_bytes` (compute: its module; graphics: all its stages) is built asynchronously.
inline bool async_replay_compile(std::uint64_t spirv_bytes) {
    return async_replay_compile() &&
           leonida::async_pipelines::asynchronous(true, async_min_bytes(), spirv_bytes);
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


// Demanded work has priority zero, speculative work is ordered by module size, and
// an idle checkpoint runs last. No Vulkan call runs with the queue lock held.
//
// Lifetime. A job names its owner (the PSO it builds, or the device for a cache checkpoint) and the device whose
// Vulkan objects it uses, and holds no reference to either: a reference kept the device alive past the title's last
// Release until the worker reached the job, so a released device's builds ran on (a test that then destroyed its
// VkDevice crashed).
//   - A PSO's last Release does not wait for its running build, which can be a dispatch-worker build of a module the
//     driver takes minutes over: after_running() drops its queued jobs and hands the PSO's deletion to the worker,
//     to run when that build returns (PipelineState::last_release). The PSO lets its device reference go at once.
//   - A device's teardown waits: cancel() drops its queued jobs (by owner or device) and waits for its running one
//     and what runs after it, so an in-flight vkCreate*Pipelines finishes before the device's objects go.
//   - The worker thread runs while a device is alive (retain/release): the last device's release stops and joins it.
//   - stop_for_exit() is for a process that ends with its device alive (main.cpp: the title's exit, a console Ctrl+C
//     or close). After it nothing more is queued: submit() refuses, and the callers build in place or not at all.
class PipelineCompiler {
public:
    static PipelineCompiler& instance() {
        static auto* pool = new PipelineCompiler;  // never destroyed: static destruction must not race the worker
        return *pool;
    }

    struct Stats {
        std::uint64_t submitted = 0;
        std::uint64_t completed = 0;  // jobs the worker ran
        std::uint64_t dropped = 0;    // queued jobs thrown away by cancel, after_running, release or stop_for_exit
        std::uint64_t refused = 0;    // submissions after stop_for_exit
        std::uint64_t waited = 0;     // cancel calls that waited for a running job
        std::uint64_t deferred = 0;   // after_running calls that left something to run after a running job
    };

    // False, with nothing queued, after stop_for_exit. A `graphics` job (a graphics pipeline build) may also run on a
    // second worker that takes only graphics jobs, so a compute build the driver takes minutes over does not hold
    // up later graphics pipelines. LEONIDA_GRAPHICS_COMPILE_WORKER=0 uses one shared worker.
    bool submit(const void* owner, const void* device, std::size_t priority, std::function<void()> work,
                bool graphics = false) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            ++stats_.refused;
            return false;
        }
        if (!worker_.joinable()) {
            const std::uint64_t generation = generation_;
            worker_ = std::thread([this, generation] { run(generation, false); });
        }
        if (graphics && graphics_worker_enabled() && !graphics_worker_.joinable()) {
            const std::uint64_t generation = generation_;
            graphics_worker_ = std::thread([this, generation] { run(generation, true); });
        }
        jobs_.emplace(priority, Job{owner, device, std::move(work), graphics});
        ++stats_.submitted;
        wake_.notify_all();
        return true;
    }

    // Lock-free: replay asks at every draw and dispatch whose pipeline is not built yet.
    bool closed() const { return closed_flag_.load(std::memory_order_acquire); }

    bool has_compile_waiter() {
        std::lock_guard lock(mutex_);
        return !jobs_.empty() && jobs_.begin()->first != SIZE_MAX;
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

    // True on the worker while it runs a job whose owner or device is `key` (not what runs after it).
    bool running_here(const void* key) {
        std::lock_guard lock(mutex_);
        const auto me = std::this_thread::get_id();
        return std::any_of(running_.begin(), running_.end(), [&](const Running& r) {
            return r.thread == me && !r.finished && (r.owner == key || r.device == key);
        });
    }

    // A device holds the worker while it lives. The last release drops whatever is still queued, stops the worker
    // and joins it; a release on the worker itself lets it return after the job it is in.
    void retain() {
        std::lock_guard lock(mutex_);
        ++users_;
    }
    void release() {
        std::vector<Job> dropped;
        Workers worker;
        {
            std::lock_guard lock(mutex_);
            if (users_ && --users_) return;
            worker = stop_locked(dropped);
        }
        finish(std::move(worker), true);
    }

    // Process exit with a device still alive: drops the queue, refuses new jobs and waits up to `wait` for a running
    // one, so ExitProcess does not end the worker inside the driver. False when a job was still running.
    bool stop_for_exit(std::chrono::milliseconds wait) {
        std::vector<Job> dropped;
        Workers worker;
        bool idle = true;
        {
            std::unique_lock lock(mutex_);
            closed_ = true;
            closed_flag_.store(true, std::memory_order_release);
            worker = stop_locked(dropped);
            const auto me = std::this_thread::get_id();
            idle = done_.wait_for(lock, wait, [&] {
                return std::none_of(running_.begin(), running_.end(), [&](const Running& r) { return r.thread != me; });
            });
        }
        finish(std::move(worker), idle);
        return idle;
    }

    Stats stats() {
        std::lock_guard lock(mutex_);
        return stats_;
    }
    bool worker_running() {
        std::lock_guard lock(mutex_);
        return worker_.joinable() || graphics_worker_.joinable();
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
    };
    static bool graphics_worker_enabled() {
        static const bool on = [] {
            wchar_t v[2]{};
            return !(GetEnvironmentVariableW(L"LEONIDA_GRAPHICS_COMPILE_WORKER", v, 2) == 1 && v[0] == L'0');
        }();
        return on;
    }
    // Both workers: the general one, and the graphics one when it was started.
    struct Workers {
        std::thread general, graphics;
    };
    struct Running {
        const void* owner;
        const void* device;
        std::thread::id thread;
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
        return {std::move(worker_), std::move(graphics_worker_)};
    }

    static void finish(std::thread worker, bool join) {
        if (!worker.joinable()) return;
        if (join && worker.get_id() != std::this_thread::get_id()) worker.join();
        else worker.detach();
    }
    static void finish(Workers workers, bool join) {
        finish(std::move(workers.general), join);
        finish(std::move(workers.graphics), join);
    }

    Running& mine(std::thread::id me) {
        return *std::find_if(running_.begin(), running_.end(), [me](const Running& r) { return r.thread == me; });
    }

    // The first job this worker may take, most urgent first: any job for the general worker, a graphics job for the
    // graphics one.
    std::multimap<std::size_t, Job>::iterator next_locked(bool graphics_only) {
        if (!graphics_only) return jobs_.begin();
        return std::find_if(jobs_.begin(), jobs_.end(), [](const auto& entry) { return entry.second.graphics; });
    }

    void run(std::uint64_t generation, bool graphics_only) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const auto me = std::this_thread::get_id();
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return generation_ != generation || next_locked(graphics_only) != jobs_.end(); });
            if (generation_ != generation) return;
            auto first = next_locked(graphics_only);
            Job job = std::move(first->second);
            jobs_.erase(first);
            running_.push_back({job.owner, job.device, me});
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
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::multimap<std::size_t, Job> jobs_;
    std::vector<Running> running_;  // one per worker; a stopped worker may still be finishing one
    std::thread worker_;
    std::thread graphics_worker_;  // graphics jobs only (submit's `graphics`), started at the first one
    std::uint64_t generation_ = 0;  // a worker runs while this is the value it started with
    unsigned users_ = 0;            // live devices (retain/release)
    bool closed_ = false;           // stop_for_exit: nothing more is queued
    std::atomic<bool> closed_flag_{false};  // closed_, for closed()
    Stats stats_;
    std::function<void(const void*)> probe_;
};
}  // namespace rt::d3d12
