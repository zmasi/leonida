#pragma once
// Which command lists recorded before a moment may still be replayed. A command list resolves the title addresses
// its root views name when they are recorded (d3d12_com.cpp GraphicsCommandList::address), so a copy the layer
// retires (a title window, DeviceState::retire_orphan_windows) may still be named by a list recorded before the
// retirement and executed, or executed again, after it; a held batch and the async worker keep copies of the lists
// too. Each list, and each copy of it, carries the epoch its recording began in (Stamp, a CommandList member):
// whatever was retired at epoch E is named by no list once the oldest live stamp is E or later.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>

namespace leonida::command_list_epochs {

struct Registry {
    std::mutex mutex;
    std::multiset<std::uint64_t> live;  // the stamps of every list and copy alive
    std::atomic<std::uint64_t> epoch{1};
};
inline Registry& registry() {
    static Registry r;
    return r;
}

// A new epoch: lists whose recording began before it carry a smaller stamp.
inline std::uint64_t advance() { return registry().epoch.fetch_add(1, std::memory_order_acq_rel) + 1; }

// The oldest stamp alive (UINT64_MAX with no list at all).
inline std::uint64_t oldest() {
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    return r.live.empty() ? UINT64_MAX : *r.live.begin();
}

class Stamp {
public:
    Stamp() : epoch_(enter(registry().epoch.load(std::memory_order_acquire))) {}
    Stamp(const Stamp& other) : epoch_(enter(other.epoch_)) {}
    Stamp& operator=(const Stamp& other) {
        if (this != &other) {
            leave(epoch_);
            epoch_ = enter(other.epoch_);
        }
        return *this;
    }
    ~Stamp() { leave(epoch_); }
    // A new recording begins (CommandList::reset).
    void restamp() {
        leave(epoch_);
        epoch_ = enter(registry().epoch.load(std::memory_order_acquire));
    }
    std::uint64_t epoch() const { return epoch_; }

private:
    static std::uint64_t enter(std::uint64_t epoch) {
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        r.live.insert(epoch);
        return epoch;
    }
    static void leave(std::uint64_t epoch) {
        Registry& r = registry();
        std::lock_guard lock(r.mutex);
        if (const auto it = r.live.find(epoch); it != r.live.end()) r.live.erase(it);
    }
    std::uint64_t epoch_;
};

}  // namespace leonida::command_list_epochs
