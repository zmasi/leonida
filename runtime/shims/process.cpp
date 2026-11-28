// Process-level imports of a mapped executable: its command line and its way out.
//
// `gta6rt --run` calls a mapped EXE's entry point inside the gta6rt process. The EXE's CRT startup asks for its
// arguments and finally calls exit(); both go through the EXE's own import table, so they can be answered here:
//   - the command line is the game's, and its CRT configures its own argv/argc at startup;
//   - exit(), _exit(), quick_exit(), ExitProcess() and abort() from the game end the game's run, not the host:
//     the exit code is recorded, the run is signalled as finished and the calling thread waits for the host to
//     exit;
//   - the game's atexit registrations (_crt_atexit, which MSVC's atexit and static destructors reach; msvcrt's
//     atexit and _onexit) go to a table of the run's own instead of the CRT's process-wide one, which the game
//     shares with gta6rt. exit() runs it newest first, as the CRT does, before the run ends; _exit(), _Exit(),
//     quick_exit(), ExitProcess() and abort() do not. _cexit() runs it and returns, _c_exit() returns. A second
//     thread that calls exit(), _exit(), _Exit() or quick_exit() while the first is leaving waits, as with the
//     CRT's exit lock. Registration and the cleanup of exit(), quick_exit() and _cexit() (the thread-local
//     callback, the table's drain and, for _cexit(), the stream flush) hold one lock of the run's own, as the UCRT's
//     exit lock serializes them: a second thread in _cexit(), or registering while a drain runs, waits for that
//     cleanup; the exiting thread may register from a handler (the lock is recursive). The lock is released before
//     the run ends. A handler that lets a C++ exception escape terminates the run as the UCRT's terminate() would:
//     abort(), exit code 3;
//   - the game's at_quick_exit registrations (_crt_at_quick_exit) go to a second table of the run's own, which
//     quick_exit() runs newest first; exit(), _exit() and _Exit() do not;
//   - the EXE's thread-local destructor callback (_register_thread_local_exe_atexit_callback, which the EXE's CRT
//     startup registers when the EXE has a thread_local with a destructor) is the run's own too: exit() and _cexit()
//     call it for the exiting thread before the atexit handlers, as the UCRT does (see
//     game_register_thread_local_exe_atexit_callback below). A second registration terminates, as in the UCRT.
// Covered CRTs: the Universal CRT (api-ms-win-crt-runtime-l1-1-0.dll / ucrtbase.dll, what MSVC links; ucrtbased.dll in Debug) and
// msvcrt.dll (MinGW). Calls made inside host DLLs, not through the game's imports, are not seen here.
// The loader matches imports by exact DLL name, so GetCommandLineA/W and ExitProcess are served under every name a
// title can import them by: kernel32.dll (the classic import libraries), kernelbase.dll, and the API sets that the
// umbrella libraries a GDK title links (OneCore_apiset.lib, mincore.lib) bind them to. The retail package's own DLLs
// import ExitProcess from api-ms-win-core-processthreads-l1-1-0.dll.
#include <windows.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "process_shim.h"
#include "shims.h"

namespace rt::process {
namespace {

std::vector<std::string> g_input_args;
std::vector<std::wstring> g_input_wargs;
std::vector<std::string> g_args;
std::vector<std::wstring> g_wargs;
std::vector<char*> g_argv;
std::vector<wchar_t*> g_wargv;
std::string g_cmdline;
std::wstring g_wcmdline;
int g_argc = 0;
char** g_argv_ptr = nullptr;
wchar_t** g_wargv_ptr = nullptr;

std::atomic<bool> g_running{false};
std::atomic<bool> g_exited{false};
std::atomic<int> g_exit_code{0};
HANDLE g_done = nullptr;

std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;  // CommandLineToArgvW: backslashes are literal unless they precede a quote
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++slashes;
        } else {
            if (c == L'"') out.append(slashes + 1, L'\\');
            slashes = 0;
        }
        out += c;
    }
    return out.append(slashes, L'\\') + L"\"";  // a trailing run is doubled before the closing quote
}

// The narrow arguments are ANSI code page text, like the CRT's narrow argv; under a UTF-8 activeCodePage CP_ACP is
// UTF-8 anyway. A character the code page has no mapping for becomes the code page's default character (or a
// best-fit one), as in the CRT, instead of failing: std::filesystem::path::string() throws there (RT-84).
std::string narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

void release_cleanup_lock();

[[noreturn]] void end_run(int code) {
    // A handler may end the run from inside a drain: the cleanup lock it holds is released before the thread parks,
    // so the host (and a later run_entry) can take it.
    release_cleanup_lock();
    // Read before signalling: once g_done is set, run_entry can return and clear g_running while the host is still
    // finishing the run (main.cpp's "game exited" line and cleanup), which this thread must not cut short.
    const bool running = g_running;
    bool expected = false;
    if (g_exited.compare_exchange_strong(expected, true)) {
        g_exit_code = code;
        if (g_done) SetEvent(g_done);
    }
    // Park until the host's ExitProcess, not ExitThread: Wine < 11.2 can exit 0 if the two race (4a9bc42a).
    if (running) for (;;) Sleep(INFINITE);
    // The run is already over: this is game code running during host shutdown (an atexit handler the game
    // registered with the shared CRT calling exit again). Finish the process with the game's exit code; ending
    // just this thread here would make the host's last thread, and so the process, exit with `code` instead.
    ExitProcess(static_cast<UINT>(g_exit_code.load()));
}

// ---- the game's atexit table
using AtexitFn = void(__cdecl*)();
using OnexitFn = int(__cdecl*)();
using TlsCallback = void(NTAPI*)(PVOID, DWORD, PVOID);  // _tls_callback_type
// The run's cleanup lock, as the UCRT's exit lock: registration takes it, and a cleanup (exit, quick_exit, _cexit)
// holds it from before the thread-local callback through the table's drain, so two threads never clean up at once
// and a registration from another thread waits for the drain. Recursive, so the exiting thread may register from a
// handler. t_cleanup_held counts this thread's holds: end_run releases them all before the thread parks.
std::recursive_mutex g_cleanup_lock;
thread_local unsigned t_cleanup_held = 0;
std::vector<AtexitFn> g_atexit;
std::vector<AtexitFn> g_at_quick_exit;
std::atomic<TlsCallback> g_thread_local_exit{nullptr};
std::atomic<DWORD> g_exiting_thread{0};

struct CleanupLock {
    CleanupLock() {
        g_cleanup_lock.lock();
        ++t_cleanup_held;
    }
    ~CleanupLock() {
        --t_cleanup_held;
        g_cleanup_lock.unlock();
    }
    CleanupLock(const CleanupLock&) = delete;
    CleanupLock& operator=(const CleanupLock&) = delete;
};
void release_cleanup_lock() {
    while (t_cleanup_held) {
        --t_cleanup_held;
        g_cleanup_lock.unlock();
    }
}

bool register_in(std::vector<AtexitFn>& table, AtexitFn fn) {
    CleanupLock lock;
    try {
        table.push_back(fn);
    } catch (const std::bad_alloc&) {
        return false;  // the CRT reports a full table as a failed registration
    }
    return true;
}
bool register_atexit(AtexitFn fn) { return register_in(g_atexit, fn); }

void __cdecl game_abort();

// A guest cleanup callback. A C++ exception it lets escape would unwind into the runtime; the UCRT's exit calls
// terminate() if one throws, and its default handler aborts: exit code 3, and the remaining handlers do not run.
// A thread_local destructor is noexcept, so one that throws reaches the game's own terminate() before this catch;
// that abort() is the game's import, game_abort, and ends the run with the same code.
template <typename Call>
void guarded(const char* what, Call&& call) {
    try {
        call();
    } catch (...) {
        std::fprintf(stderr, "[rt] process: a %s threw a C++ exception; terminating as the UCRT does\n", what);
        game_abort();
    }
}

// Newest first, with the cleanup lock held throughout (the caller took it). A handler registered while the handlers
// run goes on the end and so runs next, as in the UCRT.
void run_handlers(std::vector<AtexitFn>& table, const char* what) {
    for (;;) {
        if (table.empty()) return;
        const AtexitFn fn = table.back();
        table.pop_back();
        guarded(what, fn);
    }
}
// exit() and _cexit(): the exiting thread's thread_local destructors first (the UCRT calls the registered callback
// with DLL_PROCESS_DETACH; a second call finds the thread's list empty), then the atexit handlers and static
// destructors. The caller holds the cleanup lock.
void run_atexit_handlers() {
    if (const TlsCallback callback = g_thread_local_exit.load())
        guarded("thread-local destructor", [&] { callback(nullptr, DLL_PROCESS_DETACH, nullptr); });
    run_handlers(g_atexit, "atexit handler");
}

// UCRT: int _crt_atexit(_PVFV), 0 on success. MSVC's atexit() and the static destructors of an EXE reach it.
int __cdecl game_crt_atexit(AtexitFn fn) {
    return fn && register_atexit(fn) ? 0 : -1;
}
// UCRT: int _crt_at_quick_exit(_PVFV), 0 on success. An EXE's at_quick_exit() reaches it.
int __cdecl game_crt_at_quick_exit(AtexitFn fn) {
    return fn && register_in(g_at_quick_exit, fn) ? 0 : -1;
}
// msvcrt.dll (MinGW): int atexit(void (*)(void)) and _onexit_t _onexit(_onexit_t).
int __cdecl game_msvcrt_atexit(AtexitFn fn) { return game_crt_atexit(fn); }
OnexitFn __cdecl game_onexit(OnexitFn fn) {
    // its int result is ignored, as the CRT ignores it
    return fn && register_atexit(reinterpret_cast<AtexitFn>(fn)) ? fn : nullptr;
}

// ---- the way out
// The CRT's exit lock, which exit(), _exit(), _Exit() and quick_exit() all take: the first thread to leave owns it
// and ends the run; any other thread that tries to leave waits forever. The owner may leave again from a handler
// (undefined in C): that ends the run with the new code and skips the remaining handlers.
bool take_exit_lock() {
    const DWORD self = GetCurrentThreadId();
    DWORD owner = 0;
    if (g_exiting_thread.compare_exchange_strong(owner, self)) return true;
    if (owner != self) {
        release_cleanup_lock();  // parked for good: a drain it was in must not keep the owner waiting
        for (;;) Sleep(INFINITE);
    }
    return false;
}
// exit(): the game's handlers and static destructors, then the end of the run. The cleanup lock is taken after the
// exit lock (a thread parked by the latter holds nothing) and released by end_run.
void __cdecl game_exit(int code) {
    if (take_exit_lock()) {
        CleanupLock lock;
        run_atexit_handlers();
    }
    end_run(code);
}
// _exit(), _Exit(): the end of the run without any handler.
void __cdecl game_underscore_exit(int code) {
    take_exit_lock();
    end_run(code);
}
// quick_exit(): the at_quick_exit handlers, newest first, then the end of the run without the atexit handlers.
void __cdecl game_quick_exit(int code) {
    if (take_exit_lock()) {
        CleanupLock lock;
        run_handlers(g_at_quick_exit, "quick_exit handler");
    }
    end_run(code);
}
// _cexit() runs the handlers and flushes the streams, then returns; _c_exit() only returns. The flush is inside the
// cleanup lock, as the UCRT's _cexit flushes under its exit lock. A second thread's _cexit() waits for the first's
// cleanup, then finds the table empty.
void __cdecl game_cexit() {
    CleanupLock lock;
    run_atexit_handlers();
    std::fflush(nullptr);
}
void __cdecl game_c_exit() {}
void __stdcall game_exit_process(UINT code) { end_run(static_cast<int>(code)); }

// ---- threads and processors
// The console has cores a PC may lack, and a title pins its threads to them with masks the host rejects: Windows
// answers a mask with no CPU of the process (or one outside it) with 0 and ERROR_INVALID_PARAMETER, and a title that
// checks the previous mask then aborts. The call succeeds on the console, so it does here: the thread stays where it
// is and the answer is the mask the thread had (the process mask for a thread never pinned). A bad handle and an empty
// mask still fail, as on Windows. The fact (answer instead of failing when the host call returns 0) is from sammwyy/xvalkyrie src/runtime/modules/kernel.cpp (commit
// 53af7df77c10, GPL-3.0), reimplemented here in our own words and checked against the Win32 behaviour.
DWORD_PTR __stdcall game_set_thread_affinity_mask(HANDLE thread, DWORD_PTR mask) {
    DWORD_PTR previous = SetThreadAffinityMask(thread, mask);
    if (previous != 0) return previous;
    // An empty mask names no CPU on any machine: it fails as on Windows (the host's own answer, ERROR_INVALID_PARAMETER).
    if (mask == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    DWORD_PTR process = 0, system = 0;
    if (GetLastError() != ERROR_INVALID_PARAMETER || !GetProcessAffinityMask(GetCurrentProcess(), &process, &system))
        return 0;
    // The caller expects the mask the thread had, which may be narrower than the process mask (it may restore it
    // later). The host reports it only on a successful call, so widen the thread to the process mask, read the old
    // mask from that call, and put the old mask back: the thread ends where it started.
    const DWORD_PTR actual = SetThreadAffinityMask(thread, process);
    if (actual == 0) return 0;  // the host's error stands (a bad handle)
    if (actual != process) SetThreadAffinityMask(thread, actual);
    // The first boot shows which masks the title asks for: the first few are named, the rest only counted.
    static std::atomic<int> substituted{0};
    const int n = ++substituted;
    if (n <= 8)
        std::fprintf(stderr, "[rt] process: SetThreadAffinityMask(0x%llx) names cores the host lacks; the thread stays, "
                             "answered its mask 0x%llx (call %d)\n",
                     static_cast<unsigned long long>(mask), static_cast<unsigned long long>(actual), n);
    // The log may have failed (a closed stderr pipe leaves ERROR_NO_DATA); the call itself succeeded.
    SetLastError(ERROR_SUCCESS);
    return actual;
}
// The retail title imports the group form (processtopology API sets), not SetThreadAffinityMask. The same rule holds: a
// GROUP_AFFINITY naming cores or a processor group the PC lacks is answered, not failed. The thread stays where it is,
// the call returns TRUE and the out parameter gets the affinity the thread had. An empty mask names no CPU on any
// machine and a bad handle is the host's own error: both are passed to the host and fail as on Windows.
BOOL __stdcall game_set_thread_group_affinity(HANDLE thread, const GROUP_AFFINITY* requested, PGROUP_AFFINITY previous) {
    // An empty mask names no CPU on any machine. Some Windows builds accept it and clear the affinity instead of
    // failing, so answer the documented error before the host call rather than depending on the build.
    if (!requested || requested->Mask == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (SetThreadGroupAffinity(thread, requested, previous)) return TRUE;
    if (GetLastError() != ERROR_INVALID_PARAMETER) return FALSE;
    GROUP_AFFINITY actual = {};
    if (!GetThreadGroupAffinity(thread, &actual)) return FALSE;  // the host's error stands
    if (previous) *previous = actual;
    static std::atomic<int> substituted{0};
    const int n = ++substituted;
    if (n <= 8)
        std::fprintf(stderr, "[rt] process: SetThreadGroupAffinity(group %u, 0x%llx) names cores or a group the host lacks; "
                             "the thread stays, answered group %u mask 0x%llx (call %d)\n",
                     static_cast<unsigned>(requested->Group), static_cast<unsigned long long>(requested->Mask),
                     static_cast<unsigned>(actual.Group), static_cast<unsigned long long>(actual.Mask), n);
    // The log may have failed (a closed stderr pipe leaves ERROR_NO_DATA); the call itself succeeded.
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
// A console-only kernel32 export that pins a thread pool to cores. The host has no such call, and a pool with the
// host's own affinity runs the same work, so it succeeds and changes nothing.
BOOL __stdcall game_set_threadpool_affinity_mask(PTP_POOL, DWORD_PTR) { return TRUE; }
void __cdecl game_abort() {
    std::fprintf(stderr, "[rt] process: the game called abort()\n");
    // The game's signal() import is not shimmed: its handler goes into the host CRT's process-wide table, which
    // std::signal here reads too. UCRT's abort() raises SIGABRT before it terminates; do the same only when a
    // handler is installed, because raising it with the default action would terminate the host process. A handler
    // that exits ends the run itself; one that returns falls through to the CRT's abort code below.
    const auto previous = std::signal(SIGABRT, SIG_DFL);
    if (previous != SIG_ERR) {
        std::signal(SIGABRT, previous);
        if (previous != SIG_DFL && previous != SIG_IGN) std::raise(SIGABRT);
    }
    end_run(3);  // the CRT's abort exit code
}
// UCRT: void _register_thread_local_exe_atexit_callback(_tls_callback_type). An MSVC EXE's startup passes
// vcruntime's TLS destructor callback, which its TLS directory also lists; run_atexit_handlers calls it as
// (nullptr, DLL_PROCESS_DETACH, nullptr) for the exiting thread. Other threads' thread_local objects end through the
// loader's thread-detach notification, which calls the same function from the TLS directory. The host CRT cannot
// take it: the UCRT accepts one per process and fast-fails the second (gta6rt's own startup registers the first, so
// the title stopped at boot on Windows), and Wine's ucrtbase replaces gta6rt's own and never calls the title's,
// because the run ends through end_run. Here too the callback can be set once; a second registration calls
// terminate(), which aborts.
void __cdecl game_register_thread_local_exe_atexit_callback(TlsCallback callback) {
    TlsCallback expected = nullptr;
    if (!g_thread_local_exit.compare_exchange_strong(expected, callback)) {
        std::fprintf(stderr, "[rt] process: the thread-local exit callback was registered twice\n");
        game_abort();
    }
}

// ---- arguments
int* __cdecl game_p_argc() { return &g_argc; }
char*** __cdecl game_p_argv() { return &g_argv_ptr; }
wchar_t*** __cdecl game_p_wargv() { return &g_wargv_ptr; }

std::wstring widen_argument(const std::string& text) {
    const int size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    std::wstring wide(size > 0 ? static_cast<size_t>(size - 1) : 0, L'\0');
    if (size > 1) MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, wide.data(), size);
    return wide;
}

// UCRT startup expands each wildcard argument against the host working directory, even when the argument was
// quoted. A failed search retains the pattern; a successful search omits "." and ".." and keeps the path prefix.
// Narrow startup searches its ACP-converted text, which can have different wildcard matches from wide startup.
template <typename C>
std::vector<std::basic_string<C>> expand_arguments(const std::vector<std::basic_string<C>>& input) {
    std::vector<std::basic_string<C>> result;
    for (const auto& argument : input) {
        std::wstring pattern;
        if constexpr (std::is_same_v<C, wchar_t>) pattern = argument;
        else pattern = widen_argument(argument);
        const size_t wildcard = pattern.find_first_of(L"*?");
        if (wildcard == std::wstring::npos) {
            result.push_back(argument);
            continue;
        }
        const size_t separator = pattern.find_last_of(L"\\/:", wildcard);
        if (separator != std::wstring::npos && pattern[separator] == L':' && separator != 1) {
            result.push_back(argument);
            continue;
        }
        WIN32_FIND_DATAW found{};
        const HANDLE handle = ::FindFirstFileExW(pattern.c_str(), FindExInfoStandard, &found,
                                                 FindExSearchNameMatch, nullptr, 0);
        if (handle == INVALID_HANDLE_VALUE) {
            result.push_back(argument);
            continue;
        }
        struct Search {
            HANDLE handle;
            ~Search() { ::FindClose(handle); }
        } search{handle};
        const std::wstring prefix = pattern.substr(0, separator == std::wstring::npos ? 0 : separator + 1);
        do {
            if (std::wcscmp(found.cFileName, L".") == 0 || std::wcscmp(found.cFileName, L"..") == 0) continue;
            const std::wstring match = prefix + found.cFileName;
            if constexpr (std::is_same_v<C, wchar_t>) result.push_back(match);
            else result.push_back(narrow(match));
        } while (::FindNextFileW(handle, &found));
    }
    return result;
}

template <typename C>
int configure_argv(int mode, const std::vector<std::basic_string<C>>& input,
                   std::vector<std::basic_string<C>>& strings, std::vector<C*>& pointers, C**& published) {
    // _crt_argv_no_arguments is a no-op, including after another width or mode has been configured.
    if (mode == 0) return 0;
    if (mode != 1 && mode != 2) return errno = EINVAL;
    try {
        auto next_strings = mode == 2 ? expand_arguments(input) : input;
        std::vector<C*> next_pointers;
        next_pointers.reserve(next_strings.size() + 1);
        for (auto& argument : next_strings) next_pointers.push_back(argument.data());
        next_pointers.push_back(nullptr);
        // Publish only after every allocation succeeds. The other width keeps its own strings and pointer table.
        strings.swap(next_strings);
        pointers.swap(next_pointers);
        published = pointers.data();
        g_argc = static_cast<int>(strings.size());
        return 0;
    } catch (const std::bad_alloc&) {
        return errno = ENOMEM;
    }
}

int __cdecl game_configure_narrow_argv(int mode) {
    return configure_argv(mode, g_input_args, g_args, g_argv, g_argv_ptr);
}
int __cdecl game_configure_wide_argv(int mode) {
    return configure_argv(mode, g_input_wargs, g_wargs, g_wargv, g_wargv_ptr);
}

LPSTR __stdcall game_get_command_line_a() { return g_cmdline.data(); }
LPWSTR __stdcall game_get_command_line_w() { return g_wcmdline.data(); }
char** __cdecl game_p_acmdln_storage() {
    static char* p = nullptr;
    p = g_cmdline.data();
    return &p;
}
wchar_t** __cdecl game_p_wcmdln_storage() {
    static wchar_t* p = nullptr;
    p = g_wcmdline.data();
    return &p;
}

// What the UCRT's _get_narrow/_get_wide_winmain_command_line return, and the CRT's WinMain startup passes as
// lpCmdLine: the command line after the program name and the blanks that follow it. The program name ends at the
// first character up to ' ' outside quotes, the UCRT's own scan. The comparison is unsigned, so a non-ASCII byte of
// the narrow line is part of the name; a DBCS trail byte is never '"' or a blank, so it needs no lead-byte step.
template <typename C>
C* after_program_name(C* p) {
    using U = std::make_unsigned_t<C>;
    bool quoted = false;
    while (U(*p) > U(' ') || (*p && quoted)) {
        if (*p == C('"')) quoted = !quoted;
        ++p;
    }
    while (*p && U(*p) <= U(' ')) ++p;
    return p;
}
char* __cdecl game_get_narrow_winmain_command_line() { return after_program_name(g_cmdline.data()); }
wchar_t* __cdecl game_get_wide_winmain_command_line() { return after_program_name(g_wcmdline.data()); }

// The host CRT's environment tables, read when the game asks (RT-136). _environ and _wenviron are not stable: the host
// CRT builds the wide table only when a wide environment function first runs (a host started through main() has
// none until then), and _putenv/_wputenv reallocate both, so a pointer taken in set_command_line would be null or
// dangling by the time a wide-character startup calls __wgetmainargs. A lookup of a name no one sets makes the CRT
// create the table if it does not exist yet (from the other table or the OS block) without changing it.
// Like the msvcrt.dll original, the returned table is the CRT's own and is only valid until the next _putenv: a CRT
// startup reads it once, before the game has other threads, and a host that edits its environment concurrently with
// a game's startup is not covered.
char** current_environment() {
    if (!_environ) {
        size_t size = 0;
        (void)getenv_s(&size, nullptr, 0, "LEONIDA_RT_ENV_INIT");
    }
    return _environ;
}
wchar_t** current_wide_environment() {
    if (!_wenviron) {
        size_t size = 0;
        (void)_wgetenv_s(&size, nullptr, 0, L"LEONIDA_RT_ENV_INIT");
    }
    return _wenviron;
}

// msvcrt.dll: int __getmainargs(int* argc, char*** argv, char*** env, int expand, void* startinfo)
int __cdecl game_getmainargs(int* argc, char*** argv, char*** env, int, void*) {
    if (const int error = game_configure_narrow_argv(1)) return error;
    *argc = g_argc;
    *argv = g_argv_ptr;
    if (env) *env = current_environment();
    return 0;
}
int __cdecl game_wgetmainargs(int* argc, wchar_t*** argv, wchar_t*** env, int, void*) {
    if (const int error = game_configure_wide_argv(1)) return error;
    *argc = g_argc;
    *argv = g_wargv_ptr;
    if (env) *env = current_wide_environment();
    return 0;
}

struct EntryCall {
    void* entry;
};

DWORD WINAPI game_thread(void* p) {
    auto* call = static_cast<EntryCall*>(p);
    // An EXE entry point (mainCRTStartup and friends) takes the PEB pointer and returns the process exit code.
    auto entry = reinterpret_cast<DWORD(WINAPI*)(void*)>(call->entry);
    const DWORD code = entry(nullptr);
    end_run(static_cast<int>(code));  // returning from the entry point is an exit too
}

}  // namespace

void set_command_line(const std::wstring& program, const std::vector<std::wstring>& args) {
    g_input_wargs.clear();
    g_input_wargs.push_back(program);
    g_input_wargs.insert(g_input_wargs.end(), args.begin(), args.end());
    g_input_args.clear();
    g_wcmdline.clear();
    for (const auto& a : g_input_wargs) {
        g_input_args.push_back(narrow(a));
        if (!g_wcmdline.empty()) g_wcmdline += L' ';
        g_wcmdline += quote(a);
    }
    g_cmdline = narrow(g_wcmdline);
    // GetCommandLine is available before TLS/DllMain, but CRT argv starts unconfigured on every run.
    g_args.clear();
    g_wargs.clear();
    g_argv.clear();
    g_wargv.clear();
    g_argc = 0;
    g_argv_ptr = nullptr;
    g_wargv_ptr = nullptr;
}

int run_entry(void* entry, size_t stack_bytes) {
    {
        CleanupLock lock;
        g_atexit.clear();  // a run never sees the handlers of an earlier one
        g_at_quick_exit.clear();
    }
    g_thread_local_exit = nullptr;
    g_exiting_thread = 0;
    g_exited = false;
    g_exit_code = 0;
    g_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_done) return -1;
    g_running = true;
    EntryCall call{entry};
    HANDLE thread = CreateThread(nullptr, stack_bytes, game_thread, &call, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        g_running = false;
        CloseHandle(g_done);
        g_done = nullptr;
        return -1;
    }
    // The run ends when any game thread exits the process (exit/ExitProcess/abort) or the entry point returns.
    HANDLE waits[] = {g_done, thread};
    WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    bool expected = false;
    if (g_exited.compare_exchange_strong(expected, true)) {
        // The entry thread exited without calling end_run (e.g. ExitThread); claim g_exited so no worker thread can
        // touch g_done while we close it, and preserve the thread's exit code.
        DWORD thread_code = 0;
        if (GetExitCodeThread(thread, &thread_code)) g_exit_code = static_cast<int>(thread_code);
    } else {
        // Another thread already won g_exited and is publishing g_exit_code + SetEvent(g_done); wait until it finishes
        // so we never close g_done or return before g_exit_code is published.
        WaitForSingleObject(g_done, INFINITE);
    }
    CloseHandle(thread);
    CloseHandle(g_done);
    g_done = nullptr;
    g_running = false;
    return g_exit_code.load();
}

}  // namespace rt::process

namespace {
using namespace rt::process;
#define UCRT(dll) \
    {dll, "exit", reinterpret_cast<void*>(&game_exit)}, {dll, "_exit", reinterpret_cast<void*>(&game_underscore_exit)}, \
    {dll, "_Exit", reinterpret_cast<void*>(&game_underscore_exit)}, \
    {dll, "quick_exit", reinterpret_cast<void*>(&game_quick_exit)}, \
    {dll, "_crt_atexit", reinterpret_cast<void*>(&game_crt_atexit)}, \
    {dll, "_crt_at_quick_exit", reinterpret_cast<void*>(&game_crt_at_quick_exit)}, \
    {dll, "_register_thread_local_exe_atexit_callback", \
     reinterpret_cast<void*>(&game_register_thread_local_exe_atexit_callback)}, \
    {dll, "_cexit", reinterpret_cast<void*>(&game_cexit)}, {dll, "_c_exit", reinterpret_cast<void*>(&game_c_exit)}, \
    {dll, "abort", reinterpret_cast<void*>(&game_abort)}, {dll, "__p___argc", reinterpret_cast<void*>(&game_p_argc)}, \
    {dll, "__p___argv", reinterpret_cast<void*>(&game_p_argv)}, \
    {dll, "__p___wargv", reinterpret_cast<void*>(&game_p_wargv)}, \
    {dll, "_configure_narrow_argv", reinterpret_cast<void*>(&game_configure_narrow_argv)}, \
    {dll, "_configure_wide_argv", reinterpret_cast<void*>(&game_configure_wide_argv)}, \
    {dll, "__p__acmdln", reinterpret_cast<void*>(&game_p_acmdln_storage)}, \
    {dll, "__p__wcmdln", reinterpret_cast<void*>(&game_p_wcmdln_storage)}, \
    {dll, "_get_narrow_winmain_command_line", reinterpret_cast<void*>(&game_get_narrow_winmain_command_line)}, \
    {dll, "_get_wide_winmain_command_line", reinterpret_cast<void*>(&game_get_wide_winmain_command_line)}
#define EXIT_PROCESS(dll) {dll, "ExitProcess", reinterpret_cast<void*>(&game_exit_process)}
#define THREAD_AFFINITY(dll) {dll, "SetThreadAffinityMask", reinterpret_cast<void*>(&game_set_thread_affinity_mask)}
#define THREAD_GROUP_AFFINITY(dll) \
    {dll, "SetThreadGroupAffinity", reinterpret_cast<void*>(&game_set_thread_group_affinity)}
#define POOL_AFFINITY(dll) \
    {dll, "SetThreadpoolAffinityMask", reinterpret_cast<void*>(&game_set_threadpool_affinity_mask)}
#define COMMAND_LINE(dll) \
    {dll, "GetCommandLineA", reinterpret_cast<void*>(&game_get_command_line_a)}, \
    {dll, "GetCommandLineW", reinterpret_cast<void*>(&game_get_command_line_w)}
}  // namespace

SHIM_TABLE(process,
    UCRT("api-ms-win-crt-runtime-l1-1-0.dll"),
    UCRT("ucrtbase.dll"),
    UCRT("ucrtbased.dll"),  // the Debug UCRT: a Debug build of the stand-in game (run_tiny_game) links it
    {"msvcrt.dll", "exit", reinterpret_cast<void*>(&game_exit)},
    {"msvcrt.dll", "_exit", reinterpret_cast<void*>(&game_underscore_exit)},
    {"msvcrt.dll", "atexit", reinterpret_cast<void*>(&game_msvcrt_atexit)},
    {"msvcrt.dll", "_onexit", reinterpret_cast<void*>(&game_onexit)},
    {"msvcrt.dll", "_cexit", reinterpret_cast<void*>(&game_cexit)},
    {"msvcrt.dll", "_c_exit", reinterpret_cast<void*>(&game_c_exit)},
    {"msvcrt.dll", "abort", reinterpret_cast<void*>(&game_abort)},
    {"msvcrt.dll", "__getmainargs", reinterpret_cast<void*>(&game_getmainargs)},
    {"msvcrt.dll", "__wgetmainargs", reinterpret_cast<void*>(&game_wgetmainargs)},
    EXIT_PROCESS("kernel32.dll"),
    EXIT_PROCESS("kernelbase.dll"),
    EXIT_PROCESS("api-ms-win-core-processthreads-l1-1-0.dll"),
    THREAD_AFFINITY("kernel32.dll"),
    THREAD_AFFINITY("kernelbase.dll"),
    THREAD_AFFINITY("api-ms-win-core-processthreads-l1-1-0.dll"),
    THREAD_AFFINITY("api-ms-win-core-processthreads-l1-1-1.dll"),
    THREAD_AFFINITY("api-ms-win-core-processthreads-l1-1-2.dll"),
    THREAD_AFFINITY("api-ms-win-core-processthreads-l1-1-3.dll"),
    THREAD_GROUP_AFFINITY("kernel32.dll"),
    THREAD_GROUP_AFFINITY("kernelbase.dll"),
    THREAD_GROUP_AFFINITY("api-ms-win-core-processtopology-l1-1-0.dll"),
    THREAD_GROUP_AFFINITY("api-ms-win-core-processtopology-l1-1-1.dll"),
    THREAD_GROUP_AFFINITY("api-ms-win-core-processtopology-l1-1-2.dll"),
    POOL_AFFINITY("kernel32.dll"),
    POOL_AFFINITY("kernelbase.dll"),
    COMMAND_LINE("kernel32.dll"),
    COMMAND_LINE("kernelbase.dll"),
    COMMAND_LINE("api-ms-win-core-processenvironment-l1-1-0.dll"),
);
