// Moved to runtime/src: the runtime uses it too (large_module_gate.h there).
//
// A worker of vk_pipeline_check used to take the next module and then wait in the gate when it was large and didn't
// fit, so with the budget full the other modules sat untaken behind it and the pool ran below --jobs.
// LargeModuleQueue now puts such a module back and the worker takes the next one.
#pragma once

#include "../src/large_module_gate.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace leonida {

// The order vk_pipeline_check's workers take modules 0..count-1 in. A large module the gate doesn't admit yet is put
// back (admit() returns false) and the worker takes the next module. The oldest module put back goes first as soon
// as the gate admits it, and no later large module passes it, so none waits forever; once every index is out, a
// worker waits for it. Nothing is put back while the gate admits each large module as it comes (one worker, or room
// under the budget), so then the order is 0, 1, 2... as before.
//
// The gate is the one with a fixed budget, and its large modules all get in through this queue: places are given
// only under mu_, so one the gate admits under mu_ is still free when the ticket takes it.
class LargeModuleQueue {
public:
    struct Claim {
        size_t index = 0;
        std::optional<LargeModuleGate::Ticket> ticket;   // a module put back comes back with its place in the gate
    };

    LargeModuleQueue(LargeModuleGate& gate, size_t count) : gate_(gate), count_(count) {}

    // The next module, or none when every one is out. Drop the previous claim first: its ticket counts in the gate.
    std::optional<Claim> take() {
        std::lock_guard<std::mutex> g(mu_);
        if (!waiting_.empty()) {
            const auto [index, estimate] = waiting_.front();
            // Nothing new left: wait for its place, holding mu_ so no other module passes it. The gate frees places
            // without mu_, and a worker that blocks on mu_ holds no ticket.
            if (gate_.admits(estimate) || next_ == count_) {
                Claim claim{index, std::nullopt};
                claim.ticket.emplace(gate_, estimate);
                waiting_.pop_front();
                return claim;
            }
        }
        if (next_ == count_) return std::nullopt;
        return Claim{next_++, std::nullopt};
    }

    // A large module's place in the gate: true when the claim holds one (it came back with it, or the gate admits
    // the module now); false when the module was put back, and the worker drops the claim and takes the next one.
    bool admit(Claim& claim, uint64_t estimate) {
        if (claim.ticket) return true;
        std::lock_guard<std::mutex> g(mu_);
        if (waiting_.empty() && gate_.admits(estimate)) {   // otherwise the module waiting goes first
            claim.ticket.emplace(gate_, estimate);
            return true;
        }
        waiting_.emplace_back(claim.index, estimate);
        return false;
    }

private:
    LargeModuleGate& gate_;
    const size_t count_;
    std::mutex mu_;
    size_t next_ = 0;
    std::deque<std::pair<size_t, uint64_t>> waiting_;   // put back: index and estimate, oldest first
};

}  // namespace leonida
