// gameinput.dll without SDL3 (shims/gameinput_no_sdl.cpp). No controller, no game data.
//
//   gameinput_no_sdl_test                 LEONIDA_INPUT_SCRIPT unset: the identity-only object. The retail executable
//                                         imports GameInputCreate by ordinal, and a build without SDL3 still has to
//                                         hand back an interface instead of letting the loader fall through to the
//                                         host's gameinput.dll, which answers with a null pointer the title
//                                         dereferences.
//   gameinput_no_sdl_test --input-script  LEONIDA_INPUT_SCRIPT set: the scripted pad, found through device callbacks
//                                         and read at chosen times of a fake clock.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gameinput_abi.h"
#include "gameinput_no_sdl.h"
#include "shims.h"

using namespace gi;

namespace {
int g_failed = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok   " : "FAIL ", what);
    if (!ok) ++g_failed;
}

using CreateFn = HRESULT(WINAPI*)(void**);

int identity_only(CreateFn create) {
    void* object = nullptr;
    check(create(&object) == S_OK, "GameInputCreate returns S_OK");
    check(object != nullptr, "GameInputCreate hands back a non-null interface");
    if (!object) return 1;

    auto* vtable = *static_cast<void* const* const*>(object);
    auto add_ref = reinterpret_cast<ULONG(__stdcall*)(void*)>(vtable[1]);
    auto release = reinterpret_cast<ULONG(__stdcall*)(void*)>(vtable[2]);
    check(add_ref(object) >= 1, "slot 1 (AddRef) is callable and counts");
    check(release(object) >= 0, "slot 2 (Release) is callable and does not underflow");

    // A device query answers a clean refusal through the same object rather than faulting.
    auto get_devices = reinterpret_cast<HRESULT(__stdcall*)(void*, uint32_t, void**)>(vtable[9]);
    void* devices = reinterpret_cast<void*>(0x1);
    check(get_devices(object, 0, &devices) == E_NOTIMPL, "an unserved slot returns E_NOTIMPL");
    check(devices == reinterpret_cast<void*>(0x1), "an unserved slot writes nothing to its out-parameter");

    check(create(nullptr) == E_POINTER, "a null out parameter is E_POINTER");
    return 0;
}

// ---- the scripted pad ----

uint64_t g_now_us = 7'000'000;  // the fake clock; the script's clock starts at GameInputCreate
uint64_t fake_clock() { return g_now_us; }
void at(double seconds) { g_now_us = 7'000'000 + static_cast<uint64_t>(seconds * 1e6 + 0.5); }

struct Seen {
    int calls = 0;
    IGameInputDevice* device = nullptr;
    uint32_t current = 0, previous = 0xFFFFFFFF;
    DWORD thread = 0;
    HANDLE done = nullptr;
};
void CALLBACK on_device(GameInputCallbackToken, void* context, IGameInputDevice* device, uint64_t, uint32_t current,
                        uint32_t previous) {
    auto* seen = static_cast<Seen*>(context);
    ++seen->calls;
    seen->device = device;
    seen->current = current;
    seen->previous = previous;
    seen->thread = GetCurrentThreadId();
    if (seen->done) SetEvent(seen->done);
}

struct Pad {
    uint32_t buttons = 0xFFFFFFFF;
    float right_y = -2;
    uint32_t ui = 0xFFFFFFFF;
    uint64_t seq = 0;
    bool a_button = false;
};
Pad read_at(IGameInput* input, IGameInputDevice* device, double seconds) {
    at(seconds);
    Pad p;
    IGameInputReading* r = nullptr;
    if (input->GetCurrentReading(GameInputKindGamepad, device, &r) != S_OK || !r) return p;
    GameInputGamepadState s{};
    if (r->GetGamepadState(&s)) p.buttons = s.buttons, p.right_y = s.rightThumbstickY;
    GameInputUiNavigationState u{};
    if (r->GetUiNavigationState(&u)) p.ui = u.buttons;
    p.seq = r->GetSequenceNumber(GameInputKindGamepad);
    bool buttons[15] = {};
    if (r->GetControllerButtonState(15, buttons) == 15) p.a_button = buttons[0];
    r->Release();
    return p;
}

int scripted_pad(CreateFn create) {
    // Six presses (two overlapping A presses, Menu, B held 0.8 s, a right-stick throw, a Y press the title never reads
    // while it is down) and seven rejected entries (four of them holds that are zero, negative, missing or too long).
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT",
                            "a@1, START@2.5 ,a@1.2,b@3:0.8 ,rsup@4,y@10,bogus@1,a@-1,,x@3:0,x@3:-1,x@3:,x@3:601");
    rt::gameinput_no_sdl::set_clock(fake_clock);
    at(0);

    IGameInput* input = nullptr;
    check(create(reinterpret_cast<void**>(&input)) == S_OK && input, "script: GameInputCreate returns an instance");
    if (!input) return 1;
    IUnknown* unk = nullptr;
    check(input->QueryInterface(IID_IGameInput, reinterpret_cast<void**>(&unk)) == S_OK && unk == input,
          "script: QueryInterface(IID_IGameInput) is the same object");
    if (unk) unk->Release();
    check(input->GetCurrentTimestamp() == g_now_us, "script: GetCurrentTimestamp reads the shim's clock");

    // Device callbacks: a blocking enumeration reports the pad before it returns, on this thread.
    Seen blocking;
    GameInputCallbackToken token = 0;
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected,
                                        GameInputBlockingEnumeration, &blocking, on_device, &token) == S_OK &&
              token != kInvalidCallbackToken,
          "script: RegisterDeviceCallback (blocking) is served");
    check(blocking.calls == 1 && blocking.device && blocking.thread == GetCurrentThreadId(),
          "script: blocking enumeration reports one device on the caller's thread");
    check((blocking.current & GameInputDeviceConnected) && blocking.previous == GameInputDeviceNoStatus,
          "script: the device is reported connected, from no status");
    IGameInputDevice* device = blocking.device;
    if (!device) return 1;
    const GameInputDeviceInfo* info = device->GetDeviceInfo();
    check(info && info->deviceFamily == GameInputFamilyXboxOne && (info->supportedInput & GameInputKindGamepad),
          "script: the device is an Xbox One gamepad");
    check(info && info->gamepadInfo && info->gamepadInfo->aButtonLabel == GameInputLabelXboxA,
          "script: the A button carries the Xbox A label");
    check(info && info->displayName && info->displayName->sizeInBytes > 0, "script: the device has a display name");
    check(device->GetDeviceStatus() & GameInputDeviceConnected, "script: GetDeviceStatus says connected");
    IGameInputDevice* found = nullptr;
    check(info && input->FindDeviceFromId(&info->deviceId, &found) == S_OK && found == device,
          "script: FindDeviceFromId finds the pad");
    if (found) found->Release();
    check(input->UnregisterCallback(token, 0), "script: the blocking callback unregisters");

    // No enumeration, or a status filter the pad does not match: nothing is reported.
    Seen quiet;
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputNoEnumeration,
                                        &quiet, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback without enumeration is served");
    input->UnregisterCallback(token, 0);
    check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceWireless,
                                        GameInputBlockingEnumeration, &quiet, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback with another status filter is served");
    input->UnregisterCallback(token, 0);
    check(input->RegisterDeviceCallback(nullptr, GameInputKindKeyboard, GameInputDeviceAnyStatus,
                                        GameInputBlockingEnumeration, &quiet, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback for keyboards is served");
    input->UnregisterCallback(token, 0);
    check(quiet.calls == 0, "script: no report without enumeration, for another status or another kind");

    // Async enumeration: reported after the call, from another thread.
    Seen async;
    async.done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(input->RegisterDeviceCallback(nullptr, GameInputKindUnknown, GameInputDeviceAnyStatus,
                                        GameInputAsyncEnumeration, &async, on_device, &token) == S_OK,
          "script: RegisterDeviceCallback (async) is served");
    check(WaitForSingleObject(async.done, 5000) == WAIT_OBJECT_0 && async.calls == 1 && async.device == device,
          "script: async enumeration reports the pad");
    check(async.thread != GetCurrentThreadId(), "script: async enumeration reports it from another thread");
    input->UnregisterCallback(token, 0);

    // With a dispatcher, the title's Dispatch delivers the report on its own thread.
    IGameInputDispatcher* dispatcher = nullptr;
    check(input->CreateDispatcher(&dispatcher) == S_OK && dispatcher, "script: CreateDispatcher is served");
    if (dispatcher) {
        HANDLE wait = nullptr;
        check(dispatcher->OpenWaitHandle(&wait) == S_OK && wait, "script: the dispatcher has a wait handle");
        Seen dispatched;
        check(input->RegisterDeviceCallback(nullptr, GameInputKindGamepad, GameInputDeviceConnected,
                                            GameInputAsyncEnumeration, &dispatched, on_device, &token) == S_OK,
              "script: RegisterDeviceCallback (async, with a dispatcher) is served");
        check(wait && WaitForSingleObject(wait, 5000) == WAIT_OBJECT_0, "script: the wait handle is signalled");
        check(dispatched.calls == 0, "script: nothing is delivered before Dispatch");
        check(!dispatcher->Dispatch(0) && dispatched.calls == 1 && dispatched.thread == GetCurrentThreadId(),
              "script: Dispatch delivers the report on the caller's thread");
        input->UnregisterCallback(token, 0);
        if (wait) CloseHandle(wait);
        dispatcher->Release();
    }

    // Readings follow the script's timeline: each press is held for 0.3 s, then released.
    const Pad start = read_at(input, nullptr, 0.0);
    check(start.buttons == 0 && start.right_y == 0, "script: nothing is held at 0 s");
    const Pad idle = read_at(input, device, 0.9);
    check(idle.buttons == 0 && idle.seq == start.seq, "script: no new reading while nothing changes");
    const Pad a = read_at(input, device, 1.0);
    check(a.buttons == GameInputGamepadA && a.a_button && a.seq > idle.seq, "script: A is held at 1.0 s");
    check(a.ui == GameInputUiNavigationAccept, "script: A reads as UI Accept");
    check(read_at(input, device, 1.25).buttons == GameInputGamepadA, "script: overlapping A presses hold A at 1.25 s");
    check(read_at(input, device, 1.45).buttons == GameInputGamepadA,
          "script: a later press of the same button is not cut by the earlier one's release");
    const Pad released = read_at(input, device, 1.55);
    check(released.buttons == 0 && !released.a_button, "script: A is released after its last 0.3 s hold");
    const Pad menu = read_at(input, device, 2.6);
    check(menu.buttons == GameInputGamepadMenu && menu.ui == GameInputUiNavigationMenu,
          "script: START (any case) holds Menu at 2.6 s");
    check(read_at(input, device, 2.85).buttons == 0, "script: Menu is released at 2.85 s");
    check(read_at(input, device, 3.05).buttons == GameInputGamepadB, "script: b@3:0.8 holds B at 3.05 s");
    check(read_at(input, device, 3.7).buttons == GameInputGamepadB,
          "script: B is still held at 3.7 s, past the 0.3 s default");
    check(read_at(input, device, 3.85).buttons == 0, "script: B is released after its 0.8 s hold");
    const Pad up = read_at(input, device, 4.1);
    check(up.buttons == 0 && up.right_y == 1.0f && up.ui == GameInputUiNavigationScrollUp,
          "script: rsup throws the right stick up at 4.1 s");
    check(read_at(input, device, 4.5).right_y == 0, "script: the stick is back at rest at 4.5 s");
    // The title reads at 4.5 s and next at 60 s: y@10 starts and ends in between, and is still in the history.
    check(read_at(input, device, 60).buttons == 0, "script: nothing is held after the script ends");

    // The history, newest first: the rest after Y (10.3 s), the Y press (10.0 s), the rest after the stick throw
    // (4.3 s), the stick throw. Each reading carries the script's time of the change, not the time of the read.
    IGameInputReading* now = nullptr;
    check(input->GetCurrentReading(GameInputKindUnknown, nullptr, &now) == S_OK && now, "script: a current reading");
    if (now) {
        check(now->GetTimestamp() == 7'000'000 + 10'300'000, "script: the current reading is Y's release at 10.3 s");
        IGameInputReading* y = nullptr;
        IGameInputReading* rest = nullptr;
        IGameInputReading* stick = nullptr;
        IGameInputReading* after = nullptr;
        GameInputGamepadState s{};
        check(input->GetPreviousReading(now, GameInputKindGamepad, device, &y) == S_OK && y &&
                  y->GetGamepadState(&s) && s.buttons == GameInputGamepadY &&
                  y->GetTimestamp() == 7'000'000 + 10'000'000,
              "script: a press between two reads of the title is in the history, at its own time");
        if (y) {
            check(input->GetPreviousReading(y, GameInputKindGamepad, device, &rest) == S_OK && rest &&
                      rest->GetGamepadState(&s) && s.buttons == 0 && s.rightThumbstickY == 0 &&
                      rest->GetTimestamp() == 7'000'000 + 4'300'000,
                  "script: before Y, the stick's return at 4.3 s");
            check(input->GetNextReading(y, GameInputKindGamepad, nullptr, &after) == S_OK && after == now,
                  "script: GetNextReading steps forward from Y to the current reading");
            y->Release();
        }
        if (rest) {
            check(input->GetPreviousReading(rest, GameInputKindGamepad, device, &stick) == S_OK && stick &&
                      stick->GetGamepadState(&s) && s.rightThumbstickY == 1.0f,
                  "script: GetPreviousReading steps back to the stick throw");
            rest->Release();
        }
        if (stick) stick->Release();
        if (after) after->Release();
        IGameInputReading* none = nullptr;
        check(input->GetNextReading(now, GameInputKindUnknown, nullptr, &none) == GAMEINPUT_E_READING_NOT_FOUND &&
                  !none,
              "script: nothing after the current reading");
        IGameInputDevice* of = nullptr;
        now->GetDevice(&of);
        check(of == device, "script: a reading belongs to the pad");
        if (of) of->Release();
        now->Release();
    }

    IGameInputReading* motion = nullptr;
    check(input->GetCurrentReading(GameInputKindMotion, nullptr, &motion) == GAMEINPUT_E_READING_NOT_FOUND && !motion,
          "script: the pad has no motion readings");
    GameInputCallbackToken reading_token = 1;
    check(input->RegisterReadingCallback(nullptr, GameInputKindGamepad, 0, nullptr, nullptr, &reading_token) ==
                  E_NOTIMPL &&
              reading_token == kInvalidCallbackToken,
          "script: reading callbacks still need SDL3");
    // A blocking enumeration's callback still running on one thread while another unregisters it (kevincommits'
    // Finding 19 in #1608, item 5): UnregisterCallback must wait for it to return, or the caller frees its context
    // mid-call. And the callback may unregister itself without waiting on itself.
    {
        struct Held {
            IGameInput* input;
            HANDLE entered, release;
            GameInputCallbackToken token;
            bool self_unregistered;
        };
        static Held held{};
        held = {input, CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr), 0, false};
        HANDLE registrar = CreateThread(
            nullptr, 0,
            [](void*) -> DWORD {
                GameInputCallbackToken t = 0;
                held.input->RegisterDeviceCallback(
                    nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputBlockingEnumeration, &held,
                    [](GameInputCallbackToken token, void* ctx, IGameInputDevice*, uint64_t, uint32_t, uint32_t) {
                        auto* h = static_cast<Held*>(ctx);
                        h->token = token;
                        SetEvent(h->entered);
                        WaitForSingleObject(h->release, 3000);
                    },
                    &t);
                return 0;
            },
            nullptr, 0, nullptr);
        check(WaitForSingleObject(held.entered, 3000) == WAIT_OBJECT_0, "script: the blocking callback is running");
        HANDLE unregistrar = CreateThread(
            nullptr, 0, [](void*) -> DWORD { return held.input->UnregisterCallback(held.token, 0) ? 1 : 0; }, nullptr,
            0, nullptr);
        check(WaitForSingleObject(unregistrar, 200) == WAIT_TIMEOUT,
              "script: UnregisterCallback from another thread waits for the running blocking callback");
        SetEvent(held.release);
        DWORD unregistered = 0;
        check(WaitForSingleObject(unregistrar, 3000) == WAIT_OBJECT_0 && GetExitCodeThread(unregistrar, &unregistered) &&
                  unregistered == 1,
              "script: and returns true once it has returned");
        WaitForSingleObject(registrar, 3000);
        CloseHandle(registrar);
        CloseHandle(unregistrar);

        GameInputCallbackToken self = 0;
        check(input->RegisterDeviceCallback(
                  nullptr, GameInputKindGamepad, GameInputDeviceConnected, GameInputBlockingEnumeration, &held,
                  [](GameInputCallbackToken token, void* ctx, IGameInputDevice*, uint64_t, uint32_t, uint32_t) {
                      auto* h = static_cast<Held*>(ctx);
                      h->self_unregistered = h->input->UnregisterCallback(token, 0);
                  },
                  &self) == S_OK &&
                  held.self_unregistered,
              "script: a blocking callback can unregister itself");
        CloseHandle(held.entered);
        CloseHandle(held.release);
    }
    void* second = nullptr;
    check(create(&second) == S_OK && second == input, "script: a second GameInputCreate returns the same instance");
    if (second) static_cast<IGameInput*>(second)->Release();
    input->Release();
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    auto create = reinterpret_cast<CreateFn>(rt::shims::find("gameinput.dll", "#1"));
    auto named = reinterpret_cast<CreateFn>(rt::shims::find("gameinput.dll", "GameInputCreate"));
    check(create != nullptr, "the ordinal import gameinput.dll!#1 is served");
    check(named != nullptr, "the name GameInputCreate is served");
    if (!create) return 1;

    const bool with_script = argc == 2 && std::strcmp(argv[1], "--input-script") == 0;
    SetEnvironmentVariableA("LEONIDA_INPUT_SCRIPT", nullptr);
    SetEnvironmentVariableA("LEONIDA_INPUT_LOG", nullptr);
    const int early = with_script ? scripted_pad(create) : identity_only(create);
    std::printf("%d failure(s)\n", g_failed);
    return early || g_failed ? 1 : 0;
}
