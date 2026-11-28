// CPU-only regression: independent queues can finish in the opposite order to host submission.
#include "../src/retire_tickets.h"

#include <array>
#include <cstdint>
#include <cstdio>

int main() {
    leonida::RetireTickets<int> tickets;
    std::array<std::uint64_t, 3> done{};
    bool queried_destroyed = false;
    bool queue_one_destroyed = false;
    const auto query = [&](int queue, std::uint64_t value) {
        if (queue == 1 && queue_one_destroyed) queried_destroyed = true;
        return done[queue] >= value;
    };

    tickets.record(1, 1, 1);  // first queue waits on a fence
    tickets.record(2, 2, 1);  // second queue signals it and finishes first
    done[2] = 1;
    if (tickets.completed(query) != 0) return 1;  // ticket 2 cannot retire ticket 1's resources
    done[1] = 1;
    if (tickets.completed(query) != 2) return 2;

    tickets.record(3, 1, 2);
    tickets.record(4, 2, 2);
    done[2] = 2;
    if (tickets.completed(query) != 2) return 3;
    tickets.queue_idle(1);  // its progress semaphore can now be destroyed
    queue_one_destroyed = true;
    if (tickets.completed(query) != 4 || queried_destroyed) return 4;

    // pending_through() must coalesce repeated submissions on the same queue to the highest
    // timeline value, omit idle queues, and exclude tickets past the requested cutoff.
    tickets.record(5, 1, 10);
    tickets.record(6, 2, 20);
    tickets.record(7, 1, 12);
    tickets.record(8, 1, 15);
    tickets.record(9, 2, 25);
    tickets.queue_idle(2);
    const auto wait_list = tickets.pending_through(7);
    if (wait_list.size() != 1 || wait_list[0].first != 1 || wait_list[0].second != 12) return 5;

    // A non-monotonic ticket must not move the completed prefix backward.
    done[1] = 15;
    if (tickets.completed(query) != 9) return 6;
    tickets.record(3, 1, 16);
    done[1] = 16;
    if (tickets.completed(query) != 9) return 7;

    // Repeated or lower timeline values on the same semaphore within one completed() pass skip
    // redundant semaphore queries while still retiring the contiguous prefix.
    {
        leonida::RetireTickets<int> cached;
        std::uint64_t queries = 0;
        cached.record(10, 1, 5);
        cached.record(11, 1, 5);
        cached.record(12, 1, 4);
        cached.record(13, 2, 3);
        cached.record(14, 1, 5);
        cached.record(15, 1, 6);
        const auto retired = cached.completed([&](int sem, std::uint64_t value) {
            ++queries;
            return (sem == 1 && value <= 5) || (sem == 2 && value <= 3);
        });
        // Expect 3 queries: (1, 5) true, (2, 3) true, (1, 6) false; tickets 11, 12, 14 hit cache.
        if (retired != 14 || queries != 3) return 8;
    }

    // More than 8 distinct semaphores exercise cache eviction cleanly.
    {
        leonida::RetireTickets<int> many;
        std::uint64_t queries = 0;
        for (int sem = 1; sem <= 12; ++sem) many.record(static_cast<std::uint64_t>(sem), sem, 1);
        many.record(13, 12, 1);  // still resident in cache
        const auto retired = many.completed([&](int, std::uint64_t) {
            ++queries;
            return true;
        });
        if (retired != 13 || queries != 12) return 9;
    }

    // pending_through() with non-monotonic tickets still collects entries past a higher earlier ticket,
    // and resets monotonic mode once the pending deque drains.
    {
        leonida::RetireTickets<int> mixed;
        if (!mixed.pending_through(100).empty()) return 10;
        mixed.record(10, 1, 1);
        mixed.record(20, 2, 2);
        mixed.record(15, 3, 3);  // out of order
        const auto before = mixed.pending_through(16);
        if (before.size() != 2 || before[0].first != 1 || before[0].second != 1 ||
            before[1].first != 3 || before[1].second != 3)
            return 11;
        if (mixed.completed([](int, std::uint64_t) { return true; }) != 20) return 12;

        // After draining, monotonic fast path works cleanly including cutoff before front.
        mixed.record(30, 1, 5);
        mixed.record(31, 2, 6);
        mixed.record(32, 1, 7);
        if (!mixed.pending_through(29).empty()) return 13;
        const auto cut = mixed.pending_through(31);
        if (cut.size() != 2 || cut[0].first != 1 || cut[0].second != 5 ||
            cut[1].first != 2 || cut[1].second != 6)
            return 14;
    }

    std::puts("out-of-order queue completion kept the retirement prefix safe");
    return 0;
}
