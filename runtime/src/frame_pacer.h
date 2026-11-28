#pragma once
#include <utility>
#include <memory>

// Frame pacing for the Xbox presentation path (docs/d3d12_vulkan.md section 4.5): what SetFrameIntervalX,
// ScheduleFrameEventX, WaitFrameEventX and GetFrameStatisticsX do, and the token check and flip bookkeeping that
// PresentX needs. No Vulkan and no Windows headers: time comes from an injectable FrameClock, so the tests drive it
// with a fake clock and the real runtime uses a steady high-resolution timer (a host vblank source can replace it).
//
// What is known and what is ours. From the public GDKX samples (section 4.5): the call order, the argument counts, that
// the interval call takes an allowed frame latency (buffer count - 1 in one sample, 1 in another), that the origin
// event is scheduled once and waited on every frame, that the wait yields a token that PresentX carries, and that
// frame events must be registered again after ResumeX. Everything below the call shapes is our own model, chosen to
// be deterministic and to behave like the samples expect; none of it is measured on the console:
//   * An interval is a period in microseconds (the real enum values are unmeasured).
//   * Vblanks happen at epoch + n * period (n = 1, 2, ...), the epoch being the time of SetFrameIntervalX. The
//     first origin after ScheduleFrameEventX is the next vblank. The origin event of vblank n is signalled
//     offset microseconds after it (ScheduleFrameEventX's offset).
//   * A frame is "in flight" from PresentX until the vblank that shows it. Frames show one per vblank, in order.
//     WaitFrameEventX does not return while `latency` frames are in flight. A frame presented while the origin event
//     is not armed is not paced (PresentX does not hold it for a vblank): it shows at once and is never in flight.
//   * At most `latency` frames are in flight. A present into a full queue (several presents with no wait between
//     them) replaces the newest queued frame and takes its vblank, like a mailbox: the replaced frame is never shown,
//     and the older ones keep their vblanks. So the queue, and the next wait, never reach further than `latency`
//     vblanks ahead, however many presents came in between.
//   * A wait that comes late (after the next origin already passed) returns at once with the newest passed origin;
//     the missed origins are skipped, not queued.
//   * Tokens are 1, 2, 3, ... (0 is the null token), one per successful wait, never reused, across suspend/resume.
//   * A frame is due at the vblank after the origin that handed out its token; a PresentX after that vblank is late by
//     the time since it (present_timing), and ImmediateThresholdPercent decides whether that lateness is shown at once
//     (decide_present_sync).
//
// Uncapped (set_uncapped; LEONIDA_PRESENT_UNCAP in the shim): the registration calls keep working and waits keep
// handing out one token each, but a wait never blocks — no vblank grid, no latency throttle — and a presented
// frame counts as shown at once. The frame rate is then the title's own; the error surface (nothing registered,
// suspended) is unchanged.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace leonida {

// HRESULT values as plain numbers, so this header stands alone.
namespace pacer_hr {
inline constexpr std::int32_t ok = 0;
inline constexpr std::int32_t false_ = 1;                                  // S_FALSE
inline constexpr std::int32_t invalid_arg = std::int32_t(0x80070057);      // E_INVALIDARG
inline constexpr std::int32_t invalid_state = std::int32_t(0x8007139F);    // HRESULT_FROM_WIN32(ERROR_INVALID_STATE)
inline constexpr std::int32_t timeout = std::int32_t(0x80070102);          // HRESULT_FROM_WIN32(WAIT_TIMEOUT)
}  // namespace pacer_hr

inline constexpr std::uint64_t no_deadline = std::numeric_limits<std::uint64_t>::max();
inline constexpr std::uint32_t infinite_timeout_ms = 0xFFFFFFFFu;  // INFINITE

class FrameClock {
public:
    virtual ~FrameClock() = default;
    virtual std::uint64_t now_us() = 0;
    // Called with `lock` held on the pacer's mutex. Returns when `cv` is notified, `deadline_us` (in now_us() time;
    // no_deadline for none) has passed, or spuriously; the caller re-checks its condition.
    virtual void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::uint64_t deadline_us) = 0;
    // The pacer gives a function that takes its mutex and notifies; a fake clock calls it when time moves.
    virtual void set_wake(std::function<void()> wake) { (void)wake; }
};

// Real time: std::chrono::steady_clock (QueryPerformanceCounter on Windows).
class SteadyFrameClock final : public FrameClock {
public:
    std::uint64_t now_us() override {
        return std::uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start_).count());
    }
    void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::uint64_t deadline_us) override {
        if (deadline_us == no_deadline) cv.wait(lock);
        else cv.wait_until(lock, start_ + std::chrono::microseconds(deadline_us));
    }

private:
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
};

// Time that only moves when the test says so. A pacer waiting on it blocks until advance_us() is called.
class ManualFrameClock final : public FrameClock {
public:
    std::uint64_t now_us() override { return now_.load(); }
    void wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, std::uint64_t) override { cv.wait(lock); }
    void set_wake(std::function<void()> wake) override {
        std::lock_guard guard(wake_mutex_);
        wake_ = std::move(wake);
    }
    void advance_us(std::uint64_t microseconds) {
        now_ += microseconds;
        // Called under the lock, so a pacer that is being destroyed (set_wake({})) is never woken after it is gone.
        std::lock_guard guard(wake_mutex_);
        if (wake_) wake_();
    }

private:
    std::atomic<std::uint64_t> now_{0};
    std::mutex wake_mutex_;
    std::function<void()> wake_;
};

// Provisional definitions of ours: the real constants and layouts of the console API are unmeasured. Frame event
// kinds (0: origin, 1: vblank, 2: flip) and wait flags.
inline constexpr std::uint32_t frame_event_origin = 0;
inline constexpr std::uint32_t frame_event_vblank = 1;
inline constexpr std::uint32_t frame_event_flip = 2;
inline constexpr std::uint32_t frame_event_count = 3;
// Issue a consecutive frame token even when WaitFrameEventX(origin, ...) times out (returns S_FALSE).
inline constexpr std::uint32_t frame_wait_flag_token_on_timeout = 2;
inline constexpr std::uint32_t wait_flag_current_token = frame_wait_flag_token_on_timeout;
// Statistics types (bit mask). Interval and render entries are never produced here.
inline constexpr std::uint32_t stat_frame_interval = 0x01;
inline constexpr std::uint32_t stat_frame_event = 0x02;
inline constexpr std::uint32_t stat_render = 0x04;
inline constexpr std::uint32_t stat_present = 0x08;
inline constexpr std::uint32_t stat_display = 0x10;

// How a frame that is being presented is put on the display.
enum class PresentSync {
    vblank,     // at the next vblank (FIFO)
    immediate,  // now, mid-scan (a tear)
};

// D3D12XBOX_PRESENT_PARAMETERS::ImmediateThresholdPercent (docs/d3d12_vulkan.md section 4.5): a frame that missed its
// vblank is flipped at once, mid-scan, when the scan has not got further than `threshold_percent` of the screen,
// instead of waiting a whole interval. `late_by_us` is how long after the vblank the frame was ready (0: on time),
// `interval_us` the refresh period; the vblank grid is regular, so the scan is `late_by_us % interval_us` of the way
// down. 0 (or a negative or NaN value) never tears, 100 or more tears wherever the scan is. A frame on time, or an
// interval of 0 (unpaced), is never torn. Pure, so the tests can sweep it.
inline PresentSync decide_present_sync(std::uint64_t late_by_us, std::uint32_t interval_us, float threshold_percent) {
    if (!(threshold_percent > 0.0f) || late_by_us == 0 || interval_us == 0) return PresentSync::vblank;
    const double scanned_percent = 100.0 * double(late_by_us % interval_us) / double(interval_us);
    return scanned_percent <= double(std::min(threshold_percent, 100.0f)) ? PresentSync::immediate
                                                                          : PresentSync::vblank;
}

// Where the scan of a frame that was shown mid-scan stood, in percent (0 for a frame shown at a vblank).
inline float scan_percent(std::uint64_t late_by_us, std::uint32_t interval_us) {
    return interval_us ? float(100.0 * double(late_by_us % interval_us) / double(interval_us)) : 0.0f;
}

struct FrameStatistic {
    std::uint32_t type = 0;          // one stat_* bit
    std::uint32_t plane_index = 0;
    std::uint64_t token = 0;
    std::uint64_t time_us = 0;       // pacer clock
    float percent_scanned = 0.0f;    // display entries: where the scan was when the frame came up (0 at a vblank)
};

// Fixed-capacity ring buffer size for recent frame-time and origin-to-present latency samples (BL-6).
inline constexpr std::size_t frame_pacing_window = 1024;

// Summary over the most recent `frame_pacing_window` presented frames (p50/p95/p99 and mean drift against target).
struct FramePacingSummary {
    std::uint64_t frames = 0;             // total presented frames recorded
    std::uint64_t samples = 0;            // present-to-present intervals in the ring buffer (<= frame_pacing_window)
    std::uint32_t target_interval_us = 0; // active target interval in us (0 when unpaced or uncapped)
    double mean_us = 0.0;                 // mean present-to-present interval over `samples`
    double drift_us = 0.0;                // mean_us - target_interval_us (0 when unpaced/uncapped or samples == 0)
    std::uint64_t p50_us = 0;             // nearest-rank 50th percentile frame interval in us
    std::uint64_t p95_us = 0;             // nearest-rank 95th percentile frame interval in us
    std::uint64_t p99_us = 0;             // nearest-rank 99th percentile frame interval in us
    std::uint64_t latency_samples = 0;    // origin-to-present latency samples in the ring buffer
    std::uint64_t latency_p50_us = 0;     // nearest-rank 50th percentile origin-to-present latency in us
    std::uint64_t latency_p95_us = 0;     // nearest-rank 95th percentile origin-to-present latency in us
    std::uint64_t latency_p99_us = 0;     // nearest-rank 99th percentile origin-to-present latency in us
};

class FrameTokenSource {
public:
    std::uint64_t issue() { return last_.fetch_add(1, std::memory_order_acq_rel) + 1; }
    std::uint64_t issued() const { return last_.load(std::memory_order_acquire); }

private:
    std::atomic<std::uint64_t> last_{0};
};

class FramePacer {
public:
    explicit FramePacer(FrameClock& clock, std::shared_ptr<FrameTokenSource> tokens = nullptr,
                        bool current_origin_poll = false)
        : clock_(clock), tokens_(tokens ? std::move(tokens) : std::make_shared<FrameTokenSource>()),
          current_origin_poll_(current_origin_poll) {
        clock_.set_wake([this] {
            { std::lock_guard guard(mutex_); }
            cv_.notify_all();
        });
    }
    ~FramePacer() {
        clock_.set_wake({});
        {
            std::lock_guard guard(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(mutex_);  // waiters leave before the members go
        cv_.wait(lock, [this] { return waiters_ == 0; });
    }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // SetFrameIntervalX(output, interval, latency, flags). `period_us` is the interval as a period; `latency` is
    // the number of frames allowed in flight (0 counts as 1). The vblank grid is re-based: its epoch is now. A
    // scheduled origin event stays armed (with its offset, kept below the new period) and its next origin is one
    // new period from now. When the period changes, pending frames receive consecutive vblanks on that grid in
    // their original order. Reapplying the same period preserves their display deadlines.
    std::int32_t set_frame_interval(std::uint32_t period_us, std::uint32_t latency, std::uint32_t flags) {
        (void)flags;
        if (period_us == 0) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        if (suspended_) return pacer_hr::invalid_state;
        const auto now = clock_.now_us();
        if (period_us_ != period_us) {
            retire_locked(now);
            auto next_flip = now;
            for (Pending& frame : pending_) {
                next_flip += period_us;
                frame.flip_us = next_flip;
            }
        }
        period_us_ = period_us;
        latency_ = std::max(latency, 1u);
        epoch_us_ = now;
        interval_set_ = true;
        for (std::uint32_t k = 0; k < frame_event_count; ++k) {
            event_offset_us_[k] = std::min(event_offset_us_[k], period_us - 1);
            last_event_tick_[k] = 0;  // tick 0 is the epoch itself, which has passed
        }
        cv_.notify_all();
        return pacer_hr::ok;
    }

    // ScheduleFrameEventX(event, offset, ..., flags). Supports origin (0), vblank (1), and flip (2).
    std::int32_t schedule_frame_event(std::uint32_t event, std::uint32_t offset_us, std::uint32_t flags) {
        (void)flags;
        if (event >= frame_event_count) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        if (suspended_ || !interval_set_) return pacer_hr::invalid_state;
        if (offset_us >= period_us_) return pacer_hr::invalid_arg;
        event_offset_us_[event] = offset_us;
        event_armed_[event] = true;
        // The first event is the next one that has not come yet: tick n is due at epoch + n * period + offset.
        const std::uint64_t now = clock_.now_us();
        last_event_tick_[event] =
            now >= epoch_us_ + offset_us ? std::int64_t((now - epoch_us_ - offset_us) / period_us_) : 0;
        cv_.notify_all();
        return pacer_hr::ok;
    }

    // WaitFrameEventX(event, timeout, ..., flags, &token). Origin waits block until the next origin tick and while
    // `latency` frames are in flight, issuing a consecutive token on success (or on timeout with
    // frame_wait_flag_token_on_timeout, returning S_FALSE). Vblank/flip waits pace independently per event kind and
    // leave `*token` untouched.
    std::int32_t wait_frame_event(std::uint32_t event, std::uint32_t timeout_ms, std::uint32_t flags,
                                  std::uint64_t* token) {
        if (event >= frame_event_count) return pacer_hr::invalid_arg;
        if (event == frame_event_origin && !token) return pacer_hr::invalid_arg;
        std::unique_lock lock(mutex_);
        if (current_origin_poll_ && event == frame_event_origin && timeout_ms == 0 && (flags & wait_flag_current_token))
            return current_token_locked(token);
        ++waiters_;
        const std::int32_t result = wait_locked(lock, event, timeout_ms, flags, token);
        if (--waiters_ == 0) cv_.notify_all();
        return result;
    }

    // GetFrameStatisticsX(token, type_mask, &count, array). `count` is the room in `out` on entry and the number of
    // entries written on return. S_OK when every producible requested type was written (event, present, display),
    // S_FALSE with count 0 or fewer when they are not (there yet, the frame was replaced in a full queue and never
    // shown, or the token is unknown or has aged out of the last 64). With a null `out`, count only receives how many
    // entries are available.
    std::int32_t get_frame_statistics(std::uint64_t token, std::uint32_t type_mask, std::uint32_t* count,
                                      FrameStatistic* out) {
        if (!count) return pacer_hr::invalid_arg;
        std::lock_guard guard(mutex_);
        retire_locked(clock_.now_us());
        const Record* record = nullptr;
        for (const Record& r : records_)
            if (r.token == token) record = &r;
        FrameStatistic found[3];
        std::uint32_t n = 0, expected = 0;
        auto add = [&](std::uint32_t type, bool present, std::uint64_t time) {
            if (!(type_mask & type)) return;
            ++expected;
            if (!record || !present) return;
            found[n++] = FrameStatistic{type, 0, token, time, type == stat_display ? record->percent_scanned : 0.0f};
        };
        if (record) {
            add(stat_frame_event, true, record->origin_us);
            add(stat_present, record->presented, record->present_us);
            add(stat_display, record->displayed, record->display_us);
        } else {
            expected = ((type_mask & stat_frame_event) ? 1u : 0u) + ((type_mask & stat_present) ? 1u : 0u) +
                       ((type_mask & stat_display) ? 1u : 0u);
        }
        if (!out) {
            const std::uint32_t available = n;
            *count = available;
            return available && available == expected ? pacer_hr::ok : pacer_hr::false_;
        }
        const std::uint32_t written = std::min(n, *count);
        for (std::uint32_t i = 0; i < written; ++i) out[i] = found[i];
        *count = written;
        return written && written == expected ? pacer_hr::ok : pacer_hr::false_;
    }

    // Queue side. check_present_token before the frame is presented, commit_present_token after it was queued.
    // What real hardware does with a null, repeated or out-of-order token is unmeasured, and a title that presents
    // with Token = 0 (a loading screen), re-presents with one token, or presents from a render thread out of order
    // must not freeze. So the only refusal is a token that was never handed out (invalid_arg, which is a bug in the
    // caller); null, stale (older than the last presented) and repeated tokens return ok and say which kind they
    // were in `issue`, so the caller can log it. invalid_state while suspended.
    enum class TokenIssue { none, null_token, stale, repeated };
    std::int32_t check_present_token(std::uint64_t token, TokenIssue* issue = nullptr) {
        std::lock_guard guard(mutex_);
        if (issue) *issue = TokenIssue::none;
        if (suspended_) return pacer_hr::invalid_state;
        if (token > tokens_->issued()) return pacer_hr::invalid_arg;
        if (issue) {
            if (token == 0) *issue = TokenIssue::null_token;
            else if (token == last_presented_) *issue = TokenIssue::repeated;
            else if (token < last_presented_) *issue = TokenIssue::stale;
        }
        return pacer_hr::ok;
    }
    // How late the frame of `token` is if it is presented now (see the model above). late_us stays 0 when the frame
    // could not be shown at once anyway: unpaced, uncapped, null or unknown token, or earlier frames still waiting
    // for their vblank (a flip cannot overtake them), or the token was already presented or is older than the last
    // presented (a repeated or stale token never tears).
    struct PresentTiming {
        std::uint64_t late_us = 0;
        std::uint32_t period_us = 0;   // 0: unpaced
    };
    PresentTiming present_timing(std::uint64_t token) {
        std::lock_guard guard(mutex_);
        PresentTiming timing;
        if (!interval_set_ || uncapped_ || !token) return timing;
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);
        timing.period_us = period_us_;
        if (!pending_.empty()) return timing;
        for (const Record& r : records_)
            if (r.token == token && !r.presented && token > last_presented_ && now > r.due_us)
                timing.late_us = now - r.due_us;
        return timing;
    }

    // `sync` immediate: the frame is shown at once (display time = present time) instead of at the next vblank, and
    // percent_scanned is the lateness measured at this commit, so it agrees with the display time. The caller got
    // `sync` from decide_present_sync and the presenter confirmed it.
    void commit_present_token(std::uint64_t token, std::uint32_t plane_count, PresentSync sync = PresentSync::vblank) {
        (void)plane_count;
        std::lock_guard guard(mutex_);
        const std::uint64_t now = clock_.now_us();
        retire_locked(now);
        record_present_timing_locked(now, token);
        last_presented_ = std::max(last_presented_, token);
        if (!interval_set_ || !any_armed_locked() || uncapped_) {  // not paced or uncapped: shows at once
            for (Record& r : records_)
                if (r.token == token) { r.presented = r.displayed = true; r.present_us = r.display_us = now; }
            return;
        }
        const bool queued_before = !pending_.empty();
        std::uint64_t flip = queue_flip_locked(now);
        float percent = 0.0f;
        if (sync == PresentSync::immediate && !queued_before) {
            flip = now;
            std::uint64_t late_us = 0;
            for (const Record& r : records_)
                if (r.token == token && now > r.due_us) late_us = now - r.due_us;
            percent = scan_percent(late_us, period_us_);
            ++immediate_presents_;
        }
        pending_.push_back(Pending{token, flip, percent});
        for (Record& r : records_)
            if (r.token == token) { r.presented = true; r.present_us = now; }
        retire_locked(now);
        cv_.notify_all();
    }
    // A blank present (no planes) carries no token: it is only counted as a frame to show. With no interval set,
    // the origin event not armed or the cap off it is not paced: it shows at once and is not counted.
    void note_untokened_present() {
        std::lock_guard guard(mutex_);
        const std::uint64_t now = clock_.now_us();
        record_present_timing_locked(now, 0);
        if (!interval_set_ || !any_armed_locked() || uncapped_) return;
        retire_locked(now);
        const std::uint64_t flip = queue_flip_locked(now);
        pending_.push_back(Pending{0, flip, 0.0f});
        cv_.notify_all();
    }

    // SuspendX: presentation stops, the registration and the frames in flight are dropped, waiters fail.
    void suspend() {
        std::lock_guard guard(mutex_);
        ++suspend_generation_;
        suspended_ = true;
        interval_set_ = false;
        has_last_present_ = false;  // do not count the suspended pause as a frame interval after resume
        for (bool& a : event_armed_) a = false;
        pending_.clear();
        cv_.notify_all();
    }
    // ResumeX: presents work again, but the interval and the origin event must be set up again.
    void resume() {
        std::lock_guard guard(mutex_);
        suspended_ = false;
        interval_set_ = false;
        for (bool& a : event_armed_) a = false;
        cv_.notify_all();
    }

    bool suspended() const { std::lock_guard guard(mutex_); return suspended_; }
    std::uint64_t tokens_issued() const { return tokens_->issued(); }
    const std::shared_ptr<FrameTokenSource>& token_source() const { return tokens_; }
    std::uint32_t frames_in_flight() {
        std::lock_guard guard(mutex_);
        retire_locked(clock_.now_us());
        return std::uint32_t(pending_.size());
    }
    // Frames that were flipped mid-scan because of ImmediateThresholdPercent.
    std::uint64_t immediate_presents() const { std::lock_guard guard(mutex_); return immediate_presents_; }
    std::uint32_t period_us() const { std::lock_guard guard(mutex_); return period_us_; }
    bool registered() const { std::lock_guard guard(mutex_); return interval_set_ && any_armed_locked(); }

    // The fps cap off (LEONIDA_PRESENT_UNCAP; see the top comment): waits never block and presents count as shown
    // at once. Registration and suspension keep meaning what they did; the flag survives suspend/resume.
    void set_uncapped(bool on) { std::lock_guard guard(mutex_); uncapped_ = on; }
    bool uncapped() const { std::lock_guard guard(mutex_); return uncapped_; }

    // Frame-time percentiles, mean drift against the active target interval, and origin-to-present latency over
    // the last `frame_pacing_window` presented frames (BL-6).
    FramePacingSummary pacing_summary() const {
        FramePacingSummary out;
        std::vector<std::uint64_t> intervals;
        std::vector<std::uint64_t> latencies;
        {
            std::lock_guard guard(mutex_);
            out.frames = presents_recorded_;
            out.target_interval_us = (interval_set_ && !uncapped_) ? period_us_ : 0u;
            intervals.reserve(interval_count_);
            for (std::size_t i = 0; i < interval_count_; ++i) intervals.push_back(interval_ring_[i]);
            latencies.reserve(latency_count_);
            for (std::size_t i = 0; i < latency_count_; ++i) latencies.push_back(latency_ring_[i]);
        }
        auto percentile = [](const std::vector<std::uint64_t>& sorted, std::size_t pct) -> std::uint64_t {
            if (sorted.empty()) return 0;
            const std::size_t idx = (pct * sorted.size() + 99) / 100 - 1;
            return sorted[std::min(idx, sorted.size() - 1)];
        };
        if (!intervals.empty()) {
            out.samples = intervals.size();
            double sum = 0.0;
            for (std::uint64_t v : intervals) sum += double(v);
            out.mean_us = sum / double(intervals.size());
            if (out.target_interval_us != 0) out.drift_us = out.mean_us - double(out.target_interval_us);
            std::sort(intervals.begin(), intervals.end());
            out.p50_us = percentile(intervals, 50);
            out.p95_us = percentile(intervals, 95);
            out.p99_us = percentile(intervals, 99);
        }
        if (!latencies.empty()) {
            out.latency_samples = latencies.size();
            std::sort(latencies.begin(), latencies.end());
            out.latency_p50_us = percentile(latencies, 50);
            out.latency_p95_us = percentile(latencies, 95);
            out.latency_p99_us = percentile(latencies, 99);
        }
        return out;
    }

private:
    struct Pending {
        std::uint64_t token;
        std::uint64_t flip_us;
        float percent_scanned;  // where the scan was for a frame shown mid-scan, else 0
    };
    struct Record {
        std::uint64_t token = 0, origin_us = 0, present_us = 0, display_us = 0;
        std::uint64_t due_us = 0;  // vblank after this token's origin, fixed when issued
        float percent_scanned = 0.0f;
        bool presented = false, displayed = false;
    };

    // The vblank a frame presented now is queued for: the vblank after now, and not before the one after the previous
    // queued frame. At most `latency_` frames are in flight, so with the queue full the newest queued frame is
    // replaced (dropped: never shown) and the new one takes its vblank; the older ones keep theirs. A queue longer
    // than the latency (after SetFrameIntervalX lowered it) is cut back the same way, from the newest end.
    std::uint64_t queue_flip_locked(std::uint64_t now) {
        if (pending_.size() >= latency_) {  // latency_ is at least 1, so the queue is not empty
            std::uint64_t flip = 0;
            while (pending_.size() >= latency_) {
                flip = pending_.back().flip_us;
                pending_.pop_back();
            }
            return flip;
        }
        const std::uint64_t since = now > epoch_us_ ? now - epoch_us_ : 0;
        std::uint64_t flip = epoch_us_ + (since / period_us_ + 1) * period_us_;
        if (!pending_.empty()) flip = std::max(flip, pending_.back().flip_us + period_us_);
        return flip;
    }

    void record_present_timing_locked(std::uint64_t now, std::uint64_t token) {
        ++presents_recorded_;
        if (has_last_present_ && now >= last_present_us_) {
            interval_ring_[interval_head_] = now - last_present_us_;
            interval_head_ = (interval_head_ + 1) % frame_pacing_window;
            if (interval_count_ < frame_pacing_window) ++interval_count_;
        }
        has_last_present_ = true;
        last_present_us_ = now;
        if (token != 0) {
            for (const Record& r : records_) {
                if (r.token == token) {
                    if (now >= r.origin_us) {
                        latency_ring_[latency_head_] = now - r.origin_us;
                        latency_head_ = (latency_head_ + 1) % frame_pacing_window;
                        if (latency_count_ < frame_pacing_window) ++latency_count_;
                    }
                    break;
                }
            }
        }
    }

    // Frames whose vblank has come are shown.
    void retire_locked(std::uint64_t now) {
        while (!pending_.empty() && pending_.front().flip_us <= now) {
            for (Record& r : records_)
                if (r.token == pending_.front().token && pending_.front().token) {
                    r.displayed = true;
                    r.display_us = pending_.front().flip_us;
                    r.percent_scanned = pending_.front().percent_scanned;
                }
            pending_.pop_front();
        }
    }

    std::int32_t current_token_locked(std::uint64_t* token) {
        if (shutdown_ || suspended_) return pacer_hr::invalid_state;
        if (!interval_set_ || !event_armed_[frame_event_origin]) {
            *token = tokens_->issue();
            return pacer_hr::ok;
        }
        const auto now = clock_.now_us();
        retire_locked(now);
        const auto offset = event_offset_us_[frame_event_origin];
        const auto first = epoch_us_ + offset;
        const auto newest = now >= first ? static_cast<std::int64_t>((now - first) / period_us_) : 0;
        auto& tick = last_event_tick_[frame_event_origin];
        tick = std::max(tick, newest);
        const auto origin = first + static_cast<std::uint64_t>(tick) * period_us_;
        // The current token can be polled before the first configured origin.
        *token = issue_token_locked(std::min(origin, now), origin - offset + period_us_);
        return pacer_hr::ok;
    }

    bool any_armed_locked() const {
        return event_armed_[0] || event_armed_[1] || event_armed_[2];
    }

    std::uint64_t issue_token_locked(std::uint64_t origin_us, std::uint64_t due_us) {
        const std::uint64_t tok = tokens_->issue();  // the shared token source (the device proxy and PresentX pace with it)
        Record record;
        record.token = tok;
        record.origin_us = origin_us;
        record.due_us = due_us;
        records_.push_back(record);
        if (records_.size() > 64) records_.pop_front();
        return tok;
    }

    std::int32_t wait_locked(std::unique_lock<std::mutex>& lock, std::uint32_t event, std::uint32_t timeout_ms,
                             std::uint32_t flags, std::uint64_t* token) {
        const auto generation = suspend_generation_;  // resume may re-arm the event before this wait reacquires the lock
        const bool is_origin = (event == frame_event_origin);
        const bool token_on_timeout = (flags & frame_wait_flag_token_on_timeout) != 0;
        const std::uint64_t start = clock_.now_us();
        const std::uint64_t give_up = timeout_ms == infinite_timeout_ms ? no_deadline
                                                                         : start + std::uint64_t(timeout_ms) * 1000;
        for (;;) {
            if (shutdown_ || suspended_ || !interval_set_ || generation != suspend_generation_)
                return pacer_hr::invalid_state;
            if (is_origin ? !event_armed_[frame_event_origin]
                          : (!event_armed_[event] && !event_armed_[frame_event_origin])) {
                return pacer_hr::invalid_state;
            }
            const std::uint64_t now = clock_.now_us();
            retire_locked(now);
            if (uncapped_) {  // no vblank grid and no latency throttle
                if (is_origin) *token = issue_token_locked(now, now);
                return pacer_hr::ok;
            }
            const std::uint32_t offset = event_offset_us_[event];
            const std::uint64_t first = epoch_us_ + offset;  // tick 0
            const std::int64_t next = last_event_tick_[event] + 1;
            const std::uint64_t next_us = first + std::uint64_t(next) * period_us_;
            std::uint64_t wake = next_us;
            if (now >= next_us) {
                if (!is_origin || pending_.size() < latency_) {
                    const std::int64_t newest = std::int64_t((now - first) / period_us_);
                    const std::int64_t tick = std::max(next, newest);
                    last_event_tick_[event] = tick;
                    if (is_origin) {
                        const std::uint64_t origin_us = first + std::uint64_t(tick) * period_us_;
                        const std::uint64_t due_us = origin_us - offset + period_us_;
                        *token = issue_token_locked(origin_us, due_us);
                    }
                    return pacer_hr::ok;
                }
                wake = pending_.front().flip_us;  // blocked by the latency: until the oldest frame is shown
            }
            if (give_up != no_deadline && now >= give_up) {
                if (is_origin && token_on_timeout) {
                    // Frame started early on timeout: issue a consecutive token without consuming the tick.
                    *token = issue_token_locked(now, next_us - offset + period_us_);
                    return pacer_hr::false_;
                }
                return token_on_timeout ? pacer_hr::false_ : pacer_hr::timeout;
            }
            clock_.wait(lock, cv_, give_up == no_deadline ? wake : std::min(wake, give_up));
        }
    }

    FrameClock& clock_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool shutdown_ = false;
    std::uint32_t waiters_ = 0;

    bool suspended_ = false;
    std::uint64_t suspend_generation_ = 0;
    bool interval_set_ = false;
    bool event_armed_[frame_event_count] = {false, false, false};
    bool uncapped_ = false;
    std::uint32_t period_us_ = 0;
    std::uint32_t latency_ = 1;
    std::uint32_t event_offset_us_[frame_event_count] = {0, 0, 0};
    std::uint64_t epoch_us_ = 0;
    std::int64_t last_event_tick_[frame_event_count] = {0, 0, 0};  // tick 0 is the epoch itself, which has passed
    std::shared_ptr<FrameTokenSource> tokens_;
    bool current_origin_poll_ = false;
    std::uint64_t last_presented_ = 0;
    std::uint64_t immediate_presents_ = 0;
    std::uint64_t presents_recorded_ = 0;
    bool has_last_present_ = false;
    std::uint64_t last_present_us_ = 0;
    std::array<std::uint64_t, frame_pacing_window> interval_ring_{};
    std::size_t interval_head_ = 0;
    std::size_t interval_count_ = 0;
    std::array<std::uint64_t, frame_pacing_window> latency_ring_{};
    std::size_t latency_head_ = 0;
    std::size_t latency_count_ = 0;
    std::deque<Pending> pending_;
    std::deque<Record> records_;
};

}  // namespace leonida
