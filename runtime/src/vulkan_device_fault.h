#pragma once

#include <vulkan/vulkan.h>

#include "vulkan_device_markers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace leonida {

struct DeviceFaultExtensions {
    bool fault = false;
    bool checkpoints = false;
    bool diagnostics_config = false;
    bool markers = false;  // VK_AMD_buffer_marker (LEONIDA_DEVICE_MARKERS=1, vulkan_device_markers.h)
};

// What a device asks for. LEONIDA_DEVICE_FAULT=1 wants all of it; LEONIDA_AFTERMATH=1 alone (vulkan_aftermath.h) only
// needs the NVIDIA diagnostics config, whose flags turn on the Aftermath features of the GPU crash dump.
struct DeviceFaultRequest {
    bool fault = true;               // VK_EXT_device_fault and VK_NV_device_diagnostic_checkpoints
    bool diagnostics_config = true;  // VK_NV_device_diagnostics_config
    VkDeviceDiagnosticsConfigFlagsNV extra_flags = 0;  // added to the default config flags below
    bool markers = false;            // VK_AMD_buffer_marker, when the device has it
};

// Owns the feature structures until vkCreateDevice returns.
struct DeviceFaultFeatures {
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDeviceDiagnosticsConfigFeaturesNV diagnostics{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DIAGNOSTICS_CONFIG_FEATURES_NV};
    VkDeviceDiagnosticsConfigCreateInfoNV config{VK_STRUCTURE_TYPE_DEVICE_DIAGNOSTICS_CONFIG_CREATE_INFO_NV};

    DeviceFaultExtensions enable(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceFeatures2 query,
                                 std::span<const VkExtensionProperties> advertised,
                                 std::vector<const char*>& extensions, VkDeviceCreateInfo& device,
                                 DeviceFaultRequest request = {}) {
        const auto has = [&](const char* name) {
            return std::any_of(advertised.begin(), advertised.end(), [name](const auto& ext) {
                return std::strcmp(ext.extensionName, name) == 0;
            });
        };
        const bool has_fault = request.fault && has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        const bool has_config = request.diagnostics_config && has(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
        if (has_fault || has_config) {
            diagnostics.pNext = nullptr;
            fault.pNext = has_config ? &diagnostics : nullptr;
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = has_fault ? static_cast<void*>(&fault) : static_cast<void*>(&diagnostics);
            query(physical, &features);
        }
        const DeviceFaultExtensions enabled{has_fault && fault.deviceFault == VK_TRUE,
                                            request.fault && has(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME),
                                            has_config && diagnostics.diagnosticsConfig == VK_TRUE,
                                            request.markers && has(VK_AMD_BUFFER_MARKER_EXTENSION_NAME)};
        if (enabled.fault) {
            extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            fault.pNext = const_cast<void*>(device.pNext);
            device.pNext = &fault;
        }
        if (enabled.checkpoints) extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        if (enabled.markers) extensions.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
        if (enabled.diagnostics_config) {
            extensions.push_back(VK_NV_DEVICE_DIAGNOSTICS_CONFIG_EXTENSION_NAME);
            diagnostics.pNext = const_cast<void*>(device.pNext);
            config.pNext = &diagnostics;
            config.flags = VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_SHADER_DEBUG_INFO_BIT_NV |
                           VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_RESOURCE_TRACKING_BIT_NV |
                           VK_DEVICE_DIAGNOSTICS_CONFIG_ENABLE_AUTOMATIC_CHECKPOINTS_BIT_NV | request.extra_flags;
            device.pNext = &config;
        }
        return enabled;
    }
};

enum class CheckpointKind : std::uint8_t { draw = 1, dispatch, copy, resolve, clear };

struct CheckpointMarker {
    static constexpr std::uint32_t max_list = 0xff;
    static constexpr std::uint32_t max_command = 0x1ffff;
    static constexpr std::uint64_t tag = 0xa;

    std::uint32_t submission = 0;
    std::uint32_t list = 0;
    std::uint32_t command = 0;
    CheckpointKind kind{};

    // Vulkan treats the value as an opaque pointer. The runtime never dereferences it.
    static constexpr std::uintptr_t encode(std::uint32_t submission, std::size_t list, std::size_t command,
                                           CheckpointKind kind) {
        if (sizeof(void*) < 8 || !submission || list > max_list || command > max_command) return 0;
        return static_cast<std::uintptr_t>((tag << 60) | (std::uint64_t{submission} << 28) |
                                         (std::uint64_t{list} << 20) | (std::uint64_t{command} << 3) |
                                         static_cast<std::uint8_t>(kind));
    }

    static constexpr CheckpointMarker decode(std::uintptr_t marker) {
        const std::uint64_t bits = marker;
        const auto kind = static_cast<CheckpointKind>(bits & 7);
        if ((bits >> 60) != tag || kind < CheckpointKind::draw || kind > CheckpointKind::clear) return {};
        return {static_cast<std::uint32_t>(bits >> 28), static_cast<std::uint32_t>((bits >> 20) & max_list),
                static_cast<std::uint32_t>((bits >> 3) & max_command), kind};
    }

    const char* kind_name() const {
        switch (kind) {
        case CheckpointKind::draw: return "draw";
        case CheckpointKind::dispatch: return "dispatch";
        case CheckpointKind::copy: return "copy";
        case CheckpointKind::resolve: return "resolve";
        case CheckpointKind::clear: return "clear";
        default: return "unknown";
        }
    }
};

using CheckpointShaders = std::array<const char*, 3>;  // Immutable ShaderLibrary names, valid until context teardown.

struct CheckpointPipeline {
    VkPipeline handle = VK_NULL_HANDLE;
    std::uintptr_t id = 0;
    CheckpointShaders shaders{};
};

struct CheckpointList {
    struct ShaderChange {
        std::uint32_t command;
        CheckpointPipeline pipeline;
    };
    std::vector<ShaderChange> shaders;
};

// CPU record of work handed to vkQueueSubmit. A driver checkpoint can identify which
// entry it reached; on drivers without checkpoints these remain submitted candidates.
struct CheckpointDispatch {
    std::uint64_t execute_call;
    VkQueue queue;
    const char* queue_kind;
    std::size_t list, command;
    const char* shader;
    std::array<std::uint32_t, 3> groups;
    std::uintptr_t argument_buffer, count_buffer;
    std::uint64_t argument_offset, count_offset;
    std::uint32_t max_count, stride;
    std::array<std::uint32_t, 32> roots;
    bool indirect;
    std::uint64_t seq = 0;  // the report's dispatch number, counted over every recorded dispatch (CheckpointRecorder)
};

struct CheckpointSubmission {
    std::uint32_t sequence = 0;
    std::uint64_t execute_call = 0;
    VkQueue queue = VK_NULL_HANDLE;
    const char* queue_kind = nullptr;
    std::size_t list_count = 0;
    std::vector<CheckpointList> lists;
    std::vector<CheckpointDispatch> dispatches;
    std::size_t dispatch_count = 0;
    static constexpr std::size_t dispatch_limit = 64;
    // Numbers every recorded dispatch, across queues and submissions (the report's seq). Null: no numbers.
    std::atomic<std::uint64_t>* dispatch_seq = nullptr;
    // LEONIDA_DEVICE_MARKERS=1: this queue's marker slot, every dispatch recorded for the marker history and the seq
    // whose finished marker is still to be written after its dispatch (CheckpointRecorder::dispatched).
    MarkerTarget markers;
    std::vector<MarkedDispatch> marked;
    std::uint64_t open_marker = 0;

    void append_dispatch(CheckpointDispatch d) {
        if (dispatches.size() < dispatch_limit) dispatches.push_back(d);
        else dispatches[dispatch_count % dispatch_limit] = d;
        ++dispatch_count;
    }
};

struct CheckpointRecorder {
    PFN_vkCmdSetCheckpointNV set;
    CheckpointSubmission& submission;
    const std::unordered_map<std::uintptr_t, CheckpointShaders>& pipeline_names;
    std::size_t list;

    void record(VkCommandBuffer buffer, std::size_t command, CheckpointKind kind,
                VkPipeline pipeline = VK_NULL_HANDLE, std::uintptr_t id = 0) const {
        if (!set) return;
        const auto marker = CheckpointMarker::encode(submission.sequence, list, command, kind);
        if (!marker) return;
        if (kind == CheckpointKind::draw || kind == CheckpointKind::dispatch) {
            auto& shaders = submission.lists[list].shaders;
            if (shaders.empty() || shaders.back().pipeline.handle != pipeline || shaders.back().pipeline.id != id) {
                const auto names = pipeline_names.find(id);
                shaders.push_back({static_cast<std::uint32_t>(command),
                                   {pipeline, id, names != pipeline_names.end() ? names->second : CheckpointShaders{}}});
            }
        }
        set(buffer, reinterpret_cast<const void*>(marker));
    }

    void dispatch(VkCommandBuffer buffer, std::size_t command, VkPipeline pipeline, std::uintptr_t id,
                  const std::array<std::uint32_t, 32>& roots, std::array<std::uint32_t, 3> groups,
                  bool indirect = false, std::uintptr_t args = 0, std::uint64_t offset = 0,
                  std::uintptr_t count = 0, std::uint64_t count_offset = 0,
                  std::uint32_t max_count = 0, std::uint32_t stride = 0) const {
        const auto names = pipeline_names.find(id);
        const char* shader = names != pipeline_names.end() ? names->second[0] : nullptr;
        const std::uint64_t seq =
            submission.dispatch_seq ? submission.dispatch_seq->fetch_add(1, std::memory_order_relaxed) + 1 : 0;
        submission.append_dispatch({submission.execute_call, submission.queue, submission.queue_kind, list, command,
            shader, groups, args, count, offset, count_offset, max_count, stride, roots, indirect, seq});
        if (submission.markers) {
            submission.marked.push_back({seq, submission.execute_call, submission.queue, submission.queue_kind,
                                         static_cast<std::uint32_t>(list), static_cast<std::uint32_t>(command), shader});
            submission.markers.started(buffer, seq);
            submission.open_marker = seq;
        }
        record(buffer, command, CheckpointKind::dispatch, pipeline, id);
    }

    // After the dispatch call(s) that dispatch() announced: its finished marker.
    void dispatched(VkCommandBuffer buffer) const {
        if (!submission.open_marker) return;
        submission.markers.finished(buffer, submission.open_marker);
        submission.open_marker = 0;
    }
};

// A D3D fence may stay unsignalled by design. Only queue retirement tickets represent
// work that actually reached vkQueueSubmit.
class SubmissionProgressWatch {
public:
    using Clock = std::chrono::steady_clock;
    enum class State { idle, pending, submit_lock_busy };

    bool sample(State state, std::uint64_t retired, Clock::time_point now) {
        if (state == State::idle || state != state_ || retired != retired_) {
            state_ = state;
            retired_ = retired;
            since_ = now;
            reported_ = false;
            return false;
        }
        if (reported_ || now - since_ < std::chrono::seconds(15)) return false;
        reported_ = true;
        return true;
    }

private:
    State state_ = State::idle;
    std::uint64_t retired_ = 0;
    Clock::time_point since_{};
    bool reported_ = false;
};

// Runs on every VK_ERROR_DEVICE_LOST that DeviceFaultDiagnostics::observe or VulkanContext::mark_device_lost sees,
// with or without LEONIDA_DEVICE_FAULT. Set before the device exists by the Aftermath integration
// (vulkan_aftermath.h), which blocks there until its GPU crash dump is written.
inline std::atomic<void (*)() noexcept> device_lost_hook{nullptr};

inline void run_device_lost_hook() noexcept {
    if (const auto hook = device_lost_hook.load(std::memory_order_acquire)) hook();
}

// LOCAL DIAGNOSTIC (early device loss): what the submit lock, the recent submissions and the other threads were
// doing when a submission stalled or the device was lost.
inline std::int64_t diagnostic_ms() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
inline std::uint32_t diagnostic_thread_id() { return static_cast<std::uint32_t>(GetCurrentThreadId()); }
#else
inline std::uint32_t diagnostic_thread_id() { return 0; }
#endif

// Set by gta6rt's main (src/thread_stacks.cpp): prints the call chain of thread `tid`, or of every other thread
// when `tid` is 0. Unset in the tests.
inline std::atomic<void (*)(std::uint32_t tid, const char* why) noexcept> thread_stack_hook{nullptr};

inline void print_thread_stacks(std::uint32_t tid, const char* why) noexcept {
    if (const auto hook = thread_stack_hook.load(std::memory_order_acquire)) hook(tid, why);
}

// The compute pipeline vkCreateComputePipelines is building now (one at a time is enough: the reports name the one
// set last), for the submit-lock hold report. Names are ShaderLibrary strings, valid until device teardown.
struct DriverCompile {
    std::atomic<const char*> name{nullptr};
    std::atomic<const char*> trigger{nullptr};
    std::atomic<std::int64_t> since_ms{0};
    std::atomic<std::size_t> bytes{0};
};
inline DriverCompile driver_compile;

// VulkanContext's submit lock, which also remembers who holds it, since when and how many threads wait for it. A
// submit or an idle wait blocked inside the driver holds it for as long as it blocks; the stall watchdog and the loss
// report name that thread, and a hold of a second or more is logged when it ends.
class SubmitMutex {
public:
    static constexpr std::int64_t kLongHoldMs = 1000;
    // How long let_submit_finish(true) keeps letting queued submitters go first before the caller goes ahead anyway,
    // so a steady stream of submits cannot hold the compile worker back for ever. Waiting out the holder itself is
    // not capped: entering the driver while a submit is inside it is what lost the device.
    static constexpr std::int64_t kHandoffCapMs = 250;

    void lock() {
        if (mutex_.try_lock()) {
            taken();
            return;
        }
        // Counted until this thread owns the lock (owner_ set), so let_submit_finish never sees neither.
        waiting_.fetch_add(1, std::memory_order_relaxed);
        mutex_.lock();
        taken();
        waiting_.fetch_sub(1, std::memory_order_relaxed);
    }
    bool try_lock() {
        if (!mutex_.try_lock()) return false;
        taken();
        return true;
    }
    void unlock() {
        const std::int64_t since = since_.load(std::memory_order_relaxed);
        const std::uint32_t owner = owner_.load(std::memory_order_relaxed);
        owner_.store(0, std::memory_order_relaxed);
        releases_.fetch_add(1);  // seq_cst with the handoff_waiters_ load: no let_submit_finish misses this release
        mutex_.unlock();
        if (handoff_waiters_.load()) {
            std::lock_guard lock(handoff_mutex_);
            released_.notify_all();
        }
        const std::int64_t now = diagnostic_ms();
        if (now - since >= kLongHoldMs)
            std::fprintf(stderr, "[rt] submit-lock: held %lld ms by thread %u, t=%.3f..%.3f s\n",
                         static_cast<long long>(now - since), owner, double(since) / 1000.0, double(now) / 1000.0);
    }

    std::uint32_t owner() const { return owner_.load(std::memory_order_relaxed); }
    std::int64_t since_ms() const { return since_.load(std::memory_order_relaxed); }
    // Threads blocked in lock() (try_lock never counts).
    std::uint32_t waiting() const { return waiting_.load(std::memory_order_relaxed); }

    // Called before the driver calls that build pipelines: waits for a submit in progress to finish (unless this
    // thread is the one submitting). Measured on NVIDIA 630.20 (three early losses): back-to-back pipeline creates on
    // the compile worker keep re-taking a driver lock that vkQueueSubmit waits for; after 10 s of that wait the driver
    // returns VK_ERROR_DEVICE_LOST, with no GPU fault and no TDR. Not entering the driver while a submit is in it lets
    // the submit take that lock between two creates.
    // `queued` (the compile worker): also lets the submitters queued behind the holder go first - std::mutex is not
    // fair, so without that the worker's next create could get in ahead of them - for at most `cap_ms`; past it the
    // hold in progress is the last one waited for. The wait never takes the lock itself (taking it could queue behind
    // every submitter that re-takes it), it waits for the hold it saw to end. Returns how many milliseconds it waited.
    std::int64_t let_submit_finish(bool queued = true, std::int64_t cap_ms = kHandoffCapMs) {
        const std::uint32_t self = diagnostic_thread_id();
        if (owner_.load(std::memory_order_relaxed) == self) return 0;  // this thread submits: nothing to let through
        const auto queue = [&] { return queued && waiting_.load(std::memory_order_relaxed) != 0; };
        std::uint64_t seen = releases_.load();
        const auto held = [&] {
            seen = releases_.load();  // read before the owner: a hold that ends after this changes it
            return owner_.load() != 0;
        };
        if (!held() && !queue()) return 0;
        const std::int64_t start = diagnostic_ms();
        for (;;) {
            const bool over = diagnostic_ms() - start >= cap_ms;
            if (held()) {
                wait_release(seen);
                if (!queued || over) break;
            } else if (!queue() || over) {
                break;
            } else {
                std::this_thread::yield();  // a queued submitter is between its wake-up and taking the lock
            }
        }
        return diagnostic_ms() - start;
    }

    // The compile gate. A queue submission never enters the driver while a pipeline create is in it: measured on
    // NVIDIA 630.20 (wf6), a vkQueueSubmit that entered while the warm-up thread was inside one long
    // vkCreateComputePipelines waited 10 s in the driver and returned VK_ERROR_DEVICE_LOST; let_submit_finish cannot
    // help once the create is already running. Creates pass the gate shared, a submission exclusively, and a create
    // that arrives while a submission waits queues behind it, so a submission waits only for the creates already in
    // the driver. A long compile then stalls the submitting thread here, in our code, instead of in the driver.
    // No deadlock: a create holds the gate only across its vkCreate*Pipelines and a submission only across its
    // vkQueueSubmit, and neither driver call waits for anything of ours. LEONIDA_COMPILE_GATE=0 turns it off.
    class CompileTurn {  // around vkCreate*Pipelines
    public:
        explicit CompileTurn(SubmitMutex& gate) : gate_(gate), waited_(gate.enter_compile()) {}
        ~CompileTurn() { gate_.leave_compile(); }
        CompileTurn(const CompileTurn&) = delete;
        CompileTurn& operator=(const CompileTurn&) = delete;
        std::int64_t waited_ms() const { return waited_; }

    private:
        SubmitMutex& gate_;
        std::int64_t waited_;
    };
    class SubmitTurn {  // around vkQueueSubmit, with the submit lock held
    public:
        explicit SubmitTurn(SubmitMutex& gate) : gate_(gate) { gate.enter_submit(); }
        ~SubmitTurn() { gate_.leave_submit(); }
        SubmitTurn(const SubmitTurn&) = delete;
        SubmitTurn& operator=(const SubmitTurn&) = delete;

    private:
        SubmitMutex& gate_;
    };

    static bool compile_gate_enabled() {
        static const bool on = [] {
            const char* value = std::getenv("LEONIDA_COMPILE_GATE");
            return !(value && !std::strcmp(value, "0"));
        }();
        return on;
    }

    // The thread waiting in enter_submit for creates to leave the driver, or 0.
    std::uint32_t gate_waiter() const { return gate_waiter_.load(std::memory_order_relaxed); }

    struct GateStats {
        std::uint64_t submits = 0;         // submissions that had to wait for a create
        std::int64_t submit_ms = 0, submit_max_ms = 0;
        std::uint64_t creates = 0;         // creates that waited behind a submission
        std::int64_t create_ms = 0, create_max_ms = 0;
    };
    GateStats gate_stats() {
        std::lock_guard lock(gate_mutex_);
        return stats_;
    }

    // Waits (without entering the compile gate) while a submission is queued at the gate behind an earlier compile,
    // so a caller about to compile locally can re-check its journal file before taking a turn.
    std::int64_t wait_for_compile_gate_drain() {
        if (!compile_gate_enabled()) return 0;
        std::unique_lock lock(gate_mutex_);
        if (!submits_) return 0;
        const std::int64_t start = diagnostic_ms();
        gate_changed_.wait(lock, [&] { return submits_ == 0; });
        return diagnostic_ms() - start;
    }

    std::int64_t enter_compile() {
        if (!compile_gate_enabled()) return 0;
        std::unique_lock lock(gate_mutex_);
        std::int64_t waited = 0;
        if (submits_) {
            const std::int64_t start = diagnostic_ms();
            gate_changed_.wait(lock, [&] { return submits_ == 0; });
            waited = diagnostic_ms() - start;
            ++stats_.creates;
            stats_.create_ms += waited;
            stats_.create_max_ms = (std::max)(stats_.create_max_ms, waited);
        }
        ++compiles_;
        return waited;
    }
    void leave_compile() {
        if (!compile_gate_enabled()) return;
        std::lock_guard lock(gate_mutex_);
        if (--compiles_ == 0 && submits_) gate_changed_.notify_all();
    }

    std::int64_t enter_submit() {
        if (!compile_gate_enabled()) return 0;
        std::unique_lock lock(gate_mutex_);
        ++submits_;
        if (!compiles_) return 0;
        const std::uint32_t self = diagnostic_thread_id();
        gate_waiter_.store(self, std::memory_order_relaxed);
        const std::int64_t start = diagnostic_ms();
        const GateWait what{self, compiles_, driver_compile.name.load(std::memory_order_acquire),
                            driver_compile.trigger.load(std::memory_order_relaxed),
                            driver_compile.bytes.load(std::memory_order_relaxed)};
        std::int64_t next_note = 2000;
        while (!gate_changed_.wait_for(lock, std::chrono::milliseconds(250), [&] { return compiles_ == 0; })) {
            const std::int64_t waited = diagnostic_ms() - start;
            if (waited < next_note) continue;
            next_note = waited + 5000;
            note_gate_wait("has waited", what, waited);
        }
        gate_waiter_.store(0, std::memory_order_relaxed);
        const std::int64_t waited = diagnostic_ms() - start;
        ++stats_.submits;
        stats_.submit_ms += waited;
        stats_.submit_max_ms = (std::max)(stats_.submit_max_ms, waited);
        // Every wait of 500 ms or more, and the first 32 of 50 ms or more.
        if (waited >= 500 || (waited >= 50 && logged_waits_ < 32)) {
            ++logged_waits_;
            note_gate_wait("waited", what, waited);
        }
        return waited;
    }
    void leave_submit() {
        if (!compile_gate_enabled()) return;
        std::lock_guard lock(gate_mutex_);
        if (--submits_ == 0) gate_changed_.notify_all();
    }

private:
    void taken() {
        since_.store(diagnostic_ms(), std::memory_order_relaxed);
        owner_.store(diagnostic_thread_id(), std::memory_order_relaxed);
    }

    // Until a hold ends after `seen` releases.
    void wait_release(std::uint64_t seen) {
        handoff_waiters_.fetch_add(1);
        {
            std::unique_lock lock(handoff_mutex_);
            released_.wait(lock, [&] { return releases_.load() != seen; });
        }
        handoff_waiters_.fetch_sub(1);
    }

    // With gate_mutex_ held.
    // The creates a submission found in the driver: how many, and the compute one named last (graphics creates are
    // not named). Names are ShaderLibrary strings, valid until device teardown.
    struct GateWait {
        std::uint32_t thread;
        std::uint32_t creates;
        const char* name;
        const char* trigger;
        std::size_t bytes;
    };
    void note_gate_wait(const char* how, const GateWait& what, std::int64_t waited) const {
        std::fprintf(stderr, "[rt] compile-gate: vkQueueSubmit on thread %u %s %lld ms for %u pipeline create(s) to "
                             "leave the driver (compute %s, %zu B SPIR-V, %s) at t=%.3f s; %llu such waits so far, "
                             "%lld ms in all, longest %lld ms\n",
                     what.thread, how, static_cast<long long>(waited), what.creates, what.name ? what.name : "-",
                     what.name ? what.bytes : std::size_t{0}, what.name && what.trigger ? what.trigger : "-",
                     double(diagnostic_ms()) / 1000.0, static_cast<unsigned long long>(stats_.submits),
                     static_cast<long long>(stats_.submit_ms), static_cast<long long>(stats_.submit_max_ms));
    }

    std::mutex mutex_;
    std::atomic<std::uint32_t> owner_{0};
    std::atomic<std::int64_t> since_{0};
    std::atomic<std::uint32_t> waiting_{0};
    std::atomic<std::uint64_t> releases_{0};  // unlocks so far
    std::atomic<std::uint32_t> handoff_waiters_{0};
    std::mutex handoff_mutex_;  // leaf
    std::condition_variable released_;

    std::mutex gate_mutex_;  // leaf: guards the gate counts and stats
    std::condition_variable gate_changed_;
    std::uint32_t compiles_ = 0;  // creates in the driver
    std::uint32_t submits_ = 0;   // submissions waiting for them or in the driver
    std::atomic<std::uint32_t> gate_waiter_{0};
    GateStats stats_;
    unsigned logged_waits_ = 0;
};

// One vkQueueSubmit of a VulkanQueue: its waits, its signals and how long the call took (-1: not returned yet).
struct SubmitRecord {
    static constexpr std::size_t max_waits = 6;
    std::int64_t at_ms = 0;
    std::int64_t took_ms = -1;
    int result = 1;
    VkQueue queue = VK_NULL_HANDLE;
    const char* kind = "";
    VkSemaphore progress = VK_NULL_HANDLE;
    std::uint64_t ticket = 0;
    VkSemaphore fence = VK_NULL_HANDLE;
    std::uint64_t fence_value = 0;
    std::uint32_t buffers = 0;
    std::uint32_t waits = 0;
    std::array<VkSemaphore, max_waits> wait_semaphores{};
    std::array<std::uint64_t, max_waits> wait_values{};
};

class DeviceFaultDiagnostics {
public:
    // `memory` and `synchronization2` are for the AMD markers (enabled.markers): where their buffer goes and whether
    // vkCmdWriteBufferMarker2AMD may be used.
    DeviceFaultDiagnostics(PFN_vkGetDeviceProcAddr get, VkDevice device, DeviceFaultExtensions enabled,
                           const VkPhysicalDeviceMemoryProperties* memory = nullptr, bool synchronization2 = false)
        : device_(device) {
#define LEONIDA_FAULT_LOAD(name) name = reinterpret_cast<PFN_##name>(get(device, #name))
        if (enabled.fault) LEONIDA_FAULT_LOAD(vkGetDeviceFaultInfoEXT);
        if (enabled.checkpoints) {
            LEONIDA_FAULT_LOAD(vkCmdSetCheckpointNV);
            LEONIDA_FAULT_LOAD(vkGetQueueCheckpointDataNV);
        }
        LEONIDA_FAULT_LOAD(vkGetSemaphoreCounterValue);
#undef LEONIDA_FAULT_LOAD
        std::fprintf(stderr, "[rt] device-fault: enabled (fault info %s, checkpoints %s, NVIDIA diagnostics config %s)\n",
                     vkGetDeviceFaultInfoEXT ? "on" : "unavailable", checkpoint_command() ? "on" : "unavailable",
                     enabled.diagnostics_config ? "on" : "unavailable");
        if (enabled.markers) enable_markers(get, memory, synchronization2);
        else if (device_markers_requested())
            std::fprintf(stderr, "[rt] device-markers: unavailable: the device has no VK_AMD_buffer_marker\n");
    }

    bool markers_on() const { return markers_ != nullptr; }

    // The queue's marker slot as the GPU last wrote it ({0, 0} without markers).
    MarkerSlot marker_slot(VkQueue queue) {
        std::lock_guard lock(mutex_);
        return markers_ ? markers_->read(queue) : MarkerSlot{};
    }

    PFN_vkCmdSetCheckpointNV checkpoint_command() const {
        return sizeof(void*) >= 8 && vkGetQueueCheckpointDataNV ? vkCmdSetCheckpointNV : nullptr;
    }

    void add_queue(VkQueue queue) {
        if (!queue) return;
        std::lock_guard lock(mutex_);
        if (markers_ && !markers_->add_queue(queue))
            std::fprintf(stderr, "[rt] device-markers: no slot for queue %p (%u queues have one)\n",
                         static_cast<void*>(queue), DeviceMarkers::max_queues);
        if (!vkGetQueueCheckpointDataNV) return;
        if (std::find(queues_.begin(), queues_.end(), queue) == queues_.end()) queues_.push_back(queue);
    }

    std::uint64_t next_execute_call() { return ++execute_calls_; }

    void set_submit_lock(const SubmitMutex* lock) { submit_lock_ = lock; }

    // Called under the submit lock, before and after the vkQueueSubmit it describes.
    std::size_t begin_submit(const SubmitRecord& record) {
        std::lock_guard lock(mutex_);
        const std::size_t at = submit_count_++;
        submits_[at % submits_.size()] = record;
        return at;
    }
    void end_submit(std::size_t at, std::int64_t took_ms, VkResult result) {
        std::lock_guard lock(mutex_);
        if (submit_count_ - at > submits_.size()) return;
        auto& record = submits_[at % submits_.size()];
        record.took_ms = took_ms;
        record.result = int(result);
    }

    // The holder of the submit lock has kept it for `held_ms`: its call chain and what was submitted last.
    void report_submit_lock_holder(std::uint32_t owner, std::int64_t held_ms) {
        std::fprintf(stderr, "[rt] submit-lock: thread %u has held it for %lld ms (t=%.3f s)\n", owner,
                     static_cast<long long>(held_ms), double(diagnostic_ms()) / 1000.0);
        if (const char* name = driver_compile.name.load(std::memory_order_acquire)) {
            const char* trigger = driver_compile.trigger.load(std::memory_order_relaxed);
            std::fprintf(stderr, "[rt] submit-lock: vkCreateComputePipelines has been building %s (%zu B SPIR-V, %s) "
                                 "for %lld ms\n",
                         name, driver_compile.bytes.load(std::memory_order_relaxed), trigger ? trigger : "?",
                         static_cast<long long>(diagnostic_ms() - driver_compile.since_ms.load(std::memory_order_relaxed)));
        }
        if (submit_lock_ && submit_lock_->gate_waiter() == owner) {
            // Not in the driver: waiting at the compile gate for the create named above (SubmitMutex::CompileTurn).
            std::fprintf(stderr, "[rt] submit-lock: thread %u is waiting at the compile gate, not in the driver\n", owner);
            return;
        }
        // Every thread, the holder included: what the driver waits for may be another thread's work. No counters
        // here (dump_submits would block in the driver behind the holder); the loss report has them. A long hold can
        // also be a legitimate vkDeviceWaitIdle: the stacks of the first three holds, then at most one a minute.
        const std::int64_t now = diagnostic_ms();
        if (stack_reports_ >= 3 && now - last_stack_report_ms_ < 60000) return;
        ++stack_reports_;
        last_stack_report_ms_ = now;
        print_thread_stacks(0, "submit-lock hold");
    }

    // Before `semaphore` is destroyed (~VulkanFence, ~VulkanQueue): the ring stops naming it, so no report reads the
    // counter of a destroyed semaphore. Waits for a report that is reading counters now.
    void forget(VkSemaphore semaphore) {
        if (!semaphore) return;
        std::lock_guard reading(read_mutex_);
        if (stalled_progress_ == semaphore) stalled_progress_ = VK_NULL_HANDLE;
        std::lock_guard lock(mutex_);
        for (auto& r : submits_) {
            if (r.progress == semaphore) r.progress = VK_NULL_HANDLE;
            if (r.fence == semaphore) r.fence = VK_NULL_HANDLE;
            for (auto& wait : r.wait_semaphores)
                if (wait == semaphore) wait = VK_NULL_HANDLE;
        }
    }

    // Once a second from the progress watch, without the submit lock: when the oldest submission in the ring that
    // has not finished is 4 s old, the ring with every counter as it is now, once per such submission.
    void check_submit_progress() {
        std::lock_guard reading(read_mutex_);  // the semaphores read below outlive this call (forget)
        const std::vector<SubmitRecord> records = submit_records();
        if (!vkGetSemaphoreCounterValue) return;
        const std::int64_t now = diagnostic_ms();
        for (const auto& r : records) {
            if (r.took_ms < 0 || r.result != VK_SUCCESS || !r.progress) continue;
            std::uint64_t done = 0;
            if (vkGetSemaphoreCounterValue(device_, r.progress, &done) != VK_SUCCESS) return;
            if (done >= r.ticket) continue;
            // Age from when vkQueueSubmit returned: the call itself may have waited at the compile gate.
            if (now - (r.at_ms + r.took_ms) < 4000) return;  // the oldest unfinished one is young
            if (r.progress == stalled_progress_ && r.ticket == stalled_ticket_) return;
            stalled_progress_ = r.progress;
            stalled_ticket_ = r.ticket;
            std::fprintf(stderr, "[rt] gpu-stall: %s queue %p ticket %llu submitted at t=%.3f s not done after %lld ms "
                                 "(progress %llu)\n",
                         r.kind, static_cast<void*>(r.queue), static_cast<unsigned long long>(r.ticket),
                         double(r.at_ms) / 1000.0, static_cast<long long>(now - r.at_ms),
                         static_cast<unsigned long long>(done));
            dump_submits_reading();
            return;
        }
    }

    // The last submissions, oldest first, with every semaphore's counter now (? once the device is lost, or for a
    // semaphore destroyed since).
    void dump_submits() {
        std::lock_guard reading(read_mutex_);
        dump_submits_reading();
    }

private:
    std::vector<SubmitRecord> submit_records() {
        std::vector<SubmitRecord> records;
        std::lock_guard lock(mutex_);
        const std::size_t n = std::min(submit_count_, submits_.size());
        records.reserve(n);
        for (std::size_t i = submit_count_ - n; i < submit_count_; ++i) records.push_back(submits_[i % submits_.size()]);
        return records;
    }

    // dump_submits with read_mutex_ held.
    void dump_submits_reading() {
        const std::vector<SubmitRecord> records = submit_records();
        // Every semaphore the ring names is non-null when recorded: null is one destroyed since (forget).
        const auto counter = [&](VkSemaphore semaphore) -> std::string {
            std::uint64_t value = 0;
            if (!semaphore) return "destroyed";
            if (!vkGetSemaphoreCounterValue || vkGetSemaphoreCounterValue(device_, semaphore, &value) != VK_SUCCESS)
                return "?";
            return std::to_string(value);
        };
        const std::int64_t now = diagnostic_ms();
        for (const auto& r : records) {
            std::string line = "[rt] submit-ring: t=" + std::to_string(double(r.at_ms) / 1000.0) + " s " + r.kind +
                               " queue=" + std::to_string(reinterpret_cast<std::uintptr_t>(r.queue)) +
                               " buffers=" + std::to_string(r.buffers) + " ticket=" + std::to_string(r.ticket) +
                               " progress=" + counter(r.progress) + " took=" +
                               (r.took_ms < 0 ? "pending(" + std::to_string(now - r.at_ms) + " ms)"
                                              : std::to_string(r.took_ms) + " ms") +
                               " result=" + std::to_string(r.result);
            if (r.fence || r.fence_value)
                line += " signal=" + std::to_string(pipeline_handle(r.fence)) + ":" + std::to_string(r.fence_value) +
                        " now " + counter(r.fence);
            for (std::uint32_t i = 0; i < r.waits && i < SubmitRecord::max_waits; ++i)
                line += " wait=" + std::to_string(pipeline_handle(r.wait_semaphores[i])) + ":" +
                        std::to_string(r.wait_values[i]) + " now " + counter(r.wait_semaphores[i]);
            if (r.waits > SubmitRecord::max_waits) line += " (+" + std::to_string(r.waits - SubmitRecord::max_waits) + " waits)";
            line += '\n';
            std::fputs(line.c_str(), stderr);
        }
    }

public:
    // Accessed under the context's resource lock. Reports read the names copied into the ring instead.
    std::unordered_map<std::uintptr_t, CheckpointShaders> pipeline_names;

    // Called under the context's tracker lock, in replay order.
    CheckpointSubmission begin_submission(std::uint64_t call, VkQueue queue, const char* kind, std::size_t lists) {
        if (!++sequence_) ++sequence_;  // 0 is reserved for invalid markers
        CheckpointSubmission submission{sequence_, call, queue, kind, lists,
                std::vector<CheckpointList>(std::min(lists, std::size_t{CheckpointMarker::max_list} + 1))};
        submission.dispatch_seq = &dispatch_seq_;
        if (markers_) {
            std::lock_guard lock(mutex_);
            submission.markers = markers_->target(queue);
        }
        return submission;
    }

    void publish(CheckpointSubmission&& submission) {
        std::lock_guard lock(mutex_);
        for (std::size_t i = submission.dispatch_count - submission.dispatches.size(); i < submission.dispatch_count; ++i)
            dispatches_[dispatch_count_++ % dispatches_.size()] = submission.dispatches[i % submission.dispatch_limit];
        submission.dispatches.clear();
        if (history_)
            for (const auto& d : submission.marked) history_->push(d);
        submission.marked.clear();
        recent_[submission.sequence % recent_.size()] = std::move(submission);
    }

    static VkResult observe(DeviceFaultDiagnostics* diagnostics, VkResult result, const char* operation) noexcept {
        if (result == VK_ERROR_DEVICE_LOST) {
            if (diagnostics) {
                diagnostics->report(operation);
            } else {
                // Without LEONIDA_DEVICE_FAULT a loss was silent: the title only saw ~0 in its fence words.
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true, std::memory_order_relaxed))
                    std::fprintf(stderr, "[rt] vulkan: VK_ERROR_DEVICE_LOST from %s at t=%.3f s on thread %u "
                                         "(LEONIDA_DEVICE_FAULT=1 for the fault report)\n",
                                 operation, double(diagnostic_ms()) / 1000.0, diagnostic_thread_id());
            }
            run_device_lost_hook();
        }
        return result;
    }

    void report_stall(const char* reason, std::uint64_t submitted, std::uint64_t retired) {
        std::fprintf(stderr, "[rt] device-stall: %s for 15 s; submitted ticket %" PRIu64
                             ", last observed retired ticket %" PRIu64 "\n", reason, submitted, retired);
        // NV checkpoints and EXT fault data require a lost device. The CPU ring and the AMD markers are valid at any
        // time.
        dump_dispatches(report_markers(false));
        // LEONIDA_STALL_CHECKPOINTS=1 (diagnostic): ask for the checkpoints anyway. A hang (Xid 109) the driver
        // recovers from without losing the device otherwise names no program; outside the lost state the call is
        // out of the extension's contract, which the NVIDIA driver has answered so far.
        static const bool stall_checkpoints = [] {
            const char* v = std::getenv("LEONIDA_STALL_CHECKPOINTS");
            return v && std::strcmp(v, "1") == 0;
        }();
        if (!stall_checkpoints || !vkGetQueueCheckpointDataNV) return;
        try {
            std::lock_guard lock(mutex_);
            for (VkQueue queue : queues_) report_queue(queue);
        } catch (...) {
            std::fprintf(stderr, "[rt] device-fault: checkpoint collection failed\n");
        }
    }

    // LEONIDA_DEVICE_MARKERS=1: each queue's marker slot judged against the marker history, printed; returns the
    // likely hangs (the oldest started, unfinished dispatch of each queue) for dump_dispatches to mark.
    std::vector<std::uint64_t> report_markers(bool lost) noexcept {
        std::vector<std::uint64_t> hangs;
        if (!markers_) return hangs;
        try {
            std::lock_guard lock(mutex_);
            std::fprintf(stderr, "[rt] device-markers: AMD buffer markers, read %s; best-effort: %s\n",
                         lost ? "after the device loss" : "while the device is still up",
                         lost ? "only what the GPU wrote before it stopped, and a reset may have lost the last writes"
                              : "the GPU may still be writing them");
            for (VkQueue queue : markers_->queues()) {
                const MarkerQueueReport r = judge_markers(queue, markers_->read(queue), *history_);
                if (!r.slot.started && !r.slot.finished && !r.next_queued) continue;  // never marked a dispatch
                const char* kind = r.queue_kind ? r.queue_kind : "?";
                if (r.idle()) {
                    std::fprintf(stderr, "[rt] device-markers: queue %p (%s) idle: finished seq=%u, nothing started "
                                         "after it", static_cast<void*>(queue), kind, unsigned(r.slot.finished));
                    if (r.next_queued)
                        std::fprintf(stderr, "; next submitted seq=%" PRIu64 " not started (waiting, or behind other "
                                             "work of the queue)", r.next_queued);
                    std::fputc('\n', stderr);
                    continue;
                }
                std::string line = "[rt] device-markers: queue " + hex(queue) + " (" + kind + ") finished seq=" +
                                   std::to_string(r.slot.finished) + " started seq=" + std::to_string(r.slot.started) +
                                   "; started, not finished:";
                for (std::size_t i = 0; i < r.unfinished.size() && i < 16; ++i)
                    line += " " + std::to_string(r.unfinished[i]);
                if (r.unfinished.size() > 16) line += " (+" + std::to_string(r.unfinished.size() - 16) + ")";
                if (r.missing) line += " (no longer in the marker history)";
                line += '\n';
                std::fputs(line.c_str(), stderr);
                if (!r.likely_hang) continue;
                hangs.push_back(r.likely_hang);
                history_->for_each([&](const MarkedDispatch& d) {
                    if (d.seq != r.likely_hang) return;
                    std::fprintf(stderr, "[rt] device-markers: likely hang: seq=%" PRIu64 " execute=%" PRIu64
                                         " queue=%p (%s) list=%u command=%u shader=%s\n",
                                 d.seq, d.execute_call, static_cast<void*>(d.queue), kind, d.list, d.command,
                                 d.shader ? d.shader : "-");
                });
            }
        } catch (...) {
            std::fprintf(stderr, "[rt] device-markers: marker collection failed\n");
        }
        return hangs;
    }

private:
    void enable_markers(PFN_vkGetDeviceProcAddr get, const VkPhysicalDeviceMemoryProperties* memory,
                        bool synchronization2) {
        auto markers = std::make_unique<DeviceMarkers>();
        const VkResult result =
            memory ? markers->create(get, device_, *memory, synchronization2) : VK_ERROR_INITIALIZATION_FAILED;
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[rt] device-markers: unavailable: the marker buffer could not be made (%d)\n",
                         int(result));
            return;
        }
        std::fprintf(stderr, "[rt] device-markers: on (VK_AMD_buffer_marker, %s): each dispatch writes its seq at top "
                             "and bottom of pipe\n",
                     markers->synchronization2() ? "vkCmdWriteBufferMarker2AMD" : "vkCmdWriteBufferMarkerAMD");
        history_ = std::make_unique<MarkerHistory>();
        markers_ = std::move(markers);
    }

    template<typename Handle>
    static std::string hex(Handle handle) {
        char text[24];
        std::snprintf(text, sizeof(text), "%p", static_cast<void*>(handle));
        return text;
    }

    void dump_dispatches(const std::vector<std::uint64_t>& hangs = {}) {
        std::lock_guard lock(mutex_);
        const auto n = std::min(dispatch_count_, dispatches_.size());
        for (std::size_t i = dispatch_count_ - n; i < dispatch_count_; ++i) {
            const auto& d = dispatches_[i % dispatches_.size()];
            std::fprintf(stderr, "[rt] device-fault: submitted dispatch seq=%" PRIu64 " execute=%" PRIu64
                         " queue=%p (%s) list=%zu command=%zu shader=%s ", d.seq, d.execute_call,
                         static_cast<void*>(d.queue), d.queue_kind, d.list, d.command, d.shader ? d.shader : "-");
            if (d.indirect)
                std::fprintf(stderr, "indirect args=0x%" PRIxPTR "+0x%" PRIx64 " count=0x%" PRIxPTR
                             "+0x%" PRIx64 " max=%u stride=%u", d.argument_buffer, d.argument_offset,
                             d.count_buffer, d.count_offset, d.max_count, d.stride);
            else std::fprintf(stderr, "groups=%u,%u,%u", d.groups[0], d.groups[1], d.groups[2]);
            if (include_roots_) {
                std::fputs(" roots=", stderr);
                for (auto word : d.roots) std::fprintf(stderr, "%08x ", word);
            }
            if (d.seq && std::find(hangs.begin(), hangs.end(), d.seq) != hangs.end())
                std::fputs(" <- likely hang (device-markers)", stderr);
            std::fputc('\n', stderr);
        }
    }

public:
    // LOCAL DIAGNOSTIC: run once after a loss is reported (the compute input probe rechecks the descriptors it saw).
    void set_lost_hook(std::function<void()> hook) {
        std::lock_guard lock(mutex_);
        lost_hook_ = std::move(hook);
    }

private:
    std::function<void()> lost_hook_;

    void report(const char* operation) noexcept {
        if (reported_.exchange(true, std::memory_order_relaxed)) return;
        std::fprintf(stderr, "[rt] device-fault: VK_ERROR_DEVICE_LOST from %s\n", operation);
        try {
            const std::int64_t now = diagnostic_ms();
            const std::uint32_t owner = submit_lock_ ? submit_lock_->owner() : 0;
            std::fprintf(stderr, "[rt] device-fault: t=%.3f s on thread %u; submit lock held by thread %u for %lld ms\n",
                         double(now) / 1000.0, diagnostic_thread_id(), owner,
                         owner ? static_cast<long long>(now - submit_lock_->since_ms()) : 0LL);
            dump_submits();
            print_thread_stacks(0, "device lost");
        } catch (...) {
        }
        dump_dispatches(report_markers(true));
        try {
            std::function<void()> hook;
            {
                std::lock_guard lock(mutex_);
                hook = lost_hook_;
            }
            if (hook) hook();
        } catch (...) {
        }
        try {
            if (vkGetDeviceFaultInfoEXT) report_fault();
        } catch (...) {
            std::fprintf(stderr, "[rt] device-fault: fault info collection failed\n");
        }
        try {
            std::lock_guard lock(mutex_);
            for (VkQueue queue : queues_) report_queue(queue);
        } catch (...) {
            std::fprintf(stderr, "[rt] device-fault: checkpoint collection failed\n");
        }
    }

    void report_fault() const {
        VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        VkResult result = vkGetDeviceFaultInfoEXT(device_, &counts, nullptr);
        if (result != VK_SUCCESS) {
            std::fprintf(stderr, "[rt] device-fault: vkGetDeviceFaultInfoEXT counts failed (%d)\n", int(result));
            return;
        }
        std::fprintf(stderr, "[rt] device-fault: vendor binary size %" PRIu64 " bytes\n", counts.vendorBinarySize);
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
        std::vector<std::byte> binary;
        // Keep collecting addresses and vendor records if the crash binary cannot be allocated.
        try {
            if (counts.vendorBinarySize <= binary.max_size())
                binary.resize(static_cast<std::size_t>(counts.vendorBinarySize));
        } catch (...) {
        }
        counts.vendorBinarySize = binary.size();
        VkDeviceFaultInfoEXT fault{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
        fault.pAddressInfos = addresses.data();
        fault.pVendorInfos = vendors.data();
        fault.pVendorBinaryData = binary.data();
        result = vkGetDeviceFaultInfoEXT(device_, &counts, &fault);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            std::fprintf(stderr, "[rt] device-fault: vkGetDeviceFaultInfoEXT data failed (%d)\n", int(result));
            return;
        }
        std::fprintf(stderr, "[rt] device-fault: description: %.*s%s\n", int(sizeof(fault.description)),
                     fault.description, result == VK_INCOMPLETE ? " (partial report)" : "");
        for (std::size_t i = 0; i < counts.addressInfoCount; ++i) {
            const auto& address = addresses[i];
            std::fprintf(stderr, "[rt] device-fault: address[%zu] type=%d address=0x%" PRIx64
                                 " precision=%" PRIu64 "\n",
                         i, int(address.addressType), address.reportedAddress, address.addressPrecision);
        }
        for (std::size_t i = 0; i < counts.vendorInfoCount; ++i) {
            const auto& vendor = vendors[i];
            std::fprintf(stderr, "[rt] device-fault: vendor[%zu] %.*s code=0x%" PRIx64 " data=0x%" PRIx64 "\n",
                         i, int(sizeof(vendor.description)), vendor.description, vendor.vendorFaultCode,
                         vendor.vendorFaultData);
        }
    }

    void report_queue(VkQueue queue) const {
        std::uint32_t count = 0;
        vkGetQueueCheckpointDataNV(queue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count, {VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        if (count) vkGetQueueCheckpointDataNV(queue, &count, data.data());
        std::fprintf(stderr, "[rt] device-fault: queue %p checkpoints=%u\n", static_cast<void*>(queue), count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto& checkpoint = data[i];
            const auto stage = static_cast<std::uint64_t>(checkpoint.stage);
            const auto marker = CheckpointMarker::decode(reinterpret_cast<std::uintptr_t>(checkpoint.pCheckpointMarker));
            if (!marker.submission) {
                std::fprintf(stderr, "[rt] device-fault: stage=0x%" PRIx64 " marker=%p (unrecognized)\n",
                             stage, checkpoint.pCheckpointMarker);
                continue;
            }
            std::fprintf(stderr, "[rt] device-fault: stage=0x%" PRIx64 " submission=%u list=%u command=%u kind=%s\n",
                         stage, marker.submission, marker.list, marker.command, marker.kind_name());
            const auto& entry = recent_[marker.submission % recent_.size()];
            if (entry.sequence != marker.submission || entry.queue != queue || marker.list >= entry.lists.size()) {
                std::fprintf(stderr, "[rt] device-fault: submission metadata unavailable\n");
                continue;
            }
            std::fprintf(stderr, "[rt] device-fault: ExecuteCommandLists #%" PRIu64 " queue=%p (%s) lists=%zu\n",
                         entry.execute_call, static_cast<void*>(entry.queue), entry.queue_kind, entry.list_count);
            const auto& shaders = entry.lists[marker.list].shaders;
            const auto next = std::upper_bound(shaders.begin(), shaders.end(), marker.command,
                                               [](auto command, const auto& change) { return command < change.command; });
            if (next != shaders.begin()) {
                const auto& pipeline = (next - 1)->pipeline;
                std::fprintf(stderr, "[rt] device-fault: last shader pipeline=0x%" PRIx64 " shaders=%s %s %s\n",
                             pipeline_handle(pipeline.handle), pipeline.shaders[0] ? pipeline.shaders[0] : "-",
                             pipeline.shaders[1] ? pipeline.shaders[1] : "-",
                             pipeline.shaders[2] ? pipeline.shaders[2] : "-");
            }
        }
        // LOCAL DIAGNOSTIC (post-load GPU faults): the work in flight is between the bottom-of-pipe checkpoint (last
        // command finished) and the top-of-pipe one (last command started) of the same list. Every pipeline bound in
        // that window, so the faulting program is among them; the dispatch ring above only keeps the last 64
        // dispatches of all queues, and those were from another queue in the fault this was added for.
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto top = CheckpointMarker::decode(reinterpret_cast<std::uintptr_t>(data[i].pCheckpointMarker));
            if (!top.submission || !(static_cast<std::uint64_t>(data[i].stage) & VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT))
                continue;
            std::uint32_t from = 0;  // no bottom-of-pipe checkpoint in this list: from its start
            for (std::uint32_t j = 0; j < count; ++j) {
                const auto bottom = CheckpointMarker::decode(reinterpret_cast<std::uintptr_t>(data[j].pCheckpointMarker));
                if (bottom.submission == top.submission && bottom.list == top.list &&
                    (static_cast<std::uint64_t>(data[j].stage) & VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT))
                    from = bottom.command;
            }
            const auto& entry = recent_[top.submission % recent_.size()];
            if (entry.sequence != top.submission || entry.queue != queue || top.list >= entry.lists.size()) continue;
            const auto& shaders = entry.lists[top.list].shaders;
            // The pipeline bound when `from` ran, then every change up to the top-of-pipe command.
            auto it = std::upper_bound(shaders.begin(), shaders.end(), from,
                                       [](auto command, const auto& change) { return command < change.command; });
            if (it != shaders.begin()) --it;
            for (; it != shaders.end() && it->command <= top.command; ++it)
                std::fprintf(stderr, "[rt] device-fault: in flight (submission %u list %u, commands %u..%u): command %u "
                                     "pipeline=0x%" PRIx64 " shaders=%s %s %s\n",
                             top.submission, top.list, from, top.command, unsigned(it->command),
                             pipeline_handle(it->pipeline.handle), it->pipeline.shaders[0] ? it->pipeline.shaders[0] : "-",
                             it->pipeline.shaders[1] ? it->pipeline.shaders[1] : "-",
                             it->pipeline.shaders[2] ? it->pipeline.shaders[2] : "-");
        }
    }

    template<typename Handle>
    static std::uint64_t pipeline_handle(Handle handle) {
        if constexpr (VK_USE_64_BIT_PTR_DEFINES) return reinterpret_cast<std::uintptr_t>(handle);
        else return handle;
    }

    VkDevice device_;
    PFN_vkGetDeviceFaultInfoEXT vkGetDeviceFaultInfoEXT = nullptr;
    PFN_vkCmdSetCheckpointNV vkCmdSetCheckpointNV = nullptr;
    PFN_vkGetQueueCheckpointDataNV vkGetQueueCheckpointDataNV = nullptr;
    PFN_vkGetSemaphoreCounterValue vkGetSemaphoreCounterValue = nullptr;
    const SubmitMutex* submit_lock_ = nullptr;
    std::array<SubmitRecord, 96> submits_{};  // under mutex_
    std::size_t submit_count_ = 0;
    unsigned stack_reports_ = 0;  // report_submit_lock_holder: the progress watch's thread only
    std::int64_t last_stack_report_ms_ = 0;
    VkSemaphore stalled_progress_ = VK_NULL_HANDLE;  // under read_mutex_
    std::uint64_t stalled_ticket_ = 0;
    std::atomic<bool> reported_{false};
    std::atomic<std::uint64_t> execute_calls_{0};
    std::uint32_t sequence_ = 0;
    // Held while a report reads the counters of the semaphores in the ring, and by forget before one is destroyed.
    // Before mutex_; the submit path never takes it.
    std::mutex read_mutex_;
    std::mutex mutex_;  // leaf lock, guards queues_ and recent_
    std::vector<VkQueue> queues_;
    const bool include_roots_ = [] {
        const char* value = std::getenv("LEONIDA_DEVICE_FAULT_ROOTS");
        return value && !std::strcmp(value, "1");
    }();
    std::array<CheckpointDispatch, CheckpointSubmission::dispatch_limit> dispatches_{};
    std::size_t dispatch_count_ = 0;
    std::atomic<std::uint64_t> dispatch_seq_{0};  // the last seq handed out (CheckpointRecorder::dispatch)
    std::unique_ptr<DeviceMarkers> markers_;      // LEONIDA_DEVICE_MARKERS=1 on a device with VK_AMD_buffer_marker
    std::unique_ptr<MarkerHistory> history_;      // with markers_, under mutex_
    std::array<CheckpointSubmission, 64> recent_;
};

}  // namespace leonida
