#pragma once

#include "d3d12_title_buffer_windows.h"
#include "../src/memory_breakdown.h"
#include "../src/title_geometry_watch.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rt::d3d12 {

// One persistent copy for each geometry extent, in device-local memory (or system memory: LEONIDA_GEOMETRY_HOST). The console heap retains its original indices.
// Every CPU mapping of a source tile is watched; a physical remap changes which aliases must be watched.
// Views whose title range is released are retired immediately and their backing storage is reclaimed once
// in-flight GPU submissions up to their retire ticket have completed; previously cap-refused ranges retry
// once budget has been freed.
enum class GeometryCopy { copied, unreadable, interrupted };

class GeometryViews {
    using Address = std::uintptr_t;
    using Key = std::pair<Address, std::size_t>;
    static constexpr auto tile_size = leonida::geometry_writes::tile_size;
    using Cell = leonida::geometry_writes::Cell;
    struct NoopRetire {
        void operator()(Address, std::size_t, VkDeviceSize) const noexcept {}
    };
    struct NoCopy {};
    struct View {
        Address source{};
        std::size_t size{};
        std::uint8_t* dst{};
        VkDeviceSize va{};
        VkDeviceAddress bda{};
        bool ready{}, zeroed{}, alive{};
    };
    struct Tile {
        Cell* primary{};
        std::uint64_t mapping{}, version{};
        std::vector<std::pair<Address, Cell*>> peers;
        std::vector<std::size_t> views;
        bool retry = false;
    };
    struct FastEntry {
        VkDeviceSize va{};
        std::uint64_t refused_at{};  // free_generation_ when refused (only meaningful when va == 0)
    };
    struct Retired {
        VkDeviceSize va{};
        std::size_t size{};
        std::uint64_t ticket{};  // kAfterExecutes until execute_end() stamps it
        std::uint64_t epoch{};   // the last execute begun when it was retired (kAfterExecutes only)
    };
    std::uint64_t cap_override_{};
    mutable std::mutex mutex_;
    std::mutex refresh_mutex_;  // refresh() only, held for its whole run, before mutex_
    TitleBufferWindows pool_;
    std::map<Key, std::size_t> keys_;  // live views only
    // What resolve() and find() answer for a key already seen (va == 0 when refused), readable without mutex_:
    // refresh() holds mutex_ for its whole pass over the watched tiles (up to tens of milliseconds a submission),
    // and the title's Subrender workers resolve their root views while they record (SetGraphicsRoot*View), so they
    // waited for it (speed-26). Written under mutex_ and then fast_mutex_ (in that order), read under fast_mutex_
    // alone. A refused entry records free_generation_ at refusal so resolve() short-circuits while nothing has been
    // freed and retries automatically after reap() returns budget.
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            return std::hash<std::uint64_t>{}(std::uint64_t(k.first) * 0x9E3779B97F4A7C15ull ^ k.second);
        }
    };
    std::map<Key, std::uint64_t> refused_;  // key -> free_generation_ at refusal (under mutex_)
    std::size_t max_view_bytes_{};          // largest size of any view ever created (under mutex_)
    std::size_t max_refused_bytes_{};       // largest size of any refused key (under mutex_)
    // keys_, views_ (the vector itself, and each live view's source, size, dst, va, bda and alive) and by_address_
    // change only under mutex_ and then structure_mutex_ exclusively. lookup() and find() read them under
    // structure_mutex_ shared, so they do not wait for a refresh() in progress (which holds mutex_ and changes only a
    // view's ready/zeroed flags and its bytes), on the submit thread's heaps refresh among others.
    mutable std::shared_mutex structure_mutex_;
    mutable std::shared_mutex fast_mutex_;
    std::unordered_map<Key, FastEntry, KeyHash> fast_;
    void publish(const Key& key, VkDeviceSize va, std::uint64_t refused_at = 0) {  // mutex_ held
        std::unique_lock lock(fast_mutex_);
        fast_[key] = {va, refused_at};
    }
    bool fast_find(const Key& key, VkDeviceSize& va) const {
        std::shared_lock lock(fast_mutex_);
        const auto it = fast_.find(key);
        if (it == fast_.end()) return false;
        va = it->second.va;
        return true;
    }
    bool fast_find_resolve(const Key& key, VkDeviceSize& va) const {
        std::shared_lock lock(fast_mutex_);
        const auto it = fast_.find(key);
        if (it == fast_.end()) return false;
        if (it->second.va == 0 && it->second.refused_at != free_generation_.load(std::memory_order_acquire))
            return false;
        va = it->second.va;
        return true;
    }
    std::vector<View> views_;
    std::vector<std::size_t> free_ids_;
    std::size_t live_views_{};
    bool refreshing_{};
    std::map<VkDeviceAddress, std::size_t> by_address_;
    std::map<Address, Tile> tiles_;
    std::vector<Retired> retired_;
    std::atomic<std::size_t> retired_count_{0};
    // The ExecuteCommandLists replays in progress (execute_begin/execute_end), by the serial each began with. A view
    // retired at kAfterExecutes may be named by any of them: by a list recorded before the retirement, or by a console
    // heap slot their before_execute_ translated. It gets its retire ticket only once all of them have recorded their
    // submissions, so a ticket read then covers them, however many tickets a replay records and on whichever queue.
    std::mutex execute_mutex_;  // a leaf lock
    std::uint64_t execute_serial_{};
    std::vector<std::uint64_t> executing_;
    std::atomic<bool> unstamped_{false};
    leonida::geometry_writes::ReleaseQueue releases_;
    std::atomic<std::uint64_t> free_generation_{0};
    std::uint64_t serial_{}, bytes_{}, cap_refusals_{}, allocation_failures_{}, ready_{}, uploads_{}, uploaded_{};
    std::uint64_t submissions_{}, dirty_refreshes_{}, refilled_{}, tile_fills_{};
    std::uint64_t evicted_{}, reaped_{}, retries_succeeded_{}, final_copies_{};
    std::unordered_map<Address, std::size_t> tile_reads_;  // refresh(): offset in tile_scratch_, SIZE_MAX unreadable
    // refresh() scratch: per view, whether it waits on a tile (pinned), is filled whole, was touched, was counted;
    // and the tiles found written, in address order.
    enum : std::uint8_t { kPinned = 1, kWhole = 2, kTouched = 4, kCounted = 8 };
    std::vector<std::uint8_t> marks_;
    std::vector<Address> written_vas_;
    std::vector<std::uint8_t> tile_scratch_;
    std::chrono::steady_clock::duration refresh_time_{};
    struct Window {  // since the last periodic line
        std::uint64_t refreshes = 0, tiles = 0, views = 0, bytes = 0;
        std::chrono::steady_clock::duration time{};
        std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
    } window_;
    std::uint64_t cleared_{};
    bool retry_{};

    void detach_tiles_locked(std::size_t id, Address source, std::size_t size) {  // mutex_ held
        for (auto p = source & ~(tile_size - 1); p < source + size; p += tile_size) {
            const auto it = tiles_.find(p);
            if (it == tiles_.end()) continue;
            std::erase(it->second.views, id);
            if (it->second.views.empty()) {
                for (auto [peer, cell] : it->second.peers) leonida::geometry_writes::release(peer);
                if (it->second.primary) leonida::geometry_writes::release(p);
                tiles_.erase(it);
            }
        }
    }

    bool update(Address va, Tile& t) {
        namespace watch = leonida::geometry_writes;
        if (!t.primary) t.primary = watch::acquire(va);
        auto version = t.primary->version.load(std::memory_order_acquire);
        const auto mapping = t.primary->mapping.load(std::memory_order_acquire);
        if (t.mapping != mapping) {
            std::vector<Address> others;
            if (auto aliases = watch::aliases.load(std::memory_order_acquire)) aliases(va, others);
            auto old = std::move(t.peers);
            for (auto peer : others) t.peers.emplace_back(peer, watch::acquire(peer));
            for (auto [peer, cell] : old) watch::release(peer);
            t.mapping = mapping;
        }
        for (auto [addr, peer] : t.peers)
            version = (std::max)(version, peer->version.load(std::memory_order_acquire));
        if (version == t.version && !t.retry) return false;
        t.retry = !watch::arm(va, *t.primary);
        for (auto [addr, peer] : t.peers)
            if (!watch::arm(addr, *peer)) t.retry = true;
        t.version = version;
        retry_ |= t.retry;
        return !t.retry;
    }
    // The tile's 64 KiB as this refresh read it, or null when it could not be read whole. Read at most once a refresh.
    template<class Copy>
    const std::uint8_t* read_tile(Address tile, Copy& copy) {
        auto [it, fresh] = tile_reads_.try_emplace(tile, SIZE_MAX);
        if (fresh) {
            const std::size_t at = tile_scratch_.size();
            tile_scratch_.resize(at + tile_size);
            if (copy(tile_scratch_.data() + at, reinterpret_cast<const std::uint8_t*>(tile), tile_size) ==
                GeometryCopy::copied)
                it->second = at;
            else
                tile_scratch_.resize(at);
        }
        return it->second == SIZE_MAX ? nullptr : tile_scratch_.data() + it->second;
    }
    template<class Copy>
    void fill(View& v, Copy& copy) {
        const auto result = copy(v.dst, reinterpret_cast<const std::uint8_t*>(v.source), v.size);
        if (result == GeometryCopy::copied) {
            if (!v.ready) ++ready_;
            v.ready = true;
            v.zeroed = false;
            ++uploads_;
            uploaded_ += v.size;
        } else {
            if (v.ready) --ready_;
            v.ready = false;
            if (!v.zeroed || result == GeometryCopy::interrupted) {
                // A failing guarded copy may have written a prefix. A failed readability probe
                // wrote nothing, so an already-zeroed view needs no second write across the BAR.
                std::memset(v.dst, 0, v.size);
                cleared_ += v.size;
                v.zeroed = true;
            }
        }
    }

public:
    explicit GeometryViews(std::uint64_t cap_override = 0) : cap_override_(cap_override) {
        pool_.set_system_memory(system_memory_setting());
        leonida::geometry_writes::subscribe(releases_);
    }
    ~GeometryViews() { leonida::geometry_writes::unsubscribe(releases_); }
    GeometryViews(const GeometryViews&) = delete;
    GeometryViews& operator=(const GeometryViews&) = delete;
    GeometryViews(GeometryViews&&) = delete;
    GeometryViews& operator=(GeometryViews&&) = delete;

    static std::uint64_t cap() {
        static const std::uint64_t value = []() -> std::uint64_t {
            const char* opt = std::getenv("LEONIDA_GEOMETRY_MB");
            return opt ? (std::min)(std::strtoull(opt, nullptr, 10), 8192ull) << 20 : 0ull;
        }();
        return value;
    }

    std::uint64_t effective_cap() const { return cap_override_ ? cap_override_ : cap(); }

    // LEONIDA_GEOMETRY_HOST: where the copies live. Unset or 0: device-local memory (the BAR) while the driver gives it;
    // 1: system memory, write-back (cached) where the device offers it; 2: system memory, write-combined. The GPU then
    // reads the copies over the bus (VRAM saved, each read of a vertex or index slower).
    static int system_memory_setting() {
        const char* v = std::getenv("LEONIDA_GEOMETRY_HOST");
        if (!v || !*v || *v == '0') return TitleBufferWindows::kDeviceLocal;
        return *v == '2' ? TitleBufferWindows::kSystemUncached : TitleBufferWindows::kSystemCached;
    }
    int system_memory() const { return pool_.system_memory(); }

    bool has_released() const { return leonida::geometry_writes::has_released(releases_); }
    bool has_retired() const { return retired_count_.load(std::memory_order_acquire) != 0; }
    bool has_pending_reclaim() const { return has_released() || has_retired(); }
    leonida::geometry_writes::ReleaseQueue& release_queue() { return releases_; }

    std::uint64_t bytes() const {
        std::lock_guard lock(mutex_);
        return bytes_;
    }
    std::uint64_t reserved() const {
        std::lock_guard lock(mutex_);
        return pool_.reserved();
    }
    std::size_t live_views() const {
        std::lock_guard lock(mutex_);
        return live_views_;
    }
    std::size_t retired_count() const { return retired_count_.load(std::memory_order_acquire); }
    std::uint64_t free_generation() const { return free_generation_.load(std::memory_order_acquire); }
    std::uint64_t cap_refusals() const {
        std::lock_guard lock(mutex_);
        return cap_refusals_;
    }
    std::uint64_t evicted() const {
        std::lock_guard lock(mutex_);
        return evicted_;
    }
    std::uint64_t final_copies() const {
        std::lock_guard lock(mutex_);
        return final_copies_;
    }
    std::uint64_t reaped() const {
        std::lock_guard lock(mutex_);
        return reaped_;
    }
    std::size_t block_count() const {
        std::lock_guard lock(mutex_);
        return pool_.block_count();
    }
    std::size_t empty_blocks() const {
        std::lock_guard lock(mutex_);
        return pool_.empty_blocks();
    }

    std::uint64_t find(Address cpu, std::size_t bytes) const {
        if (VkDeviceSize va = 0; fast_find({cpu, bytes}, va)) return va;
        std::shared_lock lock(structure_mutex_);
        const auto it = keys_.find({cpu, bytes});
        return it == keys_.end() || !views_[it->second].alive ? 0 : views_[it->second].va;
    }

    template<class Device, class Copy>
    std::uint64_t resolve(Device& device, Address cpu, std::size_t bytes, Copy copy) {
        if (!bytes || bytes > UINTPTR_MAX - cpu) return 0;
        const std::uint64_t limit = effective_cap();
        if (!limit) return 0;
        const Key key = std::make_pair(cpu, bytes);
        if (VkDeviceSize known = 0; fast_find_resolve(key, known)) return known;
        std::lock_guard lock(mutex_);
        if (const auto it = keys_.find(key); it != keys_.end()) {
            const VkDeviceSize known = views_[it->second].va;
            publish(key, known, 0);
            return known;
        }
        const std::uint64_t gen = free_generation_.load(std::memory_order_acquire);
        const auto refused_it = refused_.find(key);
        if (refused_it != refused_.end() && refused_it->second == gen) {
            publish(key, 0, gen);
            return 0;
        }
        std::uint8_t* mapped{};
        VkDeviceSize va{};
        bool at_cap = false;
        const bool over_bytes = bytes_ >= limit || bytes > limit - bytes_;
        leonida::memory_breakdown::Owner owner(leonida::memory_breakdown::Consumer::geometry);
        if (over_bytes || !pool_.allocate(device, bytes, va, mapped, true, limit, &at_cap)) {
            if (at_cap || over_bytes) {
                refused_[key] = gen;
                if (bytes > max_refused_bytes_) max_refused_bytes_ = bytes;
                publish(key, 0, gen);
                if (++cap_refusals_ <= 4)
                    std::fprintf(stderr, "[rt] geometry buffer cap refused %zu bytes, %llu bytes already reserved\n",
                                 bytes, static_cast<unsigned long long>(pool_.reserved()));
            } else {
                ++allocation_failures_;
            }
            return 0;
        }
        if (refused_it != refused_.end()) {
            refused_.erase(refused_it);
            ++retries_succeeded_;
        }
        const VkDeviceAddress bda = device.arena.address + va;
        std::size_t id{};
        {
            std::unique_lock structure(structure_mutex_);
            if (!refreshing_ && !free_ids_.empty()) {
                id = free_ids_.back();
                free_ids_.pop_back();
                views_[id] = View{cpu, bytes, mapped, va, bda, false, false, true};
            } else {
                id = views_.size();
                views_.push_back(View{cpu, bytes, mapped, va, bda, false, false, true});
            }
            keys_[key] = id;
            by_address_.emplace(bda, id);
        }
        if (bytes > max_view_bytes_) max_view_bytes_ = bytes;
        ++live_views_;
        bytes_ += bytes;
        bool ready = true;
        for (auto p = cpu & ~(tile_size - 1); p < cpu + bytes; p += tile_size) {
            auto& tile = tiles_[p];
            // Previously watched tiles may have other dirty views waiting for refresh().
            if (!tile.primary) update(p, tile);
            tile.views.push_back(id);
            ready &= !tile.retry;
        }
        if (ready) fill(views_[id], copy);
        else {
            std::memset(mapped, 0, bytes);
            cleared_ += bytes;
            views_[id].zeroed = true;
        }
        publish(key, va, 0);
        return va;
    }

    // retire_range's ticket for a view that work already in flight may name: the ticket is taken by execute_end()
    // once every ExecuteCommandLists replay begun by then has recorded its submissions.
    static constexpr std::uint64_t kAfterExecutes = UINT64_MAX;

    // An ExecuteCommandLists replay starts (before its before_execute_ work): its serial, for execute_end().
    std::uint64_t execute_begin() {
        std::lock_guard lock(execute_mutex_);
        executing_.push_back(++execute_serial_);
        return execute_serial_;
    }
    // The last replay begun, and the oldest still in progress (kAfterExecutes when none): what execute_end stamps by,
    // for a copy the device retires the same way outside these views (a title window, DeviceState::retire_windows).
    std::uint64_t execute_epoch() {
        std::lock_guard lock(execute_mutex_);
        return execute_serial_;
    }
    std::uint64_t oldest_execute() {
        std::lock_guard lock(execute_mutex_);
        std::uint64_t oldest = kAfterExecutes;
        for (const auto s : executing_) oldest = (std::min)(oldest, s);
        return oldest;
    }
    // The replay that began with `serial` has recorded its submissions. Views retired while only replays now finished
    // were in progress get submitted() (the context's submitted_retire_ticket(), read here, after those replays
    // recorded theirs) as their retire ticket. Lock order: mutex_ -> execute_mutex_, as in retire_range.
    template<class Submitted>
    void execute_end(std::uint64_t serial, Submitted&& submitted) {
        {
            std::lock_guard lock(execute_mutex_);
            std::erase(executing_, serial);
        }
        if (!unstamped_.load(std::memory_order_acquire)) return;
        std::lock_guard lock(mutex_);
        std::uint64_t oldest = kAfterExecutes;
        {
            std::lock_guard execute(execute_mutex_);
            for (const auto s : executing_) oldest = (std::min)(oldest, s);
        }
        const std::uint64_t ticket = submitted();
        bool left = false;
        for (auto& r : retired_) {
            if (r.ticket != kAfterExecutes) continue;
            if (r.epoch < oldest) r.ticket = ticket;
            else left = true;
        }
        unstamped_.store(left, std::memory_order_release);
    }

    // Retires every live view overlapping [begin, end), unlinking it from lookup/refresh/watch immediately and
    // deferring its backing storage reclamation until GPU submissions through `retire_ticket` have completed
    // (kAfterExecutes: through the submissions of every replay in progress now, see execute_end).
    // With `final_copy` (a refresh's copy), a ready view whose title range is readable now is copied from it once more
    // before it leaves the refresh: a released range that is still readable (a heap block freed in memory that stays
    // committed) may hold new bytes, and a list recorded before the release that runs in a submission the view waits
    // for reads those, as it would have read them in the console's memory. Decommitted or unmapped memory is not
    // readable, so such a view keeps what it held.
    template<class OnRetire = NoopRetire, class Copy = NoCopy>
    std::size_t retire_range(Address begin, Address end, std::uint64_t retire_ticket = 0,
                             OnRetire&& on_retire = {}, Copy&& final_copy = {}) {
        if (end <= begin) return 0;
        struct CallbackItem {
            Address source;
            std::size_t size;
            VkDeviceSize va;
        };
        std::vector<CallbackItem> callbacks;
        {
            std::lock_guard lock(mutex_);
            std::vector<std::size_t> ids;
            const Key key_start{begin > max_view_bytes_ ? begin - max_view_bytes_ : Address(0), 0};
            for (auto it = keys_.lower_bound(key_start); it != keys_.end(); ++it) {
                const auto& [key, id] = *it;
                if (key.first >= end) break;
                if (begin < key.first + key.second) ids.push_back(id);
            }
            std::vector<Key> stale_refused;
            const Key refused_start{begin > max_refused_bytes_ ? begin - max_refused_bytes_ : Address(0), 0};
            for (auto it = refused_.lower_bound(refused_start); it != refused_.end(); ++it) {
                const auto& key = it->first;
                if (key.first >= end) break;
                if (begin < key.first + key.second) stale_refused.push_back(key);
            }
            if (ids.empty() && stale_refused.empty()) return 0;
            if (retire_ticket == kAfterExecutes && !ids.empty()) unstamped_.store(true, std::memory_order_release);
            callbacks.reserve(ids.size());
            const std::size_t first_retired = retired_.size();
            for (const auto id : ids) {
                View& v = views_[id];
                if constexpr (!std::is_same_v<std::decay_t<Copy>, NoCopy>) {
                    if (v.ready && final_copy(v.dst, reinterpret_cast<const std::uint8_t*>(v.source), v.size) ==
                                       GeometryCopy::copied) {
                        ++final_copies_;
                        uploaded_ += v.size;
                    }
                }
                detach_tiles_locked(id, v.source, v.size);
                if (v.ready) {
                    --ready_;
                    v.ready = false;
                }
                callbacks.push_back({v.source, v.size, v.va});
                retired_.push_back({v.va, v.size, retire_ticket, 0});
            }
            if (!ids.empty()) {
                std::unique_lock structure(structure_mutex_);
                for (const auto id : ids) {
                    View& v = views_[id];
                    keys_.erase({v.source, v.size});
                    by_address_.erase(v.bda);
                    v.alive = false;
                }
            }
            {
                std::unique_lock fast(fast_mutex_);
                for (const auto id : ids) fast_.erase({views_[id].source, views_[id].size});
                for (const auto& k : stale_refused) fast_.erase(k);
            }
            // No resolve() or find() answers these views from here on, so a replay that begins now cannot name them:
            // they wait only for the replays begun so far.
            if (retire_ticket == kAfterExecutes && !ids.empty()) {
                std::uint64_t epoch = 0;
                {
                    std::lock_guard execute(execute_mutex_);
                    epoch = execute_serial_;
                }
                for (std::size_t i = first_retired; i < retired_.size(); ++i) retired_[i].epoch = epoch;
            }
            for (const auto& k : stale_refused) refused_.erase(k);
            for (const auto id : ids) {
                free_ids_.push_back(id);
                --live_views_;
                ++evicted_;
            }
            retired_count_.store(retired_.size(), std::memory_order_release);
        }
        for (const auto& item : callbacks) on_retire(item.source, item.size, item.va);
        return callbacks.size();
    }

    // Frees backing storage for retired views whose GPU retire_ticket <= completed_ticket, returning their budget
    // and bumping free_generation_ so previously refused ranges retry on the next resolve().
    template<class Device>
    std::size_t reap(Device& device, std::uint64_t completed_ticket = UINT64_MAX) {
        if (!has_retired()) return 0;
        std::lock_guard lock(mutex_);
        std::size_t freed = 0;
        std::size_t keep = 0;
        for (std::size_t i = 0; i < retired_.size(); ++i) {
            const Retired& r = retired_[i];
            if (r.ticket != kAfterExecutes && r.ticket <= completed_ticket) {
                pool_.free(device, r.va, r.size);
                bytes_ -= r.size;
                ++reaped_;
                ++freed;
            } else {
                if (keep != i) retired_[keep] = r;
                ++keep;
            }
        }
        retired_.resize(keep);
        retired_count_.store(retired_.size(), std::memory_order_release);
        if (freed) free_generation_.fetch_add(1, std::memory_order_release);
        return freed;
    }

    // Drains released title ranges recorded by xmem (retiring matching views at `retire_ticket`, with `final_copy` as
    // retire_range takes it) and reaps retired views whose GPU work completed by `completed_ticket`.
    template<class Device, class OnRetire = NoopRetire, class Copy = NoCopy>
    void drain_released(Device& device, std::uint64_t retire_ticket, std::uint64_t completed_ticket,
                        OnRetire&& on_retire = {}, Copy&& final_copy = {}) {
        std::vector<std::pair<Address, Address>> ranges;
        leonida::geometry_writes::take_released(releases_, ranges);
        for (const auto& [begin, end] : ranges) {
            retire_range(begin, end, retire_ticket, on_retire, final_copy);
            leonida::title_gpu_views::Range pair{};
            if (end > begin && leonida::title_gpu_views::view_at(begin, 1, pair) &&
                begin >= pair.gpu && begin - pair.gpu < pair.size) {
                const Address offset = begin - pair.gpu;
                const Address span = (std::min)(end - begin, pair.size - offset);
                retire_range(pair.cpu + offset, pair.cpu + offset + span, retire_ticket, on_retire, final_copy);
            }
        }
        reap(device, completed_ticket);
    }

    template<class Copy>
    void refresh(Copy copy) {
        if (!effective_cap()) return;
        // One refresh at a time (its scratch members, and mutex_ is let go during the walk below); taken before mutex_.
        std::lock_guard serial(refresh_mutex_);
        std::unique_lock lock(mutex_);
        ++submissions_;
        auto now = leonida::geometry_writes::state().serial.load(std::memory_order_acquire);
        if (serial_ == now && !retry_) return;
        const auto start = std::chrono::steady_clock::now();
        const auto before = uploaded_;
        serial_ = now;
        retry_ = false;
        refreshing_ = true;
        struct RefreshGuard {
            bool& flag;
            ~RefreshGuard() { flag = false; }
        } guard{refreshing_};
        // The views over each tile written since, with those tiles (in address order, as tiles_ is): a ready view
        // copies only them, 64 KiB each, not the whole view again (LEONIDA_GEOMETRY_WHOLE_VIEWS=1: whole views).
        static const bool whole = [] {
            const char* v = std::getenv("LEONIDA_GEOMETRY_WHOLE_VIEWS");
            return v && *v && *v != '0';
        }();
        // One pass over the tiles: each written tile is read once and copied into every view over it, not view by
        // view (speed-16: up to 63 written tiles under 918 views a refresh, ~58,000 per-view lookups and copies,
        // 30-80 ms). A view over a tile that cannot be guarded yet (retry) is left as it is this time; a view not
        // filled yet is filled whole, once.
        marks_.assign(views_.size(), 0);
        written_vas_.clear();
        // The pass over the watched tiles makes a VirtualQuery and a VirtualProtect for each written one (tens of
        // milliseconds to a few hundred under Wine). resolve() waits for mutex_, and a new V# resolved while a
        // submission fixes its console heap slots then waited for the whole pass (speed-35: 116 ms of fixing a
        // submission with indirect draws on). So the lock is let go every 64 tiles. Walking by key (lower_bound)
        // survives both inserts from resolve() and erasures from retire_range() while unlocked.
        Address cursor = 0;
        bool have_cursor = true;
        std::size_t walked = 0;
        while (have_cursor) {
            auto it = tiles_.lower_bound(cursor);
            if (it == tiles_.end()) break;
            const Address va = it->first;
            Tile& tile = it->second;
            const bool written = update(va, tile);
            if (tile.retry)
                for (auto id : tile.views)
                    if (id < marks_.size() && views_[id].alive) marks_[id] |= kPinned;
            if (written) written_vas_.push_back(va);
            if (va > UINTPTR_MAX - tile_size) have_cursor = false;
            else cursor = va + tile_size;
            if (!(++walked & 63)) {
                lock.unlock();
                lock.lock();
            }
        }
        const std::size_t changed_tiles = written_vas_.size();
        tile_reads_.clear();
        tile_scratch_.clear();
        std::size_t changed_views = 0;
        for (const Address va : written_vas_) {
            const auto tile_it = tiles_.find(va);
            if (tile_it == tiles_.end()) continue;
            const Tile& tile = tile_it->second;
            const std::uint8_t* read = nullptr;
            bool read_done = false;
            for (const auto id : tile.views) {
                if (id >= marks_.size()) continue;  // made by resolve() during the walk above: filled whole there
                View& v = views_[id];
                if (!v.alive) continue;
                std::uint8_t& mark = marks_[id];
                if (mark & (kPinned | kWhole)) continue;
                if (!(mark & kTouched)) {
                    mark |= kTouched;
                    ++changed_views;
                }
                if (whole || !v.ready) {
                    mark |= kWhole;
                    continue;
                }
                if (!read_done) {
                    read = read_tile(va, copy);
                    read_done = true;
                }
                const Address from = (std::max)(va, v.source), to = (std::min)(va + tile_size, v.source + v.size);
                if (from >= to) continue;
                if (read) {
                    std::memcpy(v.dst + (from - v.source), read + (from - va), to - from);
                } else if (copy(v.dst + (from - v.source), reinterpret_cast<const std::uint8_t*>(from), to - from) !=
                           GeometryCopy::copied) {
                    mark |= kWhole;  // filled whole below, which zeroes it if that fails too
                    continue;
                }
                uploaded_ += to - from;
                if (!(mark & kCounted)) {
                    mark |= kCounted;
                    ++uploads_;
                    ++tile_fills_;
                }
            }
        }
        for (std::size_t id = 0; id < marks_.size(); ++id)
            if ((marks_[id] & (kWhole | kPinned)) == kWhole && views_[id].alive) fill(views_[id], copy);
        written_vas_.clear();
        if (uploaded_ != before) ++dirty_refreshes_;
        refilled_ += uploaded_ - before;
        const auto end = std::chrono::steady_clock::now();
        refresh_time_ += end - start;
        // Every 10 s: what the refreshes since the last line did.
        window_.refreshes += 1;
        window_.tiles += changed_tiles;
        window_.views += changed_views;
        window_.bytes += uploaded_ - before;
        window_.time += end - start;
        if (end - window_.since >= std::chrono::seconds(10)) {
            std::fprintf(stderr,
                         "[rt] geometry refresh: %llu refreshes with writes in %.0f s: %.1f written tiles and %.1f views "
                         "each, %.1f KiB copied each, %.3f ms each; %zu tiles watched, %zu views\n",
                         static_cast<unsigned long long>(window_.refreshes),
                         std::chrono::duration<double>(end - window_.since).count(),
                         double(window_.tiles) / double(window_.refreshes),
                         double(window_.views) / double(window_.refreshes),
                         double(window_.bytes) / 1024.0 / double(window_.refreshes),
                         std::chrono::duration<double, std::milli>(window_.time).count() / double(window_.refreshes),
                         tiles_.size(), live_views_);
            window_ = {};
            window_.since = end;
        }
    }

    // The blocks the copies live in (TitleBufferWindows::for_each_block), without the views' locks.
    const TitleBufferWindows& pool() const { return pool_; }

    bool lookup(VkDeviceAddress bda, std::uint64_t& host, std::uint64_t& bytes, std::uint64_t* source = nullptr,
                bool* populated = nullptr) const {
        std::shared_lock lock(structure_mutex_);
        auto it = by_address_.upper_bound(bda);
        if (it == by_address_.begin()) return false;
        --it;
        const auto offset = bda - it->first;
        const auto& v = views_[it->second];
        if (!v.alive || offset >= v.size) return false;
        bytes = v.size - offset;
        host = reinterpret_cast<Address>(v.dst) + offset;
        if (source) *source = v.source + offset;
        if (populated) *populated = v.ready;
        return true;
    }
    void report() const {
        if (!effective_cap()) return;
        std::lock_guard lock(mutex_);
        std::fprintf(stderr, "[rt] geometry buffers: seen %zu, backed %zu (%llu bytes), populated %llu, cap %llu MiB, "
                             "cap refused %llu, retried ok %llu, evicted %llu (reaped %llu, retired %zu, copied at "
                             "retirement %llu), allocation failures %llu, uploads %llu (%llu bytes), watched tiles %zu, "
                             "reserved %llu bytes (%s)\n",
                     keys_.size() + refused_.size(), live_views_, (unsigned long long)bytes_, (unsigned long long)ready_,
                     (unsigned long long)(effective_cap() >> 20), (unsigned long long)cap_refusals_,
                     (unsigned long long)retries_succeeded_, (unsigned long long)evicted_,
                     (unsigned long long)reaped_, retired_.size(), (unsigned long long)final_copies_,
                     (unsigned long long)allocation_failures_,
                     (unsigned long long)uploads_, (unsigned long long)uploaded_, tiles_.size(),
                     static_cast<unsigned long long>(pool_.reserved()),
                     pool_.system_memory() ? "system memory" : "device-local");
        std::fprintf(stderr, "[rt] geometry refresh: %llu/%llu submissions uploaded dirty views, %llu bytes (%llu views by "
                             "their written tiles only); %.3f s checking and refreshing sources, %llu bytes cleared\n",
                     static_cast<unsigned long long>(dirty_refreshes_), static_cast<unsigned long long>(submissions_),
                     static_cast<unsigned long long>(refilled_), static_cast<unsigned long long>(tile_fills_),
                     std::chrono::duration<double>(refresh_time_).count(), static_cast<unsigned long long>(cleared_));
    }
    template<class Device>
    void release(Device& d) {
        std::lock_guard lock(mutex_);
        for (auto& [va, tile] : tiles_) {
            for (auto [peer, cell] : tile.peers) leonida::geometry_writes::release(peer);
            if (tile.primary) leonida::geometry_writes::release(va);
        }
        tiles_.clear();
        {
            std::unique_lock structure(structure_mutex_);
            keys_.clear();
            by_address_.clear();
            views_.clear();
        }
        {
            std::unique_lock fast(fast_mutex_);
            fast_.clear();
        }
        refused_.clear();
        max_view_bytes_ = 0;
        max_refused_bytes_ = 0;
        free_ids_.clear();
        retired_.clear();
        retired_count_.store(0, std::memory_order_release);
        unstamped_.store(false, std::memory_order_release);
        live_views_ = 0;
        bytes_ = 0;
        ready_ = 0;
        pool_.release(d);
    }
};
} // namespace rt::d3d12
