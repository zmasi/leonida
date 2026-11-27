// leonida::SubmitMutex (src/vulkan_device_fault.h), on the CPU: the submit lock's owner and hold tracking, its waiter
// count, let_submit_finish (the handoff a pipeline create makes to submits before entering the driver) and the
// compile gate between pipeline creates and queue submissions. Windows only: the owner is a Win32 thread id. No GPU,
// no game data.
#include "../src/vulkan_device_fault.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
    m.set_gate_wait_limit_ms(0);  // no limit: gate_wait_limit checks the limit
    m.set_gate_adaptive(false);   // gate_adapts checks the switch
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
    return 0;
}

int gate_wait_limit() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): wait limit not checked\n");
        return 0;
    }
    // The default with LEONIDA_COMPILE_GATE_WAIT_MS unset, whatever the title watchdog variables say: none with
    // LEONIDA_COMPILE_GATE=1 or unset (classic), kDefaultGateWaitMs with LEONIDA_COMPILE_GATE=adaptive.
    const bool adaptive = SubmitMutex::compile_gate_mode() == SubmitMutex::GateMode::adaptive;
    if (!std::getenv("LEONIDA_COMPILE_GATE_WAIT_MS"))
        EXPECT(SubmitMutex::default_gate_wait_limit_ms() == (adaptive ? SubmitMutex::kDefaultGateWaitMs : 0));
    SubmitMutex m;
    m.set_gate_adaptive(false);  // as LEONIDA_COMPILE_GATE=1: it never turns off
    m.set_gate_wait_limit_ms(150);
    EXPECT(m.gate_wait_limit_ms() == 150);
    // A create that stays in the driver until it is let go (one that takes longer than the limit).
    std::atomic<bool> long_in{false}, release_long{false};
    std::atomic<std::int64_t> long_left_at{0};
    std::thread long_create([&] {
        const SubmitMutex::CompileTurn turn(m);
        long_in = true;
        while (!release_long) std::this_thread::sleep_for(1ms);
        long_left_at = now_ms();
    });
    EXPECT(eventually([&] { return long_in.load(); }));
    // The submission goes ahead at the limit, the create still in the driver.
    std::int64_t first_waited = 0;
    bool first_ahead = false;
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        first_waited = now_ms() - start;
        first_ahead = turn.went_ahead();
    }
    // The next one passes at once: the create the first went past holds no submission back.
    std::int64_t second_waited = 0;
    bool second_ahead = true;
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        second_waited = now_ms() - start;
        second_ahead = turn.went_ahead();
    }
    // A create that enters after that is waited for again, until it leaves (well within the limit now), while the
    // long one is still in the driver.
    m.set_gate_wait_limit_ms(5000);
    std::atomic<bool> short_in{false};
    std::atomic<std::int64_t> short_left_at{0};
    std::thread short_create([&] {
        const SubmitMutex::CompileTurn turn(m);
        short_in = true;
        std::this_thread::sleep_for(200ms);
        short_left_at = now_ms();
    });
    const bool short_entered = eventually([&] { return short_in.load(); });
    std::int64_t third_entered_at = 0;
    bool third_ahead = true;
    {
        const SubmitMutex::SubmitTurn turn(m);
        third_entered_at = now_ms();
        third_ahead = turn.went_ahead();
    }
    const bool long_still_in = long_left_at.load() == 0;
    release_long = true;
    long_create.join();
    short_create.join();
    EXPECT(short_entered);
    EXPECT(first_ahead && first_waited >= 140 && first_waited < 1000);
    EXPECT(!second_ahead && second_waited < 50);
    EXPECT(!third_ahead && third_entered_at >= short_left_at.load() && long_still_in);
    const SubmitMutex::GateStats stats = m.gate_stats();
    EXPECT(stats.submits == 2 && stats.ahead == 1);
    EXPECT(m.gate_verdict() == SubmitMutex::GateVerdict::pending);  // quick probes, but not adaptive
    // Both gone: a submission passes at once.
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        EXPECT(now_ms() - start < 50 && !turn.went_ahead());
    }
    std::printf("compile gate: a submission went ahead after %lld ms (limit 150 ms) with the create still in the "
                "driver, the next passed in %lld ms, and a create that entered later was waited for again\n",
                static_cast<long long>(first_waited), static_cast<long long>(second_waited));
    return 0;
}

int gate_adapts() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): adaptive switch not checked\n");
        return 0;
    }
    using Verdict = SubmitMutex::GateVerdict;
    // Quick probes: the gate goes off once two, a second apart, came back with the create still in the driver.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);  // probes at the limit only: gate_probe_budget checks the budget
        std::atomic<bool> in{false}, release{false};
        std::thread create([&] {
            const SubmitMutex::CompileTurn turn(m);
            in = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        const bool entered = eventually([&] { return in.load(); });
        bool first_ahead = false;
        {
            const SubmitMutex::SubmitTurn turn(m);  // goes ahead at the limit: the first quick probe
            first_ahead = turn.went_ahead();
        }
        { const SubmitMutex::SubmitTurn turn(m); }  // the second, too soon after the first
        const Verdict soon = m.gate_verdict();
        std::this_thread::sleep_for(1100ms);
        { const SubmitMutex::SubmitTurn turn(m); }  // the third, a second later
        const Verdict later = m.gate_verdict();
        // Off: a create that enters now holds no submission back.
        std::atomic<bool> fresh_in{false}, release_fresh{false};
        std::thread fresh([&] {
            const SubmitMutex::CompileTurn turn(m);
            fresh_in = true;
            while (!release_fresh) std::this_thread::sleep_for(1ms);
        });
        const bool fresh_entered = eventually([&] { return fresh_in.load(); });
        const std::int64_t start = now_ms();
        bool fresh_ahead = true;
        {
            const SubmitMutex::SubmitTurn turn(m);
            fresh_ahead = turn.went_ahead();
        }
        const std::int64_t passed = now_ms() - start;
        release = true;
        release_fresh = true;
        create.join();
        fresh.join();
        EXPECT(entered && fresh_entered && first_ahead);
        EXPECT(soon == Verdict::pending);
        EXPECT(later == Verdict::driver_does_not_hold);
        EXPECT(passed < 50 && !fresh_ahead);
    }
    std::printf("compile gate: off after two quick probes a second apart, not after two within the second\n");
    // A held probe: a submission that comes back just after the create beside it leaves. The gate stays on, and a
    // later create is waited for however long it takes.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);  // probes at the limit only: gate_probe_budget checks the budget
        std::atomic<bool> first_in{false}, first_release{false};
        std::thread first([&] {
            const SubmitMutex::CompileTurn turn(m);
            first_in = true;
            while (!first_release) std::this_thread::sleep_for(1ms);
        });
        const bool first_entered = eventually([&] { return first_in.load(); });
        {
            const SubmitMutex::SubmitTurn turn(m);  // goes ahead, then stays "in the driver" for 2.1 s
            std::this_thread::sleep_for(2000ms);
            first_release = true;  // the create leaves 100 ms before the submission comes back
            first.join();
            std::this_thread::sleep_for(100ms);
        }
        const Verdict held = m.gate_verdict();
        // A create of the same kind is waited for, past the limit (its kind holds submissions), until it leaves.
        std::atomic<bool> in{false}, release{false};
        std::thread create([&] {
            const SubmitMutex::CompileTurn turn(m);
            in = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        const bool entered = first_entered && eventually([&] { return in.load(); });
        std::atomic<std::int64_t> released_at{0};
        std::thread releaser([&] {
            std::this_thread::sleep_for(300ms);
            released_at = now_ms();
            release = true;
        });
        std::int64_t entered_at = 0;
        bool ahead = true;
        {
            const SubmitMutex::SubmitTurn turn(m);
            entered_at = now_ms();
            ahead = turn.went_ahead();
        }
        releaser.join();
        create.join();
        const Verdict still = m.gate_verdict();
        EXPECT(entered);
        EXPECT(held == Verdict::driver_holds && still == Verdict::driver_holds);
        EXPECT(!ahead && entered_at >= released_at.load());
    }
    std::printf("compile gate: a probe held 2.1 s keeps it on, and the create is then waited out past the limit\n");
    // A slow submission that comes back with the create beside it still there was not held by it: no verdict.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);
        std::atomic<bool> in{false}, release{false};
        std::thread create([&] {
            const SubmitMutex::CompileTurn turn(m);
            in = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        const bool entered = eventually([&] { return in.load(); });
        {
            const SubmitMutex::SubmitTurn turn(m);  // goes ahead at the limit, then stays 2.1 s
            std::this_thread::sleep_for(2100ms);
        }
        const Verdict after = m.gate_verdict();
        release = true;
        create.join();
        EXPECT(entered);
        EXPECT(after != Verdict::driver_holds);
    }
    std::printf("compile gate: a 2.1 s submission that came back with the create still there is not a held probe\n");
    return 0;
}

int gate_probe_budget() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): probe budget not checked\n");
        return 0;
    }
    using Verdict = SubmitMutex::GateVerdict;
    // The default budget, 0 when the limit is.
    if (!std::getenv("LEONIDA_COMPILE_GATE_PROBE_MS"))
        EXPECT(SubmitMutex::default_gate_probe_budget_ms() ==
               (SubmitMutex::default_gate_wait_limit_ms() ? SubmitMutex::kDefaultProbeBudgetMs : 0));
    // Short waits add up: with no per-wait limit, a submission goes ahead once the waits of the window reach the budget.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(0);
        m.set_gate_probe_budget_ms(300);
        EXPECT(m.gate_probe_budget_ms() == 300);
        std::atomic<bool> a_in{false};
        std::thread a([&] {
            const SubmitMutex::CompileTurn turn(m);
            a_in = true;
            std::this_thread::sleep_for(200ms);
        });
        const bool a_entered = eventually([&] { return a_in.load(); });
        std::int64_t first_waited = 0;
        bool first_ahead = true;
        {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m);  // waits the create out: under the budget
            first_waited = now_ms() - start;
            first_ahead = turn.went_ahead();
        }
        a.join();
        std::atomic<bool> b_in{false}, release{false};
        std::thread b([&] {
            const SubmitMutex::CompileTurn turn(m);
            b_in = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        const bool b_entered = eventually([&] { return b_in.load(); });
        std::int64_t second_waited = 0;
        bool second_ahead = false;
        {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m);  // goes ahead once both waits add up to 300 ms
            second_waited = now_ms() - start;
            second_ahead = turn.went_ahead();
        }
        const Verdict after_one = m.gate_verdict();
        release = true;
        b.join();
        EXPECT(a_entered && b_entered);
        EXPECT(!first_ahead && first_waited >= 100);
        EXPECT(second_ahead && first_waited + second_waited >= 250 && second_waited < 300);
        EXPECT(after_one == Verdict::pending);  // one quick probe is not enough
        std::printf("compile gate: waits of %lld and %lld ms reached the 300 ms probe budget, the second went ahead\n",
                    static_cast<long long>(first_waited), static_cast<long long>(second_waited));
    }
    // After a held verdict the budget no longer applies: a submission waits a create out again.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(0);
        m.set_gate_probe_budget_ms(100);
        std::atomic<bool> in{false}, release{false};
        std::thread create([&] {
            const SubmitMutex::CompileTurn turn(m);
            in = true;
            while (!release) std::this_thread::sleep_for(1ms);
        });
        const bool entered = eventually([&] { return in.load(); });
        bool ahead = false;
        {
            const SubmitMutex::SubmitTurn turn(m);  // goes ahead at the budget, then is held 2.1 s
            ahead = turn.went_ahead();
            std::this_thread::sleep_for(2000ms);
            release = true;  // the create it stayed beside leaves 100 ms before it comes back: a held probe
            create.join();
            std::this_thread::sleep_for(100ms);
        }
        const Verdict held = m.gate_verdict();
        std::atomic<bool> short_in{false};
        std::atomic<std::int64_t> short_left_at{0};
        std::thread short_create([&] {
            const SubmitMutex::CompileTurn turn(m);
            short_in = true;
            std::this_thread::sleep_for(400ms);
            short_left_at = now_ms();
        });
        const bool short_entered = eventually([&] { return short_in.load(); });
        std::int64_t entered_at = 0;
        bool later_ahead = true;
        {
            const SubmitMutex::SubmitTurn turn(m);
            entered_at = now_ms();
            later_ahead = turn.went_ahead();
        }
        short_create.join();
        EXPECT(entered && short_entered && ahead);
        EXPECT(held == Verdict::driver_holds);
        EXPECT(!later_ahead && entered_at >= short_left_at.load());
    }
    std::printf("compile gate: after a held probe a 400 ms create is waited out past the 100 ms budget\n");
    return 0;
}

int gate_per_kind() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): per-kind verdicts not checked\n");
        return 0;
    }
    using Verdict = SubmitMutex::GateVerdict;
    using Kind = SubmitMutex::CreateKind;
    SubmitMutex m;
    m.set_gate_adaptive(true);
    m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
    m.set_gate_wait_limit_ms(100);
    m.set_gate_probe_budget_ms(0);
    const SubmitMutex::CreateInfo compute{Kind::compute, "test compute", "test", 1000};
    const SubmitMutex::CreateInfo graphics{Kind::graphics, "test mesh", "test", 2000};
    // A compute create that stays until let go; the compute gate goes off after two quick probes a second apart.
    std::atomic<bool> c_in{false}, c_release{false};
    std::atomic<std::int64_t> c_left_at{0};
    std::thread c([&] {
        const SubmitMutex::CompileTurn turn(m, compute);
        c_in = true;
        while (!c_release) std::this_thread::sleep_for(1ms);
        c_left_at = now_ms();
    });
    const bool c_entered = eventually([&] { return c_in.load(); });
    { const SubmitMutex::SubmitTurn turn(m); }  // goes ahead at the limit: the first quick probe
    std::this_thread::sleep_for(1100ms);
    { const SubmitMutex::SubmitTurn turn(m); }  // the second
    const Verdict compute_off = m.gate_verdict(Kind::compute), graphics_before = m.gate_verdict(Kind::graphics);
    // A graphics create is still waited for: its kind has no verdict, so the submission goes ahead only at the limit.
    std::atomic<bool> g_in{false}, g_release{false};
    std::thread g([&] {
        const SubmitMutex::CompileTurn turn(m, graphics);
        g_in = true;
        while (!g_release) std::this_thread::sleep_for(1ms);
    });
    const bool g_entered = eventually([&] { return g_in.load(); });
    std::int64_t g_waited = 0;
    bool g_ahead = false;
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        g_waited = now_ms() - start;
        g_ahead = turn.went_ahead();
    }
    g_release = true;
    g.join();
    const Verdict graphics_after = m.gate_verdict(Kind::graphics);  // one quick probe: still pending
    // A second compute create, there through the rest.
    std::atomic<bool> c2_in{false}, c2_release{false};
    std::atomic<std::int64_t> c2_left_at{0};
    std::thread c2([&] {
        const SubmitMutex::CompileTurn turn(m, compute);
        c2_in = true;
        while (!c2_release) std::this_thread::sleep_for(1ms);
        c2_left_at = now_ms();
    });
    const bool c2_entered = eventually([&] { return c2_in.load(); });
    // Re-arm: beside the compute creates (gate off), a submission stays 2.1 s in the "driver" and comes back 100 ms
    // after the first one leaves.
    {
        const SubmitMutex::SubmitTurn turn(m);
        std::this_thread::sleep_for(2000ms);
        c_release = true;
        c.join();
        std::this_thread::sleep_for(100ms);
    }
    const Verdict compute_rearmed = m.gate_verdict(Kind::compute);
    // The second compute create is waited for, and not gone past at the limit: its kind holds submissions.
    std::atomic<std::int64_t> released_at{0};
    std::thread releaser([&] {
        std::this_thread::sleep_for(400ms);
        released_at = now_ms();
        c2_release = true;
    });
    std::int64_t entered_at = 0;
    bool after_ahead = true;
    {
        const SubmitMutex::SubmitTurn turn(m);
        entered_at = now_ms();
        after_ahead = turn.went_ahead();
    }
    releaser.join();
    c2.join();
    EXPECT(c_entered && c2_entered && g_entered);
    EXPECT(compute_off == Verdict::driver_does_not_hold && graphics_before == Verdict::pending);
    EXPECT(g_ahead && g_waited >= 90 && g_waited < 1000);
    EXPECT(graphics_after == Verdict::pending);
    EXPECT(compute_rearmed == Verdict::driver_holds);
    EXPECT(!after_ahead && entered_at >= c2_left_at.load() && entered_at >= released_at.load());
    m.report_creates_on_loss();  // nothing in the driver now: the verdicts only
    std::printf("compile gate: compute off with graphics still gated; a 2.1 s submission re-armed compute, and the next "
                "one waited %lld ms past the 100 ms limit for the compute create\n",
                static_cast<long long>(entered_at - (released_at.load() - 400)));
    return 0;
}

// A create that stays in the "driver" until `release` is set; `in` is set once it is there, `left_at` when it leaves.
struct HeldCreate {
    std::atomic<bool> in{false}, release{false};
    std::atomic<std::int64_t> left_at{0};
    std::thread thread;
    HeldCreate(SubmitMutex& m, const SubmitMutex::CreateInfo& info) {
        thread = std::thread([&m, info, this] {
            const SubmitMutex::CompileTurn turn(m, info);
            in = true;
            while (!release) std::this_thread::sleep_for(1ms);
            left_at = now_ms();
        });
    }
    void let_go() {
        release = true;
        if (thread.joinable()) thread.join();
    }
    ~HeldCreate() { let_go(); }
};

// The cases raised in review of #2412: a later create leaving does not hide an earlier one, presents and waits do
// not probe, and a kind that holds submissions is waited for up to the held limit only.
int gate_review_cases() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): review cases not checked\n");
        return 0;
    }
    using Verdict = SubmitMutex::GateVerdict;
    using Kind = SubmitMutex::CreateKind;
    const SubmitMutex::CreateInfo compute{Kind::compute, "test compute", "test", 1000};
    const auto turn_compute_off = [&](SubmitMutex& m, HeldCreate& c) {
        { const SubmitMutex::SubmitTurn turn(m); }
        std::this_thread::sleep_for(1100ms);
        { const SubmitMutex::SubmitTurn turn(m); }
        return m.gate_verdict(Kind::compute) == Verdict::driver_does_not_hold && c.in.load();
    };
    // A, there when the submission entered, leaves; then B, which entered after it, leaves; then the submission comes
    // back. A's leave still counts: re-armed.
    for (const bool a_first : {true, false}) {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);
        HeldCreate a(m, compute);
        const bool off = eventually([&] { return a.in.load(); }) && turn_compute_off(m, a);
        std::unique_ptr<HeldCreate> b;
        {
            const SubmitMutex::SubmitTurn turn(m);
            b = std::make_unique<HeldCreate>(m, compute);  // enters after the submission did
            EXPECT(eventually([&] { return b->in.load(); }));
            std::this_thread::sleep_for(2000ms);
            if (a_first) {
                a.let_go();
                std::this_thread::sleep_for(30ms);
                b->let_go();
            } else {
                b->let_go();
                std::this_thread::sleep_for(30ms);
                a.let_go();
            }
            std::this_thread::sleep_for(50ms);
        }
        EXPECT(off);
        EXPECT(m.gate_verdict(Kind::compute) == Verdict::driver_holds);
    }
    std::printf("compile gate: A then B and B then A leaving just before a 2.1 s submission both re-arm compute\n");
    // B alone leaves (it entered after the submission), A is still there: not held.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);
        HeldCreate a(m, compute);
        const bool off = eventually([&] { return a.in.load(); }) && turn_compute_off(m, a);
        {
            const SubmitMutex::SubmitTurn turn(m);
            HeldCreate b(m, compute);
            EXPECT(eventually([&] { return b.in.load(); }));
            std::this_thread::sleep_for(2000ms);
            b.let_go();
            std::this_thread::sleep_for(50ms);
        }
        EXPECT(off);
        EXPECT(m.gate_verdict(Kind::compute) == Verdict::driver_does_not_hold);
    }
    std::printf("compile gate: a create that entered after the submission leaving does not re-arm compute\n");
    // A present or wait (probe false) that comes back 2.1 s later, just after the create left, judges nothing.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);
        HeldCreate a(m, compute);
        EXPECT(eventually([&] { return a.in.load(); }));
        {
            const SubmitMutex::SubmitTurn turn(m, false);
            std::this_thread::sleep_for(2000ms);
            a.let_go();
            std::this_thread::sleep_for(50ms);
        }
        EXPECT(m.gate_verdict(Kind::compute) == Verdict::pending);
    }
    std::printf("compile gate: a 2.1 s present or wait judges nothing\n");
    // A kind that holds is waited for up to the held limit, then gone past; the next submission does not wait for it.
    {
        SubmitMutex m;
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_wait_limit_ms(100);
        m.set_gate_probe_budget_ms(0);
        m.set_gate_held_limit_ms(400);
        {
            HeldCreate first(m, compute);
            EXPECT(eventually([&] { return first.in.load(); }));
            const SubmitMutex::SubmitTurn turn(m);
            std::this_thread::sleep_for(2000ms);
            first.let_go();
            std::this_thread::sleep_for(50ms);
        }
        const Verdict held = m.gate_verdict(Kind::compute);
        HeldCreate stays(m, compute);
        EXPECT(eventually([&] { return stays.in.load(); }));
        std::int64_t waited = 0, next_waited = 0;
        bool ahead = false;
        {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m);
            waited = now_ms() - start;
            ahead = turn.went_ahead();
        }
        {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m);
            next_waited = now_ms() - start;
        }
        EXPECT(held == Verdict::driver_holds);
        EXPECT(ahead && waited >= 390 && waited < 2000);
        EXPECT(next_waited < 50);
        std::printf("compile gate: with a holding kind a submission waited %lld ms (held limit 400 ms), the next %lld ms\n",
                    static_cast<long long>(waited), static_cast<long long>(next_waited));
    }
    return 0;
}

// The second review of #2412: whether a create that holds is in the driver is checked on every pass of the wait, the
// held limit applies on its own, the notice comes once per wait, and a present or wait never uses the probe budget.
int gate_second_review() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): second review cases not checked\n");
        return 0;
    }
    using Verdict = SubmitMutex::GateVerdict;
    using Kind = SubmitMutex::CreateKind;
    const SubmitMutex::CreateInfo compute{Kind::compute, "test compute", "test", 1000};
    const SubmitMutex::CreateInfo graphics{Kind::graphics, "test mesh", "test", 2000};
    const auto adaptive = [](SubmitMutex& m) {
        m.set_gate_adaptive(true);
        m.set_gate_graphics(true);  // as LEONIDA_COMPILE_GATE=adaptive
        m.set_gate_probe_budget_ms(0);
    };
    // Compute gets the held verdict: a submission goes ahead at a 100 ms limit, stays 2 s, and comes back just after
    // the create leaves.
    const auto compute_holds = [&](SubmitMutex& m) {
        m.set_gate_wait_limit_ms(100);
        HeldCreate first(m, compute);
        if (!eventually([&] { return first.in.load(); })) return false;
        {
            const SubmitMutex::SubmitTurn turn(m);
            std::this_thread::sleep_for(2000ms);
            first.let_go();
            std::this_thread::sleep_for(50ms);
        }
        return m.gate_verdict(Kind::compute) == Verdict::driver_holds;
    };
    // (a) The holding compute create leaves while a graphics create (no verdict) stays: the submission goes ahead at
    // the plain limit, even with no held limit.
    {
        SubmitMutex m;
        adaptive(m);
        const bool holds = compute_holds(m);
        m.set_gate_wait_limit_ms(300);
        m.set_gate_held_limit_ms(0);
        HeldCreate c(m, compute);
        HeldCreate g(m, graphics);
        EXPECT(eventually([&] { return c.in.load() && g.in.load(); }));
        std::atomic<std::int64_t> waited{-1};
        std::atomic<bool> ahead{false};
        std::thread submit([&] {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m);
            waited = now_ms() - start;
            ahead = turn.went_ahead();
        });
        std::this_thread::sleep_for(150ms);
        c.let_go();
        const bool went = eventually([&] { return waited.load() >= 0; });  // within 2 s, the graphics one still there
        g.let_go();
        submit.join();
        EXPECT(holds);
        EXPECT(went && ahead.load() && waited.load() >= 290 && waited.load() < 1000);
        std::printf("compile gate: the holding create left, the other kind's stayed: went ahead after %lld ms "
                    "(limit 300 ms, no held limit)\n", static_cast<long long>(waited.load()));
    }
    // (b) and (c): a held limit under the plain limit applies on its own, and one over it gives one notice per wait.
    for (const bool shorter : {true, false}) {
        SubmitMutex m;
        adaptive(m);
        const bool holds = compute_holds(m);
        m.set_gate_wait_limit_ms(shorter ? 2000 : 100);
        m.set_gate_held_limit_ms(shorter ? 300 : 400);
        HeldCreate c(m, compute);
        EXPECT(eventually([&] { return c.in.load(); }));
        const unsigned notices = m.gate_held_notices();
        std::int64_t waited = 0;
        bool ahead = false;
        {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m);
            waited = now_ms() - start;
            ahead = turn.went_ahead();
        }
        const unsigned noted = m.gate_held_notices() - notices;
        c.let_go();
        EXPECT(holds && ahead);
        if (shorter) {
            EXPECT(waited >= 290 && waited < 1000 && noted == 0);
        } else {
            EXPECT(waited >= 390 && waited < 1000 && noted == 1);
        }
        std::printf("compile gate: held limit %s the plain limit: went ahead after %lld ms, %u notice(s)\n",
                    shorter ? "300 ms, under" : "400 ms, over", static_cast<long long>(waited), noted);
    }
    // (d) A present or wait beside a create with no verdict never goes ahead at the probe budget, only at the limit.
    {
        SubmitMutex m;
        adaptive(m);
        m.set_gate_wait_limit_ms(1000);
        m.set_gate_probe_budget_ms(200);
        HeldCreate c(m, compute);
        EXPECT(eventually([&] { return c.in.load(); }));
        std::int64_t waited = 0;
        bool ahead = false;
        {
            const std::int64_t start = now_ms();
            const SubmitMutex::SubmitTurn turn(m, false);
            waited = now_ms() - start;
            ahead = turn.went_ahead();
        }
        c.let_go();
        EXPECT(ahead && waited >= 990 && waited < 2000);
        EXPECT(m.gate_verdict(Kind::compute) == Verdict::pending);
        std::printf("compile gate: a present beside an unverified create waited %lld ms (budget 200 ms, limit 1000 "
                    "ms)\n", static_cast<long long>(waited));
    }
    return 0;
}

// The classic gate (LEONIDA_COMPILE_GATE unset, the default): compute creates are waited for however long they take,
// graphics creates do not pass the gate, nothing probes.
int gate_classic() {
    if (!SubmitMutex::compile_gate_enabled()) {
        std::printf("compile gate off (LEONIDA_COMPILE_GATE=0): classic gate not checked\n");
        return 0;
    }
    using Kind = SubmitMutex::CreateKind;
    using Verdict = SubmitMutex::GateVerdict;
    const SubmitMutex::GateMode mode = SubmitMutex::compile_gate_mode();
    if (mode == SubmitMutex::GateMode::classic) {
        EXPECT(!SubmitMutex::compile_gate_graphics());
        if (!std::getenv("LEONIDA_COMPILE_GATE_PROBE_MS") && !std::getenv("LEONIDA_COMPILE_GATE_WAIT_MS"))
            EXPECT(SubmitMutex::default_gate_wait_limit_ms() == 0 && SubmitMutex::default_gate_probe_budget_ms() == 0);
    }
    SubmitMutex m;
    m.set_gate_adaptive(false);
    m.set_gate_graphics(false);
    m.set_gate_wait_limit_ms(0);
    m.set_gate_probe_budget_ms(0);
    // A graphics create in the driver holds no submission back.
    std::atomic<bool> g_in{false}, g_release{false};
    std::thread g([&] {
        const SubmitMutex::CompileTurn turn(m, SubmitMutex::CreateInfo{Kind::graphics, "test mesh", "test", 2000});
        g_in = true;
        while (!g_release) std::this_thread::sleep_for(1ms);
    });
    const bool g_entered = eventually([&] { return g_in.load(); });
    std::int64_t beside_graphics = 0;
    {
        const std::int64_t start = now_ms();
        const SubmitMutex::SubmitTurn turn(m);
        beside_graphics = now_ms() - start;
    }
    g_release = true;
    g.join();
    // A compute create is waited for until it leaves, well past the adaptive gate's 2.5 s budget, and decides nothing.
    std::atomic<bool> c_in{false};
    std::atomic<std::int64_t> c_left_at{0};
    std::thread c([&] {
        const SubmitMutex::CompileTurn turn(m, SubmitMutex::CreateInfo{Kind::compute, "test compute", "test", 1000});
        c_in = true;
        std::this_thread::sleep_for(3000ms);
        c_left_at = now_ms();
    });
    const bool c_entered = eventually([&] { return c_in.load(); });
    std::int64_t entered_at = 0;
    bool ahead = true;
    {
        const SubmitMutex::SubmitTurn turn(m);
        entered_at = now_ms();
        ahead = turn.went_ahead();
    }
    c.join();
    EXPECT(g_entered && c_entered);
    EXPECT(beside_graphics < 50);
    EXPECT(entered_at >= c_left_at.load() && !ahead);
    EXPECT(m.gate_verdict(Kind::compute) == Verdict::pending && m.gate_verdict(Kind::graphics) == Verdict::pending);
    std::printf("classic gate: %lld ms beside a graphics create; a submission waited out a 3 s compute create\n",
                static_cast<long long>(beside_graphics));
    return 0;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (owner_tracking() || waits_for_holder() || waits_for_queued_submitter() || handoff_is_capped() ||
        compile_gate() || gate_wait_limit() || gate_adapts() ||
        gate_probe_budget() || gate_per_kind() || gate_review_cases() || gate_second_review() ||
        gate_classic())
        return 1;
    std::printf("submit_mutex_test: all passed\n");
    return 0;
}
