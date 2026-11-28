// lifecycle: console application lifecycle shims for mapped titles.
//
// Console titles never receive a normal desktop WM_CLOSE; quitting suspends the title (running its
// RegisterAppStateChangeNotification callbacks with quiesced = TRUE, giving it ~1 s to flush saves) and then ends the
// process. Additionally, console titles often size their top-level window to a 4K render target (3840x2160), which
// overflows smaller desktop work areas.
//
// This shim provides:
//   - RegisterAppStateChangeNotification / UnregisterAppStateChangeNotification on api-ms-win-core-psm-appnotify-*
//     and kernelbase.dll;
//   - CreateWindowExW / CreateWindowExA on user32.dll and ext-ms-win-rtcore-ntuser-window-* (CreateWindowExA also on
//     the -ansi- set), fitting the title's first top-level window into the primary monitor work area and
//     subclassing it so SC_CLOSE runs registered suspend callbacks before the run ends.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "lifecycle.h"
#include "shims.h"

namespace rt::lifecycle {
namespace {

using AppStateRoutine = void(WINAPI*)(BOOLEAN quiesced, PVOID context);

struct Registration {
    std::uintptr_t id;
    AppStateRoutine routine;
    void* context;
};

constexpr wchar_t kTitleProcProp[] = L"leonida.title.wndproc";
constexpr auto kSuspendDeadline = std::chrono::seconds(1);

// Recursive so a callback can unregister from inside run_suspend_callbacks() on the same thread.
std::recursive_mutex& reg_mutex() {
    // Intentionally immortal so shutdown/WM_SYSCOMMAND cannot race static destruction.
    static auto* m = new std::recursive_mutex;
    return *m;
}

std::vector<Registration>& reg_list() {
    static auto* list = new std::vector<Registration>;
    return *list;
}

std::atomic<std::uintptr_t> g_next_reg_id{1};
std::atomic<bool> g_ending{false};
std::atomic<HWND> g_title_window{nullptr};

// Ends the run through the title's own ExitProcess (process.cpp's game_exit_process -> end_run, which lets the host
// finish its cleanup), not TerminateProcess. Falls back to ExitProcess if the shim table has no binding.
[[noreturn]] void end_title_run() {
    using ExitProcessFn = void(__stdcall*)(UINT);
    if (auto* exit_process = reinterpret_cast<ExitProcessFn>(rt::shims::find("kernel32.dll", "ExitProcess"))) {
        exit_process(0);
    }
    ExitProcess(0);
}

// Runs every registration still in the list, with the registration lock held for the whole loop: an Unregister on
// another thread waits for the callback in flight, and one from inside a callback re-enters the recursive lock and
// stops the later calls of its id. Ids come from a snapshot and are looked up again, so an entry removed by an
// earlier callback is skipped.
// ponytail: a callback that blocks on another thread which is itself in Register/Unregister stalls until the 1 s
// deadline ends the run.
void run_suspend_callbacks() {
    std::lock_guard lock(reg_mutex());
    std::vector<std::uintptr_t> ids;
    for (const auto& r : reg_list()) ids.push_back(r.id);
    for (const auto id : ids) {
        AppStateRoutine routine = nullptr;
        void* context = nullptr;
        for (const auto& r : reg_list()) {
            if (r.id == id) {
                routine = r.routine;
                context = r.context;
                break;
            }
        }
        if (routine) routine(TRUE, context);
    }
}

void trigger_suspend_and_exit() {
    if (g_ending.exchange(true, std::memory_order_seq_cst)) return;
    std::fprintf(stderr, "[rt] lifecycle: window closed; running %zu suspend routine(s)\n",
                 registered_notification_count());

    // Run suspend callbacks off the UI thread so callbacks that synchronize with the window thread do not deadlock.
    std::thread([]() {
        run_suspend_callbacks();
        std::fprintf(stderr, "[rt] lifecycle: title suspended; exiting\n");
        end_title_run();
    }).detach();

    std::thread([]() {
        std::this_thread::sleep_for(kSuspendDeadline);
        std::fprintf(stderr, "[rt] lifecycle: suspend deadline elapsed; exiting\n");
        end_title_run();
    }).detach();
}

LRESULT CALLBACK lifecycle_wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SYSCOMMAND && (wparam & 0xFFF0u) == SC_CLOSE) {
        trigger_suspend_and_exit();
        return 0;
    }
    const HANDLE prop = GetPropW(hwnd, kTitleProcProp);
    if (message == WM_NCDESTROY) {
        RemovePropW(hwnd, kTitleProcProp);
        HWND expected = hwnd;
        g_title_window.compare_exchange_strong(expected, nullptr);  // a later main window may be claimed
    }
    auto original = reinterpret_cast<WNDPROC>(prop);
    if (!original) return DefWindowProcW(hwnd, message, wparam, lparam);
    return CallWindowProcW(original, hwnd, message, wparam, lparam);
}

void install_window_hook(HWND hwnd) {
    if (!hwnd) return;
    const LONG_PTR prev = GetWindowLongPtrW(hwnd, GWLP_WNDPROC);
    if (!prev || reinterpret_cast<WNDPROC>(prev) == &lifecycle_wnd_proc) return;
    SetPropW(hwnd, kTitleProcProp, reinterpret_cast<HANDLE>(prev));
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&lifecycle_wnd_proc));
}

// Before CreateWindowEx: a candidate is a top-level, non-child, non-tool window. Returns whether it is one, and fits
// it only when it is the first candidate (no title window claimed yet).
bool prepare_title_window(HWND parent, DWORD style, DWORD ex_style, int& x, int& y, int& width, int& height) {
    if (parent != nullptr || (style & WS_CHILD) || (ex_style & WS_EX_TOOLWINDOW)) return false;
    if (g_title_window.load() != nullptr) return true;
    RECT area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &area, 0)) return true;
    const auto [fitted_w, fitted_h] = fit_window_to_area(area, width, height);
    if (fitted_w == width && fitted_h == height) return true;
    std::fprintf(stderr, "[rt] lifecycle: fitted oversized title window %dx%d -> %dx%d\n", width, height, fitted_w,
                 fitted_h);
    width = fitted_w;
    height = fitted_h;
    const std::int64_t area_w = static_cast<std::int64_t>(area.right) - static_cast<std::int64_t>(area.left);
    const std::int64_t area_h = static_cast<std::int64_t>(area.bottom) - static_cast<std::int64_t>(area.top);
    const std::int64_t cx = static_cast<std::int64_t>(area.left) + (area_w - width) / 2;
    const std::int64_t cy = static_cast<std::int64_t>(area.top) + (area_h - height) / 2;
    x = static_cast<int>(
        std::clamp<std::int64_t>(cx, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    y = static_cast<int>(
        std::clamp<std::int64_t>(cy, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    return true;
}

// After CreateWindowEx: the first candidate to succeed claims the title window and gets the SC_CLOSE hook.
void adopt_title_window(HWND hwnd, bool candidate) {
    if (!hwnd || !candidate) return;
    HWND expected = nullptr;
    if (g_title_window.compare_exchange_strong(expected, hwnd)) install_window_hook(hwnd);
}

ULONG WINAPI register_app_state_change(AppStateRoutine routine, PVOID context, PVOID* registration) {
    if (!routine || !registration) return ERROR_INVALID_PARAMETER;
    std::uintptr_t id = 0;
    do {
        id = g_next_reg_id.fetch_add(1, std::memory_order_relaxed);
    } while (id == 0);
    try {
        std::lock_guard lock(reg_mutex());
        reg_list().push_back({id, routine, context});
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    *registration = reinterpret_cast<PVOID>(id);
    return ERROR_SUCCESS;
}

void WINAPI unregister_app_state_change(PVOID registration) {
    const auto id = reinterpret_cast<std::uintptr_t>(registration);
    std::lock_guard lock(reg_mutex());
    auto& list = reg_list();
    list.erase(std::remove_if(list.begin(), list.end(), [id](const Registration& r) { return r.id == id; }),
               list.end());
}

HWND WINAPI create_window_ex_w_shim(DWORD ex_style, LPCWSTR class_name, LPCWSTR window_name, DWORD style, int x, int y,
                                    int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    const bool candidate = prepare_title_window(parent, style, ex_style, x, y, width, height);
    const HWND hwnd =
        ::CreateWindowExW(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, instance, param);
    adopt_title_window(hwnd, candidate);
    return hwnd;
}

HWND WINAPI create_window_ex_a_shim(DWORD ex_style, LPCSTR class_name, LPCSTR window_name, DWORD style, int x, int y,
                                    int width, int height, HWND parent, HMENU menu, HINSTANCE instance, LPVOID param) {
    const bool candidate = prepare_title_window(parent, style, ex_style, x, y, width, height);
    const HWND hwnd =
        ::CreateWindowExA(ex_style, class_name, window_name, style, x, y, width, height, parent, menu, instance, param);
    adopt_title_window(hwnd, candidate);
    return hwnd;
}

}  // namespace

std::pair<int, int> fit_window_to_area(const RECT& area, int width, int height) {
    const std::int64_t area_w = static_cast<std::int64_t>(area.right) - static_cast<std::int64_t>(area.left);
    const std::int64_t area_h = static_cast<std::int64_t>(area.bottom) - static_cast<std::int64_t>(area.top);
    if (width <= 0 || height <= 0 || area_w <= 0 || area_h <= 0 ||
        (static_cast<std::int64_t>(width) <= area_w && static_cast<std::int64_t>(height) <= area_h)) {
        return {width, height};
    }
    const std::int64_t max_w = std::min<std::int64_t>(area_w, std::numeric_limits<int>::max());
    const std::int64_t max_h = std::min<std::int64_t>(area_h, std::numeric_limits<int>::max());
    const double scale =
        std::min(static_cast<double>(max_w) / static_cast<double>(width),
                 static_cast<double>(max_h) / static_cast<double>(height));
    const int fitted_w = static_cast<int>(
        std::clamp<std::int64_t>(static_cast<std::int64_t>(static_cast<double>(width) * scale), 1, max_w));
    const int fitted_h = static_cast<int>(
        std::clamp<std::int64_t>(static_cast<std::int64_t>(static_cast<double>(height) * scale), 1, max_h));
    return {fitted_w, fitted_h};
}

std::size_t registered_notification_count() {
    std::lock_guard lock(reg_mutex());
    return reg_list().size();
}

void invoke_suspend_callbacks_for_test() {
    run_suspend_callbacks();
}

}  // namespace rt::lifecycle

namespace {
using namespace rt::lifecycle;

#define APP_NOTIFY_API(dll)                                                                          \
    {dll, "RegisterAppStateChangeNotification", reinterpret_cast<void*>(&register_app_state_change)}, \
    {dll, "UnregisterAppStateChangeNotification", reinterpret_cast<void*>(&unregister_app_state_change)}

#define WINDOW_API(dll)                                                                \
    {dll, "CreateWindowExW", reinterpret_cast<void*>(&create_window_ex_w_shim)},       \
    {dll, "CreateWindowExA", reinterpret_cast<void*>(&create_window_ex_a_shim)}

}  // namespace

SHIM_TABLE(lifecycle,
    APP_NOTIFY_API("api-ms-win-core-psm-appnotify-l1-1-0.dll"),
    APP_NOTIFY_API("api-ms-win-core-psm-appnotify-l1-1-1.dll"),
    APP_NOTIFY_API("kernelbase.dll"),
    WINDOW_API("user32.dll"),
    WINDOW_API("ext-ms-win-rtcore-ntuser-window-l1-1-0.dll"),
    WINDOW_API("ext-ms-win-rtcore-ntuser-window-l1-1-1.dll"),
    // The title's import table names CreateWindowExA from the ANSI set.
    {"ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll", "CreateWindowExA",
     reinterpret_cast<void*>(&create_window_ex_a_shim)},
);
