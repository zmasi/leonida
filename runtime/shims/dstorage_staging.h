// Temporary payloads across all DirectStorage queues. A pair reserves both requests
// together so its ordered publisher never waits for space held by its own helper.
#pragma once

#include "dstorage_xs_abi.h"
#include "../src/dstorage_trace.h"
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace leonida::dstorage {

inline uint64_t staging_bytes(const dsx::DSTORAGE_REQUEST& r) {
    using namespace dsx;
    const auto o = r.Options.AsUINT64;
    const bool bc = options::bcpack(o) != 0;
    uint64_t input = r.SourceSize, peak = 0;
    const auto add_sat = [](uint64_t a, uint64_t b) {
        return a > UINT64_MAX - b ? UINT64_MAX : a + b;
    };
    const auto pages = [add_sat](uint64_t size, unsigned offset) -> uint64_t {
        if (!size) return 0;
        const uint64_t span = add_sat(size - 1, offset);
        const uint64_t count = 1 + span / 65536;
        const uint64_t table = count > UINT64_MAX / sizeof(uint64_t) ? UINT64_MAX : sizeof(uint64_t) * count;
        return add_sat(size, table);
    };
    if (o & options::kSourcePages) peak = pages(input, r.SourcePageOffset);
    const auto stage = [&](uint64_t output) {
        peak = (std::max)(peak, add_sat(input, output));
        input = output;
    };
    if (options::zlib(o)) stage(bc ? r.IntermediateSize : r.DestinationSize);
    if (bc) stage(r.DestinationSize);
    if (options::swizzle(o)) stage(r.DestinationSize);
    if (o & options::kDestinationPages) peak = (std::max)(peak, pages(input, r.DestinationPageOffset));
    return peak; // Raw buffer reads/copies allocate no private payload.
}

class StagingPool {
public:
    static constexpr uint64_t limit = 64ull << 20;
    // Requests above the shared window run alone. The validated ABI sizes bound
    // one transform to two 1 GiB buffers. No payload is allocated by the pool itself.
    class Lease {
    public:
        explicit Lease(StagingPool& pool) : pool_(pool) {}
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        bool try_pair(uint64_t bytes) {
            if (held_) return false;
            std::lock_guard lock(pool_.mutex_);
            if (pool_.helpers_ >= 2 || pool_.next_ != pool_.serving_ ||
                bytes > limit || pool_.used_ > limit - bytes) return false;
            ++pool_.helpers_;
            pair_ = true;
            held_ = true;
            take(bytes);
            return true;
        }
        void acquire(uint64_t bytes) {
            if (held_) release();
            if (!bytes) return;
            bool notify = false;
            {
                std::unique_lock lock(pool_.mutex_);
                const uint64_t ticket = pool_.next_++;
                pool_.changed_.wait(lock, [&] {
                    return ticket == pool_.serving_ && (!pool_.used_ ||
                           (bytes <= limit && pool_.used_ <= limit - bytes));
                });
                held_ = true;
                take(bytes);
                ++pool_.serving_;
                notify = pool_.next_ != pool_.serving_;
            }
            if (notify) pool_.changed_.notify_all();
        }
        void release() {
            if (!held_) return;
            bool notify = false;
            {
                std::lock_guard lock(pool_.mutex_);
                pool_.used_ -= bytes_;
                if (pair_) --pool_.helpers_;
                bytes_ = 0;
                pair_ = false;
                held_ = false;
                if (!pool_.used_ && pool_.drained_ < pool_.peak_) {
                    pool_.drained_ = pool_.peak_;
                    if (trace_enabled())
                        std::fprintf(stderr, "[rt] ds-staging idle_bytes=0 peak_bytes=%llu\n",
                                     static_cast<unsigned long long>(pool_.peak_));
                }
                notify = pool_.next_ != pool_.serving_;
            }
            if (notify) pool_.changed_.notify_all();
        }
        ~Lease() { release(); }
    private:
        void take(uint64_t bytes) {
            bytes_ = bytes;
            pool_.used_ += bytes;
            if (pool_.used_ > pool_.peak_) {
                pool_.peak_ = pool_.used_;
                if (trace_enabled())
                    std::fprintf(stderr, "[rt] ds-staging peak_bytes=%llu limit_bytes=%llu\n",
                                 static_cast<unsigned long long>(pool_.peak_),
                                 static_cast<unsigned long long>(limit));
            }
        }
        StagingPool& pool_;
        uint64_t bytes_{};
        bool pair_{};
        bool held_{};
    };

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    unsigned helpers_{};
    uint64_t used_{}, peak_{}, drained_{}, next_{}, serving_{};
};

inline StagingPool& staging_pool() {
    static auto* pool = new StagingPool;
    return *pool;
}

} // namespace leonida::dstorage
