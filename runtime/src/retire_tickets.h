#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace leonida {

// Global submission tickets may finish out of order on independent Vulkan queues. Only the
// contiguous completed prefix is safe for deferred resource destruction. The caller serializes
// record(), completed() and queue_idle() with the device submission mutex. record() must
// receive strictly increasing global ticket numbers; otherwise a duplicate or backward ticket
// can make the completed prefix stall or move backward.
template<class Semaphore>
class RetireTickets {
public:
    void record(std::uint64_t ticket, Semaphore semaphore, std::uint64_t value) {
        if (pending_.empty()) {
            monotonic_ = true;
        } else if (ticket < pending_.back().ticket) {
            monotonic_ = false;
        }
        pending_.push_back({ticket, semaphore, value, false});
    }

    template<class Query>
    std::uint64_t completed(Query&& query) {
        // Cache the highest timeline value confirmed done per semaphore in this pass so
        // repeated or lower values on the same queue skip redundant Vulkan counter queries.
        std::array<std::pair<Semaphore, std::uint64_t>, 8> confirmed{};
        std::size_t confirmed_count = 0;
        std::size_t next_evict = 0;

        while (!pending_.empty()) {
            const Entry& front = pending_.front();
            if (!front.idle) {
                bool already_done = false;
                for (std::size_t i = 0; i < confirmed_count; ++i) {
                    if (confirmed[i].first == front.semaphore) {
                        if (front.value <= confirmed[i].second) already_done = true;
                        break;
                    }
                }
                if (!already_done) {
                    if (!query(front.semaphore, front.value)) break;
                    bool updated = false;
                    for (std::size_t i = 0; i < confirmed_count; ++i) {
                        if (confirmed[i].first == front.semaphore) {
                            if (front.value > confirmed[i].second) confirmed[i].second = front.value;
                            updated = true;
                            break;
                        }
                    }
                    if (!updated) {
                        if (confirmed_count < confirmed.size()) {
                            confirmed[confirmed_count++] = {front.semaphore, front.value};
                        } else {
                            confirmed[next_evict] = {front.semaphore, front.value};
                            next_evict = (next_evict + 1) % confirmed.size();
                        }
                    }
                }
            }
            if (front.ticket > completed_) completed_ = front.ticket;
            pending_.pop_front();
        }
        if (pending_.empty()) monotonic_ = true;
        return completed_;
    }

    // The (semaphore, value) of every submission up to `ticket` that has not been seen complete: what a caller
    // that must have them done (and may not hold the lock that guards this object while it waits) waits for.
    std::vector<std::pair<Semaphore, std::uint64_t>> pending_through(std::uint64_t ticket) const {
        std::vector<std::pair<Semaphore, std::uint64_t>> out;
        if (pending_.empty() || (monotonic_ && ticket < pending_.front().ticket)) return out;
        for (const Entry& entry : pending_) {
            if (entry.ticket > ticket) {
                if (monotonic_) break;
                continue;
            }
            if (entry.idle) continue;
            bool merged = false;
            for (auto& [sem, val] : out) {
                if (sem == entry.semaphore) {
                    if (entry.value > val) val = entry.value;
                    merged = true;
                    break;
                }
            }
            if (!merged) out.emplace_back(entry.semaphore, entry.value);
        }
        return out;
    }

    // Call only after the owning queue's wait-idle succeeded. Merely requesting idle does not
    // prove its submissions completed and must not advance the retirement prefix.
    void queue_idle(Semaphore semaphore) {
        for (Entry& entry : pending_)
            if (entry.semaphore == semaphore) entry.idle = true;
    }

private:
    struct Entry {
        std::uint64_t ticket;
        Semaphore semaphore;
        std::uint64_t value;
        bool idle;
    };
    std::deque<Entry> pending_;
    std::uint64_t completed_ = 0;
    bool monotonic_ = true;
};

}  // namespace leonida
