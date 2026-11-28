#pragma once
// LEONIDA_DEV_OVERLAY=1 (off by default, Windows): a small click-through window over the top left corner of the title's
// window that says how the boot is doing: frames and submissions a second, where a submission's CPU time goes and which
// phase runs now, pipeline builds, indirect draws, CPU, memory. It is its own window with its own thread and GDI text,
// so it keeps counting while the title shows no frames for seconds (pipeline builds, a stalled submission), it is in no
// captured frame, and it changes nothing the GPU does. It only reads counters: submit_phases.h, indirect_draw_stats.h,
// dev_status.h, and memory_figures.h for the private memory and the device's GPU memory, which LEONIDA_TITLE_HUD
// (shims/title_hud.h) shows with the same text. The HUD is the other view: frame times, memory and compile time in the
// window's title bar.
#include "dev_status.h"
#include "indirect_draw_stats.h"
#include "memory_figures.h"
#include "submit_phases.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "psapi.lib")
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace leonida::dev_overlay {

// The counters between two looks. A total that went down was reset by its own log line: what it holds is the new part.
inline std::uint64_t grown(std::uint64_t now, std::uint64_t& before) {
    const std::uint64_t by = now >= before ? now - before : now;
    before = now;
    return by;
}

struct Sample {
    double seconds = 0;
    double frames = 0, submissions = 0;                 // a second
    double phase_ms[submit_phases::kCount]{};           // a submission
    int phase = -1;                                     // running now
    double phase_for = 0;                               // seconds it has been
    unsigned building = 0;
    std::uint64_t built = 0;
    std::string build;                                  // the last one started
    unsigned long long drawn = 0, skipped = 0;          // indirect draws so far
    double cores = 0, free_gb = 0;
    memory_figures::ProcessMemory ram;                  // private memory and working set
    VramBudget vram;                                    // the device's VK_EXT_memory_budget; "n/a" without it
};

// The text of one sample: pure, for the test.
inline std::vector<std::string> lines(const Sample& s, double age) {
    char text[160];
    std::vector<std::string> out;
    std::snprintf(text, sizeof text, "Leonida dev  %.0f s   %.1f fps   %.1f submissions/s", age, s.frames, s.submissions);
    out.emplace_back(text);
    double total = 0;
    for (const double ms : s.phase_ms) total += ms;
    std::snprintf(text, sizeof text, "CPU/submission %.1f ms: descriptors %.1f, textures %.1f, copies %.1f, replay %.1f", total,
                  s.phase_ms[submit_phases::heaps], s.phase_ms[submit_phases::textures],
                  s.phase_ms[submit_phases::shadows] + s.phase_ms[submit_phases::geometry],
                  s.phase_ms[submit_phases::replay]);
    out.emplace_back(text);
    static const char* const now[submit_phases::kCount] = {"waiting for the GPU",   "fixing descriptors", "copying title memory",
                                                           "copying geometry",      "refreshing textures", "recording and submitting"};
    if (s.phase >= 0 && s.phase < int(submit_phases::kCount)) {
        if (std::isfinite(s.phase_for) && s.phase_for >= 1.0)
            std::snprintf(text, sizeof text, "now: %s for %.0f s", now[s.phase], std::floor(s.phase_for));
        else
            std::snprintf(text, sizeof text, "now: %s", now[s.phase]);
    } else {
        std::snprintf(text, sizeof text, "now: the title's own code");
    }
    out.emplace_back(text);
    if (s.building)
        std::snprintf(text, sizeof text, "pipelines: %llu built, %u building (%.50s)",
                      static_cast<unsigned long long>(s.built), s.building, s.build.c_str());
    else
        std::snprintf(text, sizeof text, "pipelines: %llu built", static_cast<unsigned long long>(s.built));
    out.emplace_back(text);
    std::snprintf(text, sizeof text, "indirect draws: %llu drawn, %llu skipped", s.drawn, s.skipped);
    out.emplace_back(text);
    std::snprintf(text, sizeof text, "%.1f cores, %.1f GB RAM free", s.cores, s.free_gb);
    out.emplace_back(text);
    out.emplace_back(memory_figures::ram_text(s.ram) + "   " + memory_figures::vram_text(s.vram));
    return out;
}

inline bool enabled() {
    static const bool on = [] {
#ifdef _WIN32
        char value[3]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof value, "LEONIDA_DEV_OVERLAY") == 0 && length == 2 && value[0] == '1';
#else
        const char* value = std::getenv("LEONIDA_DEV_OVERLAY");
        return value && value[0] == '1' && !value[1];
#endif
    }();
    return on;
}

#ifdef _WIN32
class Overlay {
public:
    void run() {
        WNDCLASSW type{};
        type.lpfnWndProc = &Overlay::procedure;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"LeonidaDevOverlay";
        if (!RegisterClassW(&type)) return;
        window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                                  type.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, type.hInstance, this);
        if (!window_) return;
        SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetLayeredWindowAttributes(window_, 0, 205, LWA_ALPHA);
        font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        look();
        SetTimer(window_, 1, 500, nullptr);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) DispatchMessageW(&message);
    }

private:
    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
        auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == WM_TIMER) {
            self->look();
            return 0;
        }
        if (self && message == WM_PAINT) {
            self->paint();
            return 0;
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        return DefWindowProcW(window, message, w, l);
    }

    // The process's largest visible top-level window is the title's.
    struct Search {
        HWND own, found;
        std::int64_t area;
    };
    static BOOL CALLBACK consider(HWND window, LPARAM parameter) {
        auto& search = *reinterpret_cast<Search*>(parameter);
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        RECT r{};
        if (process != GetCurrentProcessId() || window == search.own || !IsWindowVisible(window) || IsIconic(window) ||
            !GetClientRect(window, &r))
            return TRUE;
        const std::int64_t area = std::int64_t(r.right) * r.bottom;
        if (area > search.area) search = {search.own, window, area};
        return TRUE;
    }

    void look() {
        namespace sp = submit_phases;
        sp::Totals& totals = sp::totals();
        const std::int64_t now = sp::now_ns();
        Sample s;
        s.seconds = before_ ? double(now - before_) * 1e-9 : 0;
        const std::uint64_t frames = grown(totals.present_serial.load(std::memory_order_relaxed), frames_);
        const std::uint64_t submissions = grown(totals.all_submissions.load(std::memory_order_relaxed), submissions_);
        std::uint64_t ns[sp::kCount];
        for (unsigned p = 0; p < sp::kCount; ++p) ns[p] = grown(totals.ns[p].load(std::memory_order_relaxed), ns_[p]);
        // A stall longer than a look leaves nothing to divide by: the figures then cover the last two seconds.
        window_frames_ += frames;
        window_submissions_ += submissions;
        window_seconds_ += s.seconds;
        for (unsigned p = 0; p < sp::kCount; ++p) window_ns_[p] += ns[p];
        if (window_seconds_ >= 2 || !before_) {
            shown_ = {};
            if (window_seconds_ > 0) {
                shown_.frames = double(window_frames_) / window_seconds_;
                shown_.submissions = double(window_submissions_) / window_seconds_;
            }
            for (unsigned p = 0; p < sp::kCount; ++p) {
                if (window_submissions_) shown_.phase_ms[p] = double(window_ns_[p]) * 1e-6 / double(window_submissions_);
                window_ns_[p] = 0;
            }
            window_frames_ = window_submissions_ = 0;
            window_seconds_ = 0;
        }
        s.frames = shown_.frames;
        s.submissions = shown_.submissions;
        std::copy(std::begin(shown_.phase_ms), std::end(shown_.phase_ms), std::begin(s.phase_ms));
        before_ = now;

        const sp::Running running = sp::running();
        s.phase = running.phase;
        if (s.phase >= 0) s.phase_for = std::max(0.0, double(now - running.since) * 1e-9);
        dev_status::Builds& builds = dev_status::builds();
        s.building = builds.active.load(std::memory_order_relaxed);
        s.built = builds.done.load(std::memory_order_relaxed);
        s.build = builds.last();
        const auto& draws = indirect_stats::counts().values;
        s.drawn = draws[std::size_t(indirect_stats::Event::emitted)].load(std::memory_order_relaxed);
        s.skipped = draws[std::size_t(indirect_stats::Event::record_skip)].load(std::memory_order_relaxed) +
                    draws[std::size_t(indirect_stats::Event::replay_skip)].load(std::memory_order_relaxed);
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            const auto ticks = [](const FILETIME& f) { return (std::uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
            const std::uint64_t used = grown(ticks(kernel) + ticks(user), cpu_);
            if (s.seconds > 0) s.cores = double(used) * 1e-7 / s.seconds;
        }
        s.ram = memory_figures::process_memory();
        s.vram = memory_figures::latest_vram();  // reads the device at most once per 500 ms
        MEMORYSTATUSEX status{sizeof status};
        if (GlobalMemoryStatusEx(&status)) s.free_gb = double(status.ullAvailPhys) / double(1u << 30);
        lines_ = lines(s, sp::process_age());
        place();
    }

    void place() {
        Search search{window_, nullptr, 0};
        EnumWindows(&Overlay::consider, reinterpret_cast<LPARAM>(&search));
        // Shown only over the title while it is the window in front, so the topmost text is never over anything else.
        if (!search.found || GetForegroundWindow() != search.found) {
            ShowWindow(window_, SW_HIDE);
            return;
        }
        POINT corner{8, 8};
        ClientToScreen(search.found, &corner);
        std::size_t longest = 0;
        for (const auto& line : lines_) longest = std::max(longest, line.size());
        SetWindowPos(window_, HWND_TOPMOST, corner.x, corner.y, int(longest) * 8 + 12, int(lines_.size()) * 17 + 8,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(window_, nullptr, FALSE);
    }

    void paint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(window_, &ps);
        RECT area;
        GetClientRect(window_, &area);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
        HGDIOBJ old_bitmap = SelectObject(memory, bitmap), old_font = SelectObject(memory, font_);
        FillRect(memory, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        SetBkMode(memory, TRANSPARENT);
        int y = 4;
        for (std::size_t i = 0; i < lines_.size(); ++i, y += 17) {
            SetTextColor(memory, i == 0 ? RGB(120, 255, 140) : RGB(235, 235, 235));
            TextOutA(memory, 6, y, lines_[i].data(), int(lines_[i].size()));
        }
        BitBlt(dc, 0, 0, area.right, area.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old_bitmap);
        SelectObject(memory, old_font);
        DeleteObject(bitmap);
        DeleteDC(memory);
        EndPaint(window_, &ps);
    }

    HWND window_ = nullptr;
    HFONT font_ = nullptr;
    std::vector<std::string> lines_;
    std::int64_t before_ = 0;
    std::uint64_t frames_ = 0, submissions_ = 0, cpu_ = 0, ns_[submit_phases::kCount]{};
    std::uint64_t window_frames_ = 0, window_submissions_ = 0, window_ns_[submit_phases::kCount]{};
    double window_seconds_ = 0;
    Sample shown_;
};
#endif

// Once per process; nothing without LEONIDA_DEV_OVERLAY=1.
inline void start() {
#ifdef _WIN32
    if (!enabled()) return;
    std::fprintf(stderr, "[rt] dev overlay on (LEONIDA_DEV_OVERLAY=1)\n");
    std::thread([] { static Overlay overlay; overlay.run(); }).detach();
#endif
}

}  // namespace leonida::dev_overlay
