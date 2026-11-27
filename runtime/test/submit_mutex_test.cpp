// leonida::SubmitMutex (src/vulkan_device_fault.h), on the CPU: the submit lock's owner and hold tracking, its waiter
// count, let_submit_finish (the handoff a pipeline create makes to submits before entering the driver) and the
// compile gate between pipeline creates and queue submissions. Windows only: the owner is a Win32 thread id. No GPU,
// no game data.
#include "../src/vulkan_device_fault.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); return 1; } \
} while (0)

namespace {

using leonida::SubmitMutex;
using namespace std::chrono_literals;

std::int64_t now_ms() { return leonida::diagnostic_ms(); }

// Polls `done` for up to two seconds.
template<class Done> bool eventually(Done&& done) {
    for (int i = 0; i < 2000; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return done();
}

int owner_tracking() {
    SubmitMutex m;
    EXPECT(m.owner() == 0 && m.waiting() == 0);
    const std::int64_t before = now_ms();
    m.lock();
    EXPECT(m.owner() == GetCurrentThreadId());
    EXPECT(m.since_ms() >= before && m.since_ms() <= now_ms());
    // The holder's own thread: nothing to let through, no wait (it would deadlock on the lock it holds).
    EXPECT(m.let_submit_finish(true) == 0);
    EXPECT(m.let_submit_finish(false) == 0);
    m.unlock();
    EXPECT(m.owner() == 0);
    EXPECT(m.try_lock());
    EXPECT(m.owner() == GetCurrentThreadId());
    m.unlock();
    // Free: no wait at all.
    EXPECT(m.let_submit_finish(true) == 0);
    std::printf("owner and hold tracking, holder's own thread returns at once\n");
    return 0;
}

int waits_for_holder() {
    SubmitMutex m;
    std::atomic<bool> held{false}, released{false};
    std::thread holder([&] {
        m.lock();
        held = true;
        std::this_thread::sleep_for(200ms);
        released = true;
        m.unlock();
    });
    EXPECT(eventually([&] { return held.load(); }));
    EXPECT(m.owner() != 0 && m.owner() != GetCurrentThreadId());
    const std::int64_t waited = m.let_submit_finish(false);
    const bool after_release = released.load();
    holder.join();
    EXPECT(after_release);
    EXPECT(waited >= 100);
    std::printf("let_submit_finish(false) waited %lld ms for another thread's hold\n", static_cast<long long>(waited));
    return 0;
}

int waits_for_queued_submitter() {
    // A holds the lock, B queues behind it in lock(); the worker's handoff returns only after B has had its turn.
    SubmitMutex m;
    std::atomic<bool> a_held{false}, b_done{false};
    std::atomic<std::int64_t> b_released_at{0};
    std::thread a([&] {
        m.lock();
        a_held = true;
        std::this_thread::sleep_for(150ms);
        m.unlock();
    });
    EXPECT(eventually([&] { return a_held.load(); }));
    std::thread b([&] {
        m.lock();
        std::this_thread::sleep_for(100ms);
        b_released_at = now_ms();
        b_done = true;
        m.unlock();
    });
    EXPECT(eventually([&] { return m.waiting() == 1; }));  // B is counted while it waits
    const std::int64_t waited = m.let_submit_finish(true);
    const bool b_had_turn = b_done.load();
    const std::int64_t returned_at = now_ms();
    a.join();
    b.join();
    EXPECT(b_had_turn);
    EXPECT(returned_at >= b_released_at.load());
    EXPECT(m.waiting() == 0 && m.owner() == 0);
    std::printf("let_submit_finish(true) let a queued submitter go first (%lld ms)\n", static_cast<long long>(waited));
    return 0;
}

int handoff_is_capped() {
    // Two threads keep the lock busy back to back (one always queued): the worker still gets its turn after the cap.
    SubmitMutex m;
    std::atomic<bool> stop{false};
    auto churn = [&] {
        while (!stop) {
            m.lock();
            std::this_thread::sleep_for(10ms);
            m.unlock();
        }
    };
    std::thread a(churn), b(churn);
    EXPECT(eventually([&] { return m.owner() != 0 && m.waiting() != 0; }));
    const std::int64_t waited = m.let_submit_finish(true, 100);
    stop = true;
    a.join();
    b.join();
    std::printf("capped handoff: %lld ms\n", static_cast<long long>(waited));
    EXPECT(waited >= 90 && waited < 600);
    std::printf("let_submit_finish(true, 100) under a steady stream of submits returned after %lld ms\n",
                static_cast<long long>(waited));
    return 0;
}

int compile_gate() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): not checked\n");
        return 0;
    }
    SubmitMutex m;
    // Nothing in the driver: a submission passes at once.
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        EXPECT(now_ms() - start < 50);
    }
    // A submission waits for the create in the driver; a create arriving meanwhile waits behind the submission.
    std::atomic<bool> creating{false};
    std::atomic<std::int64_t> create_left_at{0}, submit_entered_at{0}, submit_left_at{0}, second_entered_at{0};
    std::atomic<std::int64_t> second_waited{-1};
    std::thread create([&] {
        const SubmitMutex::CompileTurn turn(m);
        creating = true;
        std::this_thread::sleep_for(250ms);
        create_left_at = now_ms();
    });
    EXPECT(eventually([&] { return creating.load(); }));
    std::thread submit([&] {
        m.lock();  // as a submitter does: the submit lock, then the gate around vkQueueSubmit
        {
            const SubmitMutex::SubmitTurn turn(m);
            submit_entered_at = now_ms();
            std::this_thread::sleep_for(100ms);
            submit_left_at = now_ms();
        }
        m.unlock();
    });
    EXPECT(eventually([&] { return m.gate_waiter() != 0; }));  // waiting at the gate, with the submit lock held
    EXPECT(m.owner() == m.gate_waiter());
    std::thread second([&] {
        const SubmitMutex::CompileTurn turn(m);
        second_entered_at = now_ms();
        second_waited = turn.waited_ms();
    });
    create.join();
    submit.join();
    second.join();
    EXPECT(submit_entered_at.load() >= create_left_at.load());   // never in the driver together with a create
    EXPECT(second_entered_at.load() >= submit_left_at.load());   // the waiting submission went first
    EXPECT(second_waited.load() >= 50);
    EXPECT(m.gate_waiter() == 0);
    const SubmitMutex::GateStats stats = m.gate_stats();
    EXPECT(stats.submits == 1 && stats.submit_max_ms >= 100);
    EXPECT(stats.creates == 1 && stats.create_max_ms >= 50);
    std::printf("compile gate: submission waited %lld ms for a create, a later create waited %lld ms behind it\n",
                static_cast<long long>(stats.submit_max_ms), static_cast<long long>(stats.create_max_ms));
    // Creates share the gate.
    {
        const SubmitMutex::CompileTurn first(m);
        std::atomic<std::int64_t> other_waited{-1};
        std::thread other([&] {
            const SubmitMutex::CompileTurn turn(m);
            other_waited = turn.waited_ms();
        });
        other.join();
        EXPECT(other_waited.load() == 0);
    }
    std::printf("compile gate: creates pass it together\n");
    // wait_for_compile_gate_drain returns at once when no submission waits, and blocks until a queued submission leaves
    // without entering the compile gate itself (no increment to stats.creates).
    EXPECT(m.wait_for_compile_gate_drain() == 0);
    {
        const std::uint64_t creates_before = m.gate_stats().creates;
        std::atomic<bool> in_compile{false};
        std::atomic<std::int64_t> sub_left_at{0}, drained_at{0}, drained_ms{-1};
        std::thread holder([&] {
            const SubmitMutex::CompileTurn turn(m);
            in_compile = true;
            std::this_thread::sleep_for(120ms);
        });
        EXPECT(eventually([&] { return in_compile.load(); }));
        std::thread sub([&] {
            m.lock();
            {
                const SubmitMutex::SubmitTurn turn(m);
                std::this_thread::sleep_for(80ms);
                sub_left_at = now_ms();
            }
            m.unlock();
        });
        EXPECT(eventually([&] { return m.gate_waiter() != 0; }));
        std::thread drainer([&] {
            drained_ms = m.wait_for_compile_gate_drain();
            drained_at = now_ms();
        });
        holder.join();
        sub.join();
        drainer.join();
        EXPECT(drained_at.load() >= sub_left_at.load());
        EXPECT(drained_ms.load() >= 50);
        EXPECT(m.gate_stats().creates == creates_before);
    }
    std::printf("compile gate: drain waits for queued submission without taking a compile turn\n");
    return 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (owner_tracking() || waits_for_holder() || waits_for_queued_submitter() || handoff_is_capped() ||
        compile_gate())
        return 1;
    std::printf("submit_mutex_test: all passed\n");
    return 0;
}
