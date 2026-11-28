// A bounded pool that compiles the pipelines of one submission side by side (docs/d3d12_vulkan.md, "Pipeline creation").
//
// run() is a blocking parallel loop: the calling thread works on its own batch beside the workers, so a batch completes
// even with no worker left (no thread could be started, or stop() was called), and nothing outlives the call. Several
// threads may call run() at once; they share the workers. The workers are started by the first batch that can use them,
// so a device that never compiles at draw time has none. They are plain std::threads: a driver's compiler needs the 8 MB
// of stack the loader's header gives those (docs/pipeline_check.md, "Stack reserve").
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace leonida {

class PipelineWorkers {
public:
    static constexpr unsigned kMaxThreads = 64;

    // What the pool has done: batches handed to the workers, the jobs in them, and the jobs running now.
    struct Stats {
        unsigned threads = 0;     // the most workers the pool starts
        std::size_t started = 0;  // the workers it has started
        std::uint64_t batches = 0;
        std::uint64_t jobs = 0;
        std::size_t running = 0;
    };

    explicit PipelineWorkers(unsigned threads) : limit_(std::min(threads, kMaxThreads)) {}
    ~PipelineWorkers() { stop(); }
    PipelineWorkers(const PipelineWorkers&) = delete;
    PipelineWorkers& operator=(const PipelineWorkers&) = delete;

    unsigned threads() const { return limit_; }

    // Calls job(0) .. job(count - 1) and returns when every call has returned. When calls threw, rethrows what the
    // lowest of them threw, after the last has returned. With no workers or a single job the calls are made in order on
    // this thread, and the first exception ends them.
    void run(std::size_t count, const std::function<void(std::size_t)>& job) {
        if (!limit_ || count < 2) {
            for (std::size_t i = 0; i < count; ++i) job(i);
            return;
        }
        Batch batch{&job, count};
        std::unique_lock lock(mutex_);
        start(count - 1);
        try {
            queue_.push_back(&batch);
        } catch (const std::bad_alloc&) {
            lock.unlock();
            for (std::size_t i = 0; i < count; ++i) job(i);
            return;
        }
        ++batches_;
        jobs_ += count;
        work_.notify_all();
        while (batch.next < batch.count) step(batch, lock);
        batch.done.wait(lock, [&] { return !batch.running; });
        lock.unlock();
        if (batch.error) std::rethrow_exception(batch.error);
    }

    // Ends the workers once each has finished the job it is in. A run() in progress on another thread finishes its
    // remaining jobs itself, and so does every later one.
    void stop() {
        std::vector<std::thread> threads;
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
            threads.swap(threads_);
        }
        work_.notify_all();
        for (std::thread& thread : threads) thread.join();
    }

    Stats stats() {
        std::lock_guard lock(mutex_);
        return {limit_, started_, batches_, jobs_, running_};
    }

private:
    struct Batch {  // on the stack of the run() that made it; guarded by mutex_
        const std::function<void(std::size_t)>* job;
        std::size_t count;
        std::size_t next = 0;
        std::size_t running = 0;
        std::exception_ptr error{};
        std::size_t error_index = 0;
        std::condition_variable done{};
    };

    // With mutex_ held: enough workers for `wanted` more jobs, within the limit.
    void start(std::size_t wanted) {
        wanted = std::min<std::size_t>(wanted, limit_);
        while (!stopping_ && !exhausted_ && threads_.size() < wanted) {
            try {
                threads_.emplace_back([this] { work(); });
                ++started_;
            } catch (const std::system_error&) {
                exhausted_ = true;  // the workers there are, and the callers, do it all
            } catch (const std::bad_alloc&) {
                break;  // out of memory growing the pool; existing workers and callers finish this batch
            }
        }
    }

    void work() {
        std::unique_lock lock(mutex_);
        for (;;) {
            work_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            step(*queue_.front(), lock);
        }
    }

    // Takes the next job of `batch` (there is one), runs it unlocked, and returns locked. The batch leaves the queue
    // with its last job, so no worker can reach it once run() has seen nothing left and nothing running.
    void step(Batch& batch, std::unique_lock<std::mutex>& lock) {
        const std::size_t index = batch.next++;
        if (batch.next == batch.count) std::erase(queue_, &batch);
        ++batch.running;
        ++running_;
        lock.unlock();
        std::exception_ptr error;
        try {
            (*batch.job)(index);
        } catch (...) {
            error = std::current_exception();
        }
        lock.lock();
        --running_;
        if (error && (!batch.error || index < batch.error_index)) {
            batch.error = std::move(error);
            batch.error_index = index;
        }
        // Notified with the mutex held: run() cannot return, and end the batch, before this has.
        if (!--batch.running && batch.next == batch.count) batch.done.notify_all();
    }

    const unsigned limit_;
    std::mutex mutex_;
    std::condition_variable work_;
    std::deque<Batch*> queue_;  // batches with jobs nobody has taken yet
    std::vector<std::thread> threads_;
    bool stopping_ = false;
    bool exhausted_ = false;
    std::size_t started_ = 0;
    std::uint64_t batches_ = 0;
    std::uint64_t jobs_ = 0;
    std::size_t running_ = 0;
};

// LEONIDA_PIPELINE_THREADS: how many workers, 0 for none (pipelines compile on the thread that submits, as before).
// Unset or empty: half the logical processors and at most 8, past which the compiles of one submission gained little
// (docs/d3d12_vulkan.md). `valid` is false for text that is not a number, which counts as unset.
struct PipelineThreads {
    unsigned count = 0;
    bool valid = true;
};

inline PipelineThreads pipeline_threads(const char* text, unsigned logical_processors) {
    const unsigned automatic = std::min(8u, logical_processors / 2);
    if (!text || !*text) return {automatic, true};
    unsigned value = 0;
    for (const char* c = text; *c; ++c) {
        if (*c < '0' || *c > '9') return {automatic, false};
        value = std::min(PipelineWorkers::kMaxThreads, value * 10 + unsigned(*c - '0'));
    }
    return {value, true};
}

}  // namespace leonida
