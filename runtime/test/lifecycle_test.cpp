// Checks the console lifecycle shim (runtime/shims/lifecycle.cpp): app-state change registrations, suspend
// callback delivery, oversized window fitting, and window subclass forwarding.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "lifecycle.h"
#include "shims.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

struct SuspendProbe {
    int calls = 0;
    BOOLEAN last_quiesced = FALSE;
};

void WINAPI on_app_state(BOOLEAN quiesced, PVOID context) {
    auto* probe = static_cast<SuspendProbe*>(context);
    if (probe) {
        ++probe->calls;
        probe->last_quiesced = quiesced;
    }
}

using UnregFn = void(WINAPI*)(PVOID);
UnregFn g_unreg = nullptr;

// Context for a callback that unregisters `target` (another registration, or its own handle) when it runs.
struct UnregProbe {
    int calls = 0;
    PVOID target = nullptr;
};

void WINAPI on_app_state_unregister(BOOLEAN, PVOID context) {
    auto* probe = static_cast<UnregProbe*>(context);
    ++probe->calls;
    if (probe->target) g_unreg(probe->target);
}

}  // namespace

int main() {
    using RegFn = ULONG(WINAPI*)(void(WINAPI*)(BOOLEAN, PVOID), PVOID, PVOID*);
    using CreateExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE,
                                      LPVOID);

    const auto reg = reinterpret_cast<RegFn>(
        rt::shims::find("api-ms-win-core-psm-appnotify-l1-1-0.dll", "RegisterAppStateChangeNotification"));
    const auto unreg = reinterpret_cast<UnregFn>(
        rt::shims::find("api-ms-win-core-psm-appnotify-l1-1-0.dll", "UnregisterAppStateChangeNotification"));
    const auto create_w = reinterpret_cast<CreateExWFn>(rt::shims::find("user32.dll", "CreateWindowExW"));
    const auto create_w_ext =
        reinterpret_cast<CreateExWFn>(rt::shims::find("ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "CreateWindowExW"));

    check(reg != nullptr && unreg != nullptr, "Register/UnregisterAppStateChangeNotification resolve");
    check(create_w != nullptr && create_w == create_w_ext,
          "CreateWindowExW resolves through user32 and ext-ms-win-rtcore-ntuser-window");
    check(!rt::shims::owns_dll("user32.dll"), "user32.dll stays partially overridden");
    // The title imports CreateWindowExA from the ANSI window set, not from user32.dll.
    const void* create_a = rt::shims::find("user32.dll", "CreateWindowExA");
    check(create_a != nullptr &&
              rt::shims::find("ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA") == create_a,
          "CreateWindowExA resolves through ext-ms-win-rtcore-ntuser-window-ansi to the same shim");
    if (!reg || !unreg || !create_w) return 1;
    g_unreg = unreg;

    // 1. Parameter validation and registration / unregistration / suspend dispatch.
    PVOID handle_a = nullptr, handle_b = nullptr;
    check(reg(nullptr, nullptr, &handle_a) == ERROR_INVALID_PARAMETER, "null routine returns ERROR_INVALID_PARAMETER");
    check(reg(&on_app_state, nullptr, nullptr) == ERROR_INVALID_PARAMETER,
          "null registration out pointer returns ERROR_INVALID_PARAMETER");

    SuspendProbe probe_a{}, probe_b{};
    check(reg(&on_app_state, &probe_a, &handle_a) == ERROR_SUCCESS && handle_a != nullptr, "register callback A");
    check(reg(&on_app_state, &probe_b, &handle_b) == ERROR_SUCCESS && handle_b != nullptr && handle_b != handle_a,
          "register callback B with distinct handle");
    check(rt::lifecycle::registered_notification_count() == 2, "two registrations active");

    unreg(handle_a);
    check(rt::lifecycle::registered_notification_count() == 1, "unregister removes callback A");

    rt::lifecycle::invoke_suspend_callbacks_for_test();
    check(probe_a.calls == 0 && probe_b.calls == 1 && probe_b.last_quiesced == TRUE,
          "suspend dispatch invokes only remaining callback with quiesced=TRUE");
    unreg(handle_b);
    check(rt::lifecycle::registered_notification_count() == 0, "all registrations removed");

    // 1b. A callback that unregisters a later registration: the later one is skipped during the same dispatch.
    PVOID handle_later = nullptr, handle_first = nullptr;
    SuspendProbe later_probe{};
    UnregProbe first_probe{};
    check(reg(&on_app_state_unregister, &first_probe, &handle_first) == ERROR_SUCCESS,
          "register unregistering callback");
    check(reg(&on_app_state, &later_probe, &handle_later) == ERROR_SUCCESS, "register later callback");
    first_probe.target = handle_later;
    rt::lifecycle::invoke_suspend_callbacks_for_test();
    check(first_probe.calls == 1 && later_probe.calls == 0,
          "unregistered later callback does not run in the same dispatch");
    check(rt::lifecycle::registered_notification_count() == 1, "later registration is gone, the first remains");
    unreg(handle_first);
    check(rt::lifecycle::registered_notification_count() == 0, "dispatch test registrations removed");

    // 1c. A callback that unregisters itself: no deadlock, it runs once, and it is gone afterwards.
    PVOID handle_self = nullptr;
    UnregProbe self_probe{};
    check(reg(&on_app_state_unregister, &self_probe, &handle_self) == ERROR_SUCCESS,
          "register self-unregistering callback");
    self_probe.target = handle_self;
    rt::lifecycle::invoke_suspend_callbacks_for_test();
    check(self_probe.calls == 1 && rt::lifecycle::registered_notification_count() == 0,
          "self-unregistering callback runs once and is removed without deadlock");
    rt::lifecycle::invoke_suspend_callbacks_for_test();
    check(self_probe.calls == 1, "removed self-unregistering callback is not called again");

    // 2. Oversized window scaling math.
    const RECT area{0, 0, 1920, 1040};
    const auto fitted_4k = rt::lifecycle::fit_window_to_area(area, 3840, 2160);
    check(fitted_4k.first == 1848 && fitted_4k.second == 1040,
          "3840x2160 scales proportionally to 1848x1040 inside 1920x1040 work area");
    const auto fitted_720 = rt::lifecycle::fit_window_to_area(area, 1280, 720);
    check(fitted_720.first == 1280 && fitted_720.second == 720, "1280x720 stays unchanged");
    const auto fitted_def = rt::lifecycle::fit_window_to_area(area, CW_USEDEFAULT, 0);
    check(fitted_def.first == CW_USEDEFAULT && fitted_def.second == 0, "CW_USEDEFAULT stays unchanged");
    const RECT square{0, 0, 1000, 1000};
    const auto fitted_wide = rt::lifecycle::fit_window_to_area(square, 4000, 2);
    check(fitted_wide.first == 1000 && fitted_wide.second == 1,
          "extreme wide aspect ratio clamps minor axis to at least 1 pixel");
    const auto fitted_tall = rt::lifecycle::fit_window_to_area(square, 2, 4000);
    check(fitted_tall.first == 1 && fitted_tall.second == 1000,
          "extreme tall aspect ratio clamps minor axis to at least 1 pixel");
    const RECT wide_span{-2000000000, 0, 2000000000, 1080};
    const auto fitted_span = rt::lifecycle::fit_window_to_area(wide_span, 3840, 2160);
    check(fitted_span.first == 1920 && fitted_span.second == 1080,
          "work area span exceeding 32-bit signed int does not overflow");
    const RECT empty_area{100, 100, 100, 500};
    const auto fitted_empty = rt::lifecycle::fit_window_to_area(empty_area, 3840, 2160);
    check(fitted_empty.first == 3840 && fitted_empty.second == 2160, "empty work area leaves dimensions unchanged");

    // 3. Subclassed window still forwards normal messages to its original window procedure.
    const HWND hwnd = create_w(0, L"STATIC", L"before", 0, 0, 0, 16, 16, nullptr, nullptr, nullptr, nullptr);
    check(hwnd != nullptr, "CreateWindowExW shim creates a live window");
    if (hwnd) {
        SetWindowTextW(hwnd, L"after");
        wchar_t buf[16] = {};
        const int n = GetWindowTextW(hwnd, buf, 16);
        check(n == 5 && std::wcscmp(buf, L"after") == 0, "subclassed window forwards WM_SETTEXT/WM_GETTEXT");
        check(GetPropW(hwnd, L"leonida.title.wndproc") != nullptr, "original WNDPROC property installed");
        DestroyWindow(hwnd);
    }

    // 4. Only the title's first top-level window is hooked; a window is released when it is destroyed. Hidden windows
    // only: no SC_CLOSE is posted.
    const wchar_t* kHookProp = L"leonida.title.wndproc";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LeonidaLifecycleTest";
    RegisterClassExW(&wc);
    const DWORD kMain = WS_OVERLAPPEDWINDOW;
    const wchar_t* kClass = L"LeonidaLifecycleTest";
    HWND main_win = create_w(0, kClass, L"main", kMain, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    check(main_win && GetPropW(main_win, kHookProp) != nullptr, "first parentless window is the hooked title window");
    HWND second = create_w(0, kClass, L"second", kMain, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    check(second && GetPropW(second, kHookProp) == nullptr, "a second parentless window is not hooked");
    HWND tool = create_w(WS_EX_TOOLWINDOW, L"LeonidaLifecycleTest", L"tool", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr,
                         nullptr, nullptr);
    check(tool && GetPropW(tool, kHookProp) == nullptr, "a WS_EX_TOOLWINDOW window is not hooked");
    if (main_win) DestroyWindow(main_win);
    HWND again = create_w(0, kClass, L"again", kMain, 0, 0, 64, 64, nullptr, nullptr, nullptr, nullptr);
    check(again && GetPropW(again, kHookProp) != nullptr, "after the title window is destroyed, a new one is hooked");
    for (HWND w : {second, tool, again}) {
        if (w) DestroyWindow(w);
    }

    std::printf("%d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
