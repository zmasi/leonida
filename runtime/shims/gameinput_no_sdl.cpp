// gameinput.dll: GameInputCreate without SDL3.
//
// runtime/shims/gameinput.cpp serves the real thing through SDL3 and is only compiled when SDL3 is available. A build
// without SDL3 therefore left gameinput.dll unserved, the loader fell through to the host's gameinput.dll (wine ships
// one), and that host answers GameInputCreate with a null interface while the title ignores the HRESULT: the title's
// lazy singleton stores the null and calls AddRef/Release through it, which is a page fault at address 0.
//
// This file serves one of two objects, chosen on the first GameInputCreate:
//
//   - LEONIDA_INPUT_SCRIPT unset: an object that keeps the identity honest without any input backend. One static
//     object, slot 0/1/2 as QueryInterface/AddRef/Release, E_NOTIMPL for every other slot, so a caller that only
//     holds and releases the interface survives and one that asks for devices gets a clean refusal instead of a null
//     dereference.
//   - LEONIDA_INPUT_SCRIPT set: one scripted pad, driven only by the script (gameinput_script.h, the same parser and
//     timeline the SDL3 shim's keyboard pad uses). Without it a build without SDL3 has no device at all and the
//     title waits at its first "press A" prompt forever. The pad is an Xbox One gamepad that is connected from the
//     first GameInputCreate on: device enumeration through RegisterDeviceCallback (blocking, async, and through a
//     dispatcher), GetCurrentReading, GetNextReading and GetPreviousReading, and readings whose GetGamepadState and
//     GetUiNavigationState follow the script. Readings are taken when the title asks for one, so no thread runs,
//     but each one first adds a reading for every change the script made since the last, stamped with the script's
//     own time: a press that starts and ends between two reads still reaches the history for GetNextReading.
//     Reading callbacks, aggregate devices, rumble and every other device still need SDL3. Controllers and the
//     keyboard pad need SDL3 too.
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gameinput_abi.h"
#include "gameinput_no_sdl.h"
#include "gameinput_script.h"
#include "shims.h"

using namespace gi;
namespace script = rt::gameinput::script;

namespace {

// ---- no script: the identity-only object ------------------------------------------------------------------------

constexpr size_t kSlots = 64;  // comfortably covers the public v0 IGameInput vtable

struct GameInputStub {
    const void* const* vtable;
};

std::atomic<uint32_t> g_refs{1};

HRESULT __stdcall stub_qi(GameInputStub* self, const IID& riid, void** out);
ULONG __stdcall stub_add_ref(GameInputStub*);
ULONG __stdcall stub_release(GameInputStub*);

template <size_t Slot>
HRESULT __stdcall stub_method(GameInputStub*) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] gameinput: slot %zu is not served without SDL3; E_NOTIMPL (logged once)\n", Slot);
    return E_NOTIMPL;
}

template <size_t... Slot>
constexpr auto make_vtable(std::index_sequence<Slot...>) {
    return std::array<void*, sizeof...(Slot)>{
        (Slot == 0   ? reinterpret_cast<void*>(&stub_qi)
         : Slot == 1 ? reinterpret_cast<void*>(&stub_add_ref)
         : Slot == 2 ? reinterpret_cast<void*>(&stub_release)
                     : reinterpret_cast<void*>(&stub_method<Slot>))...};
}

const auto kVtable = make_vtable(std::make_index_sequence<kSlots>{});
GameInputStub g_gameinput{kVtable.data()};

HRESULT __stdcall stub_qi(GameInputStub*, const IID& riid, void** out) {
    if (!out) return E_POINTER;
    // An interface pointer is handed back for any IID: the measured call site holds this object only to AddRef and
    // Release it, and the other v0 interfaces answer E_NOTIMPL on their slots rather than faulting on a null.
    (void)riid;
    *out = &g_gameinput;
    g_refs.fetch_add(1);
    return S_OK;
}

ULONG __stdcall stub_add_ref(GameInputStub*) { return g_refs.fetch_add(1) + 1; }

ULONG __stdcall stub_release(GameInputStub*) { return g_refs.fetch_sub(1) - 1; }

// ---- LEONIDA_INPUT_SCRIPT: the scripted pad ---------------------------------------------------------------------

constexpr uint32_t kKinds =
    GameInputKindControllerAxis | GameInputKindControllerButton | GameInputKindGamepad | GameInputKindUiNavigation;
constexpr uint32_t kConnectedStatus = GameInputDeviceConnected | GameInputDeviceInputEnabled | GameInputDeviceOutputEnabled;
constexpr uint32_t kAxisCount = 6;     // LX, LY, RX, RY, LT, RT, as the SDL3 shim reports them
constexpr uint32_t kButtonCount = 15;  // SDL's standard gamepad buttons, numbered as the script's Button::sdl
constexpr size_t kHistoryCap = 64;     // readings kept for GetNextReading/GetPreviousReading; one per change
constexpr char kPadName[] = "Scripted pad (Leonida)";
std::atomic<bool> g_trace_readings{false};
std::atomic<uint64_t> g_gamepad_state_queries{0};

uint64_t qpc_now_us() {
    static const LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint64_t>(c.QuadPart / freq.QuadPart * 1'000'000 + c.QuadPart % freq.QuadPart * 1'000'000 / freq.QuadPart);
}
std::atomic<rt::gameinput_no_sdl::ClockFn> g_clock{&qpc_now_us};
uint64_t now_us() { return g_clock.load()(); }

bool same_guid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

ULONG release_floor(std::atomic<ULONG>& refs) {  // objects kept for the process: never freed
    ULONG r = refs.load();
    while (r > 1 && !refs.compare_exchange_weak(r, r - 1)) {}
    return r > 1 ? r - 1 : 1;
}

template <size_t Slot>
void not_served(const char* what) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] gameinput: slot %zu (%s) is not served without SDL3; E_NOTIMPL (logged once)\n",
                     Slot, what);
}

class Pad;
Pad& pad();

class Reading final : public IGameInputReading {
public:
    Reading(const GameInputGamepadState& state, uint64_t seq, uint64_t ts) : state_(state), seq_(seq), ts_(ts) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, IID_IUnknown) || same_guid(riid, IID_IGameInputReading)) {
            AddRef();
            *out = static_cast<IGameInputReading*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    uint32_t STDMETHODCALLTYPE GetInputKind() override {
        static std::atomic<uint64_t> queries{0};
        const uint64_t query = queries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_trace_readings.load(std::memory_order_relaxed) && query <= 8)
            std::fprintf(stderr, "[rt] gameinput: GetInputKind #%llu seq=%llu kinds=0x%08x\n",
                         static_cast<unsigned long long>(query), static_cast<unsigned long long>(seq_), kKinds);
        return kKinds;
    }
    uint64_t STDMETHODCALLTYPE GetSequenceNumber(uint32_t) override { return seq_; }
    uint64_t STDMETHODCALLTYPE GetTimestamp() override { return ts_; }
    void STDMETHODCALLTYPE GetDevice(IGameInputDevice** device) override;
    bool STDMETHODCALLTYPE GetRawReport(void** report) override {
        if (report) *report = nullptr;
        return false;
    }
    uint32_t STDMETHODCALLTYPE GetControllerAxisCount() override { return kAxisCount; }
    uint32_t STDMETHODCALLTYPE GetControllerAxisState(uint32_t count, float* out) override {
        if (!out) return 0;
        // 0..1 with 0.5 at rest for the sticks, as the SDL3 shim reports them.
        const float axes[kAxisCount] = {(state_.leftThumbstickX + 1) * 0.5f, (state_.leftThumbstickY + 1) * 0.5f,
                                        (state_.rightThumbstickX + 1) * 0.5f, (state_.rightThumbstickY + 1) * 0.5f,
                                        state_.leftTrigger, state_.rightTrigger};
        const uint32_t n = std::min(count, kAxisCount);
        std::memcpy(out, axes, n * sizeof(float));
        static std::atomic<uint64_t> queries{0};
        const uint64_t query = queries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_trace_readings.load(std::memory_order_relaxed) && query <= 8)
            std::fprintf(stderr, "[rt] gameinput: GetControllerAxisState #%llu seq=%llu count=%u returned=%u\n",
                         static_cast<unsigned long long>(query), static_cast<unsigned long long>(seq_), count, n);
        return n;
    }
    uint32_t STDMETHODCALLTYPE GetControllerButtonCount() override { return kButtonCount; }
    uint32_t STDMETHODCALLTYPE GetControllerButtonState(uint32_t count, bool* out) override {
        if (!out) return 0;
        bool buttons[kButtonCount] = {};
        for (const script::Button& b : script::kButtons) buttons[b.sdl] = (state_.buttons & b.bit) != 0;
        const uint32_t n = std::min(count, kButtonCount);
        std::memcpy(out, buttons, n * sizeof(bool));
        static std::atomic<uint64_t> queries{0};
        const uint64_t query = queries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_trace_readings.load(std::memory_order_relaxed) && query <= 8)
            std::fprintf(stderr, "[rt] gameinput: GetControllerButtonState #%llu seq=%llu count=%u returned=%u buttons=0x%08x\n",
                         static_cast<unsigned long long>(query), static_cast<unsigned long long>(seq_), count, n,
                         state_.buttons);
        return n;
    }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetControllerSwitchState(uint32_t, GameInputSwitchPosition*) override { return 0; }
    uint32_t STDMETHODCALLTYPE GetKeyCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetKeyState(uint32_t, void*) override { return 0; }
    bool STDMETHODCALLTYPE GetMouseState(void*) override { return false; }
    uint32_t STDMETHODCALLTYPE GetTouchCount() override { return 0; }
    uint32_t STDMETHODCALLTYPE GetTouchState(uint32_t, void*) override { return 0; }
    bool STDMETHODCALLTYPE GetMotionState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetArcadeStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetFlightStickState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetGamepadState(GameInputGamepadState* state) override {
        if (!state) return false;
        *state = state_;
        const uint64_t query = g_gamepad_state_queries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_trace_readings.load(std::memory_order_relaxed) && (query == 1 || state_.buttons || query % 1000 == 0))
            std::fprintf(stderr, "[rt] gameinput: GetGamepadState #%llu seq=%llu buttons=0x%08x\n",
                         static_cast<unsigned long long>(query), static_cast<unsigned long long>(seq_), state_.buttons);
        return true;
    }
    bool STDMETHODCALLTYPE GetRacingWheelState(void*) override { return false; }
    bool STDMETHODCALLTYPE GetUiNavigationState(GameInputUiNavigationState* state) override {
        if (!state) return false;
        state->buttons = script::ui_navigation(state_);
        static std::atomic<uint64_t> queries{0};
        const uint64_t query = queries.fetch_add(1, std::memory_order_relaxed) + 1;
        if (g_trace_readings.load(std::memory_order_relaxed) && query <= 8)
            std::fprintf(stderr, "[rt] gameinput: GetUiNavigationState #%llu seq=%llu buttons=0x%08x\n",
                         static_cast<unsigned long long>(query), static_cast<unsigned long long>(seq_), state->buttons);
        return true;
    }

    const GameInputGamepadState& state() const { return state_; }
    uint64_t seq() const { return seq_; }

private:
    std::atomic<ULONG> refs_{1};
    GameInputGamepadState state_;
    uint64_t seq_, ts_;
};

// The one device. It lives for the process and is connected from the first GameInputCreate on.
class Pad final : public IGameInputDevice {
public:
    Pad() {
        GameInputDeviceInfo& i = info_;
        i.infoSize = sizeof(GameInputDeviceInfo);
        i.usage = {0x01, 0x05};  // HID generic desktop / game pad
        std::memcpy(i.deviceId.value, "LEONIDA_SCRIPTED_PAD_DEVICE_ID\0\0", sizeof i.deviceId.value);
        i.deviceRootId = i.deviceId;
        i.deviceFamily = GameInputFamilyXboxOne;  // every pad is an Xbox One pad to the title, as in the SDL3 shim
        i.supportedInput = kKinds;
        i.controllerAxisCount = kAxisCount;
        i.controllerButtonCount = kButtonCount;
        for (uint32_t a = 0; a < kAxisCount; ++a) {
            axis_info_[a].mappedInputKinds = GameInputKindGamepad;
            axis_info_[a].isContinuous = true;
            axis_info_[a].hasRestValue = true;
            axis_info_[a].restValue = a < 4 ? 0.5f : 0.0f;
            axis_info_[a].resolution = 65536;
            axis_info_[a].legacyHidIndex = axis_info_[a].legacyDInputIndex = static_cast<uint16_t>(a);
        }
        axis_info_[4].label = GameInputLabelXboxLeftTrigger;
        axis_info_[5].label = GameInputLabelXboxRightTrigger;
        const GameInputLabel labels[kButtonCount] = {
            GameInputLabelXboxA,            GameInputLabelXboxB,          GameInputLabelXboxX,
            GameInputLabelXboxY,            GameInputLabelXboxView,       GameInputLabelXboxGuide,
            GameInputLabelXboxMenu,         GameInputLabelXboxLeftStickButton, GameInputLabelXboxRightStickButton,
            GameInputLabelXboxLeftShoulder, GameInputLabelXboxRightShoulder, GameInputLabelXboxDPadUp,
            GameInputLabelXboxDPadDown,     GameInputLabelXboxDPadLeft,   GameInputLabelXboxDPadRight};
        for (uint32_t b = 0; b < kButtonCount; ++b) {
            button_info_[b].mappedInputKinds = GameInputKindGamepad;
            button_info_[b].label = labels[b];
            button_info_[b].legacyHidIndex = button_info_[b].legacyDInputIndex = static_cast<uint16_t>(b);
        }
        gamepad_info_ = {GameInputLabelXboxMenu,        GameInputLabelXboxView,           GameInputLabelXboxA,
                         GameInputLabelXboxB,           GameInputLabelXboxX,              GameInputLabelXboxY,
                         GameInputLabelXboxDPadUp,      GameInputLabelXboxDPadDown,       GameInputLabelXboxDPadLeft,
                         GameInputLabelXboxDPadRight,   GameInputLabelXboxLeftShoulder,   GameInputLabelXboxRightShoulder,
                         GameInputLabelXboxLeftStickButton, GameInputLabelXboxRightStickButton};
        const GameInputLabel none = GameInputLabelNone;
        ui_info_ = {GameInputLabelXboxMenu,       GameInputLabelXboxView,          GameInputLabelXboxA,
                    GameInputLabelXboxB,          GameInputLabelXboxDPadUp,        GameInputLabelXboxDPadDown,
                    GameInputLabelXboxDPadLeft,   GameInputLabelXboxDPadRight,     GameInputLabelXboxX,
                    GameInputLabelXboxY,          GameInputLabelXboxLeftStickButton, GameInputLabelXboxRightStickButton,
                    GameInputLabelXboxLeftTrigger, GameInputLabelXboxRightTrigger, GameInputLabelXboxLeftShoulder,
                    GameInputLabelXboxRightShoulder, none, none, none, none, GameInputLabelXboxGuide};
        i.controllerAxisInfo = axis_info_;
        i.controllerButtonInfo = button_info_;
        i.gamepadInfo = &gamepad_info_;
        i.uiNavigationInfo = &ui_info_;
        constexpr uint32_t name_size = sizeof kPadName - 1;
        display_name_ = {name_size, name_size, kPadName};
        i.displayName = &display_name_;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, IID_IUnknown) || same_guid(riid, IID_IGameInputDevice)) {
            AddRef();
            *out = static_cast<IGameInputDevice*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return release_floor(refs_); }

    const GameInputDeviceInfo* STDMETHODCALLTYPE GetDeviceInfo() override {
        static std::atomic<bool> logged{false};
        if (g_trace_readings.load(std::memory_order_relaxed) && !logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] gameinput: GetDeviceInfo family=%d input=0x%08x status=0x%08x\n",
                         info_.deviceFamily, info_.supportedInput, kConnectedStatus);
        return &info_;
    }
    uint32_t STDMETHODCALLTYPE GetDeviceStatus() override { return kConnectedStatus; }
    void STDMETHODCALLTYPE GetBatteryState(GameInputBatteryState* state) override {
        if (state) *state = {0, 0, 0, 0, GameInputBatteryUnknown};
    }
    // No motors: the pad reports none, so every motor index is out of range.
    HRESULT STDMETHODCALLTYPE CreateForceFeedbackEffect(uint32_t, const void*, void** effect) override {
        if (effect) *effect = nullptr;
        return E_INVALIDARG;
    }
    bool STDMETHODCALLTYPE IsForceFeedbackMotorPoweredOn(uint32_t) override { return false; }
    void STDMETHODCALLTYPE SetForceFeedbackMotorGain(uint32_t, float) override {}
    HRESULT STDMETHODCALLTYPE SetHapticMotorState(uint32_t, const void*) override { return E_INVALIDARG; }
    void STDMETHODCALLTYPE SetRumbleState(const GameInputRumbleParams*) override {}
    void STDMETHODCALLTYPE SetInputSynchronizationState(bool) override {}
    void STDMETHODCALLTYPE SendInputSynchronizationHint() override {}
    void STDMETHODCALLTYPE PowerOff() override {}
    HRESULT STDMETHODCALLTYPE CreateRawDeviceReport(uint32_t, int32_t, void** report) override { return no_raw(report); }
    HRESULT STDMETHODCALLTYPE GetRawDeviceFeature(uint32_t, void** report) override { return no_raw(report); }
    HRESULT STDMETHODCALLTYPE SetRawDeviceFeature(void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutput(void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SendRawDeviceOutputWithResponse(void*, void** response) override { return no_raw(response); }
    HRESULT STDMETHODCALLTYPE ExecuteRawDeviceIoControl(uint32_t, size_t, const void*, size_t, void*, size_t* size) override {
        if (size) *size = 0;
        return E_NOTIMPL;
    }
    bool STDMETHODCALLTYPE AcquireExclusiveRawDeviceAccess(uint64_t) override { return false; }
    void STDMETHODCALLTYPE ReleaseExclusiveRawDeviceAccess() override {}

private:
    static HRESULT no_raw(void** out) {
        if (out) *out = nullptr;
        return E_NOTIMPL;
    }
    std::atomic<ULONG> refs_{1};
    GameInputDeviceInfo info_{};
    GameInputControllerAxisInfo axis_info_[kAxisCount]{};
    GameInputControllerButtonInfo button_info_[kButtonCount]{};
    GameInputGamepadInfo gamepad_info_{};
    GameInputUiNavigationInfo ui_info_{};
    GameInputString display_name_{};
};

Pad& pad() {
    static Pad* p = new Pad;
    return *p;
}

void Reading::GetDevice(IGameInputDevice** device) {
    if (!device) return;
    pad().AddRef();
    *device = &pad();
}

enum class CbType { Device, Guide, KeyboardLayout };

struct Callback {
    GameInputCallbackToken token;
    CbType type;
    void* context;
    void* fn;
    bool active;
};

// A device callback's enumeration report that is delivered later (async enumeration), by a one-shot thread or, when
// the title made a dispatcher, by IGameInputDispatcher::Dispatch on the title's thread.
struct Pending {
    GameInputCallbackToken token;
    uint64_t timestamp;
};

struct State {
    std::recursive_mutex mu;          // everything below
    std::recursive_mutex deliver_mu;  // held while callbacks run; UnregisterCallback waits on it
    std::atomic<DWORD> delivering_thread{0};
    script::Script script;
    uint64_t origin_us = 0;      // the script's clock starts at the first GameInputCreate
    double updated_s = -1;       // the script time of the last update; its changes up to here are in the history
    bool log_presses = true;     // LEONIDA_INPUT_LOG
    bool trace_reads = false;    // LEONIDA_INPUT_TRACE
    uint32_t logged_held = 0;    // what log_scripted saw last
    uint64_t reading_queries = 0;
    std::deque<Reading*> history;  // one reference each, oldest first; the newest is the current reading
    uint64_t evicted_seq = 0;      // sequence number of the newest reading dropped from history
    uint64_t seq = 0;
    std::vector<Callback> callbacks;
    std::vector<Pending> pending;
    GameInputCallbackToken next_token = 1;
    int dispatchers = 0;
    HANDLE wait_event = nullptr;  // auto-reset, signalled when a dispatcher has work
};

State& g() {
    static State* s = new State;  // never destroyed: a title thread may still read during exit
    return *s;
}

// Takes a reading when the script's state `seconds` into it differs from the current one. Under g().mu.
void take_reading(double seconds, uint64_t timestamp) {
    State& s = g();
    const script::Held held = script::held_at(s.script, seconds);
    GameInputGamepadState state{};
    state.buttons = held.buttons;
    state.rightThumbstickY = static_cast<float>(held.stick);
    const Reading* last = s.history.empty() ? nullptr : s.history.back();
    if (last && last->state().buttons == state.buttons && last->state().rightThumbstickY == state.rightThumbstickY)
        return;
    script::log_scripted(held.buttons, &s.logged_held, seconds);
    if (s.log_presses && last)
        script::log_presses(state.buttons & ~last->state().buttons, last->state().rightThumbstickY,
                            state.rightThumbstickY, seconds, kPadName);
    s.history.push_back(new Reading(state, ++s.seq, timestamp));
    while (s.history.size() > kHistoryCap) {
        s.evicted_seq = s.history.front()->seq();
        s.history.front()->Release();
        s.history.pop_front();
    }
}

// Brings the history up to `now`: one reading for every start and end of a press the script made since the last
// update, at the time the script made it, then the state at `now`. The title may read its pad only every few seconds
// (at world-start frame rates it does); a press that began and ended in between is still in the history, where
// GetNextReading finds it, and the readings are the same however often the title reads. Under g().mu.
void update(uint64_t now) {
    State& s = g();
    const double seconds = now > s.origin_us ? (now - s.origin_us) / 1e6 : 0.0;
    if (seconds > s.updated_s) {
        std::vector<double> edges;
        for (const script::Press& p : s.script.presses)
            for (const double edge : {p.at, p.at + p.hold})
                if (edge > s.updated_s && edge < seconds) edges.push_back(edge);
        std::sort(edges.begin(), edges.end());
        edges.erase(std::unique(edges.begin(), edges.end()), edges.end());
        for (const double edge : edges) take_reading(edge, s.origin_us + static_cast<uint64_t>(edge * 1e6 + 0.5));
        s.updated_s = seconds;
    }
    take_reading(seconds, now);
}

void trace_reading(State& s, const char* api, uint32_t kind, const Reading* reading) {
    const uint64_t query = ++s.reading_queries;
    const uint32_t buttons = reading ? reading->state().buttons : 0;
    if (s.trace_reads && (query <= 128 || buttons || query % 1000 == 0))
        std::fprintf(stderr, "[rt] gameinput: %s #%llu kind=0x%08x seq=%llu buttons=0x%08x\n", api,
                     static_cast<unsigned long long>(query), kind,
                     static_cast<unsigned long long>(reading ? reading->seq() : 0), buttons);
}

void call_device(const Callback& c, uint64_t timestamp) {
    if (g().trace_reads)
        std::fprintf(stderr, "[rt] gameinput: device callback token=%llu timestamp=%llu status=0x%08x\n",
                     static_cast<unsigned long long>(c.token), static_cast<unsigned long long>(timestamp), kConnectedStatus);
    reinterpret_cast<GameInputDeviceCallback>(c.fn)(c.token, c.context, &pad(), timestamp, kConnectedStatus,
                                                    GameInputDeviceNoStatus);
}

// Runs the pending enumeration reports on the calling thread, without holding the state lock.
void deliver() {
    State& s = g();
    std::lock_guard dl(s.deliver_mu);
    std::vector<Pending> batch;
    {
        std::lock_guard lock(s.mu);
        batch.swap(s.pending);
    }
    s.delivering_thread = GetCurrentThreadId();
    for (const Pending& p : batch) {
        Callback target{};
        {
            std::lock_guard lock(s.mu);
            for (const Callback& c : s.callbacks)
                if (c.token == p.token && c.active) target = c;
        }
        if (target.fn) call_device(target, p.timestamp);
    }
    s.delivering_thread = 0;
}

// Async enumeration: the report arrives after RegisterDeviceCallback returns, as GameInput delivers it from a thread
// of its own; with a dispatcher, the title's Dispatch delivers it instead.
void schedule_delivery() {
    State& s = g();
    {
        std::lock_guard lock(s.mu);
        if (s.dispatchers > 0) {
            SetEvent(s.wait_event);
            return;
        }
    }
    std::thread([] {
        {
            std::lock_guard lock(g().mu);
            if (g().dispatchers > 0) {  // a dispatcher appeared meanwhile: it owns the delivery
                SetEvent(g().wait_event);
                return;
            }
        }
        deliver();
    }).detach();
}

class Dispatcher final : public IGameInputDispatcher {
public:
    Dispatcher() {
        std::lock_guard lock(g().mu);
        ++g().dispatchers;
        if (!g().pending.empty()) SetEvent(g().wait_event);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, IID_IUnknown) || same_guid(riid, IID_IGameInputDispatcher)) {
            AddRef();
            *out = static_cast<IGameInputDispatcher*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --refs_;
        if (r == 0) {
            {
                std::lock_guard lock(g().mu);
                --g().dispatchers;
            }
            delete this;
        }
        return r;
    }
    // The pending reports are few (one per async enumeration), so all of them run whatever the quota.
    bool STDMETHODCALLTYPE Dispatch(uint64_t) override {
        deliver();
        std::lock_guard lock(g().mu);
        return !g().pending.empty();
    }
    HRESULT STDMETHODCALLTYPE OpenWaitHandle(HANDLE* handle) override {
        if (!handle) return E_POINTER;
        *handle = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), g().wait_event, GetCurrentProcess(), handle, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return HRESULT_FROM_WIN32(GetLastError());
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{1};
};

// GetNextReading (forward) and GetPreviousReading: the reading right after or right before the reference. A step
// across a reading dropped from the history is REFERENCE_READING_TOO_OLD, as in the SDL3 shim.
HRESULT find_reading(IGameInputReading* ref, uint32_t kind, IGameInputReading** out, bool forward) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!ref) return E_INVALIDARG;
    if (kind != GameInputKindUnknown && !(kind & kKinds)) return GAMEINPUT_E_READING_NOT_FOUND;
    State& s = g();
    std::lock_guard lock(s.mu);
    update(now_us());
    const uint64_t r = static_cast<Reading*>(ref)->seq();
    trace_reading(s, forward ? "GetNextReading" : "GetPreviousReading", kind,
                  s.history.empty() ? nullptr : s.history.back());
    if (forward ? s.evicted_seq > r : s.evicted_seq >= r) return GAMEINPUT_E_REFERENCE_READING_TOO_OLD;
    Reading* best = nullptr;
    for (Reading* c : s.history) {
        if (forward && c->seq() > r) {
            best = c;
            break;
        }
        if (!forward && c->seq() < r) best = c;
    }
    if (!best) return GAMEINPUT_E_READING_NOT_FOUND;
    best->AddRef();
    *out = best;
    return S_OK;
}

bool is_pad(IGameInputDevice* device) { return device == static_cast<IGameInputDevice*>(&pad()); }

class ScriptedInput final : public IGameInput {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (same_guid(riid, IID_IUnknown) || same_guid(riid, IID_IGameInput)) {
            AddRef();
            *out = static_cast<IGameInput*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { return release_floor(refs_); }  // a process-wide singleton

    uint64_t STDMETHODCALLTYPE GetCurrentTimestamp() override { return now_us(); }

    HRESULT STDMETHODCALLTYPE GetCurrentReading(uint32_t kind, IGameInputDevice* device, IGameInputReading** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (kind != GameInputKindUnknown && !(kind & kKinds)) return GAMEINPUT_E_READING_NOT_FOUND;
        if (device && !is_pad(device)) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        State& s = g();
        std::lock_guard lock(s.mu);
        update(now_us());
        Reading* r = s.history.back();
        trace_reading(s, "GetCurrentReading", kind, r);
        r->AddRef();
        *out = r;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNextReading(IGameInputReading* ref, uint32_t kind, IGameInputDevice* device,
                                             IGameInputReading** out) override {
        if (device && !is_pad(device)) return out ? (*out = nullptr, GAMEINPUT_E_DEVICE_NOT_FOUND) : E_POINTER;
        return find_reading(ref, kind, out, true);
    }
    HRESULT STDMETHODCALLTYPE GetPreviousReading(IGameInputReading* ref, uint32_t kind, IGameInputDevice* device,
                                                 IGameInputReading** out) override {
        if (device && !is_pad(device)) return out ? (*out = nullptr, GAMEINPUT_E_DEVICE_NOT_FOUND) : E_POINTER;
        return find_reading(ref, kind, out, false);
    }
    // GameInput v0's own answer (see the SDL3 shim).
    HRESULT STDMETHODCALLTYPE GetTemporalReading(uint64_t, IGameInputDevice*, IGameInputReading** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        return E_NOTIMPL;
    }
    // Readings are taken when the title asks for one, so there is nothing to call back with.
    HRESULT STDMETHODCALLTYPE RegisterReadingCallback(IGameInputDevice*, uint32_t, float, void*, GameInputReadingCallback,
                                                      GameInputCallbackToken* token) override {
        if (token) *token = kInvalidCallbackToken;
        not_served<8>("RegisterReadingCallback");
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE RegisterDeviceCallback(IGameInputDevice* device, uint32_t kind, uint32_t status_filter,
                                                     GameInputEnumerationKind enumeration, void* context,
                                                     GameInputDeviceCallback fn, GameInputCallbackToken* token) override {
        if (token) *token = kInvalidCallbackToken;
        if (!fn) return E_INVALIDARG;
        State& s = g();
        Callback c{0, CbType::Device, context, reinterpret_cast<void*>(fn), true};
        // The pad is connected already, so only an enumeration reports it: there is no later connection event.
        const bool report = enumeration != GameInputNoEnumeration && (!device || is_pad(device)) &&
                            (kind == GameInputKindUnknown || (kind & kKinds)) && (kConnectedStatus & status_filter);
        bool later = false;
        {
            std::lock_guard lock(s.mu);
            c.token = s.next_token++;
            s.callbacks.push_back(c);
            if (report && enumeration != GameInputBlockingEnumeration) {
                s.pending.push_back({c.token, now_us()});
                later = true;
            }
        }
        if (s.trace_reads)
            std::fprintf(stderr,
                         "[rt] gameinput: RegisterDeviceCallback kind=0x%08x filter=0x%08x enumeration=%d report=%d\n",
                         kind, status_filter, static_cast<int>(enumeration), report ? 1 : 0);
        if (token) *token = c.token;
        // Blocking enumeration: the pad is reported before this returns, on the caller's thread. Under deliver_mu and
        // marked as delivering, as deliver() runs callbacks, so an UnregisterCallback on another thread waits for it
        // to return and one from inside it does not wait on itself.
        if (report && enumeration == GameInputBlockingEnumeration) {
            std::lock_guard dl(s.deliver_mu);
            const DWORD outer = s.delivering_thread.exchange(GetCurrentThreadId());
            call_device(c, now_us());
            s.delivering_thread = outer;
        }
        if (later) schedule_delivery();
        return S_OK;
    }
    // The script has no guide button and the pad no keyboard layout: these are accepted and never run.
    HRESULT STDMETHODCALLTYPE RegisterGuideButtonCallback(IGameInputDevice*, void* context, GameInputGuideButtonCallback fn,
                                                          GameInputCallbackToken* token) override {
        return add(CbType::Guide, context, reinterpret_cast<void*>(fn), token);
    }
    HRESULT STDMETHODCALLTYPE RegisterKeyboardLayoutCallback(IGameInputDevice*, void* context,
                                                             GameInputKeyboardLayoutCallback fn,
                                                             GameInputCallbackToken* token) override {
        return add(CbType::KeyboardLayout, context, reinterpret_cast<void*>(fn), token);
    }
    void STDMETHODCALLTYPE StopCallback(GameInputCallbackToken token) override {
        std::lock_guard lock(g().mu);
        for (Callback& c : g().callbacks)
            if (c.token == token) c.active = false;
    }
    // Removes the callback and, unless called from inside a callback, waits for one that is running to return.
    bool STDMETHODCALLTYPE UnregisterCallback(GameInputCallbackToken token, uint64_t) override {
        State& s = g();
        bool found = false;
        {
            std::lock_guard lock(s.mu);
            auto it = std::find_if(s.callbacks.begin(), s.callbacks.end(),
                                   [token](const Callback& c) { return c.token == token; });
            if (it != s.callbacks.end()) {
                s.callbacks.erase(it);
                found = true;
            }
        }
        if (found && s.delivering_thread != GetCurrentThreadId()) std::lock_guard wait(s.deliver_mu);
        return found;
    }
    HRESULT STDMETHODCALLTYPE CreateDispatcher(IGameInputDispatcher** out) override {
        if (!out) return E_POINTER;
        *out = new Dispatcher;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CreateAggregateDevice(uint32_t, IGameInputDevice** out) override {
        if (out) *out = nullptr;
        not_served<15>("CreateAggregateDevice");
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromId(const AppLocalDeviceId* id, IGameInputDevice** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!id) return E_INVALIDARG;
        if (std::memcmp(id, &pad().GetDeviceInfo()->deviceId, sizeof *id) != 0) return GAMEINPUT_E_DEVICE_NOT_FOUND;
        pad().AddRef();
        *out = &pad();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE FindDeviceFromObject(IUnknown*, IGameInputDevice** out) override { return not_found(out); }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformHandle(HANDLE, IGameInputDevice** out) override { return not_found(out); }
    HRESULT STDMETHODCALLTYPE FindDeviceFromPlatformString(LPCWSTR, IGameInputDevice** out) override { return not_found(out); }
    HRESULT STDMETHODCALLTYPE EnableOemDeviceSupport(uint16_t, uint16_t, uint8_t, uint8_t) override { return S_OK; }
    void STDMETHODCALLTYPE SetFocusPolicy(uint32_t) override {}

private:
    static HRESULT not_found(IGameInputDevice** out) {
        if (out) *out = nullptr;
        return GAMEINPUT_E_DEVICE_NOT_FOUND;
    }
    static HRESULT add(CbType type, void* context, void* fn, GameInputCallbackToken* token) {
        if (token) *token = kInvalidCallbackToken;
        if (!fn) return E_INVALIDARG;
        std::lock_guard lock(g().mu);
        const GameInputCallbackToken t = g().next_token++;
        g().callbacks.push_back({t, type, context, fn, true});
        if (token) *token = t;
        return S_OK;
    }
    std::atomic<ULONG> refs_{1};
};

// The scripted object when LEONIDA_INPUT_SCRIPT is set, read once on the first call; null otherwise.
ScriptedInput* scripted() {
    static ScriptedInput* const input = []() -> ScriptedInput* {
        const std::string text = script::env_lower("LEONIDA_INPUT_SCRIPT");
        if (text.empty()) return nullptr;
        State& s = g();
        std::lock_guard lock(s.mu);
        s.script = script::parse(text);
        s.log_presses = script::env_lower("LEONIDA_INPUT_LOG") != "0";
        s.trace_reads = script::env_lower("LEONIDA_INPUT_TRACE") == "1";
        g_trace_readings = s.trace_reads;
        s.wait_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        s.origin_us = now_us();
        update(s.origin_us);  // the first reading: nothing held unless the script presses at 0 s
        std::fprintf(stderr,
                     "[rt] gameinput: served without SDL3: one scripted pad for LEONIDA_INPUT_SCRIPT (%zu scripted "
                     "press(es), %zu rejected); controllers and the keyboard pad need SDL3\n",
                     s.script.presses.size(), s.script.rejected);
        return new ScriptedInput;
    }();
    return input;
}

HRESULT WINAPI GameInputCreate(void** out) {
    if (!out) return E_POINTER;
    if (ScriptedInput* input = scripted()) {
        input->AddRef();
        *out = static_cast<IGameInput*>(input);
        return S_OK;
    }
    g_refs.fetch_add(1);
    *out = &g_gameinput;
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] gameinput: served without SDL3 (no devices; E_NOTIMPL for every other slot)\n");
    return S_OK;
}

}  // namespace

namespace rt::gameinput_no_sdl {
void set_clock(ClockFn now) { g_clock.store(now ? now : &qpc_now_us); }
}  // namespace rt::gameinput_no_sdl

SHIM_TABLE(gameinput_no_sdl,
    {"gameinput.dll", "GameInputCreate", reinterpret_cast<void*>(&GameInputCreate)},
    // The retail executable imports this one by ordinal, so the registry key is "#1"; without the alias the loader
    // fell through to the host's gameinput.dll, which handed the title a null interface.
    {"gameinput.dll", "#1", reinterpret_cast<void*>(&GameInputCreate)},
);
