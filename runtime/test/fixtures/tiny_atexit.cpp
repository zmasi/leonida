// A stand-in program for `gta6rt --run`: atexit handlers, static destructors and the ways a program ends.
// Test code, not game content. check_atexit.cmake runs it natively and through the loader with each mode below
// and compares the lines it prints.
//   return        main returns: handlers and static destructors run, in reverse order of registration
//   exit          exit(7): the same
//   _exit         _exit(8): no handlers, no destructors
//   quick_exit    quick_exit(9): no atexit handlers, no destructors
//   ExitProcess   ExitProcess(10): no handlers, no destructors
//   cexit         _cexit() runs every handler and returns; main then returns 11
//   c_exit        _c_exit() runs none and returns; main then returns 12, and exit runs them all
//   concurrent_exit  a second thread calls _exit(7) while exit(7) runs a handler: it waits for them all
//   thread_local_exit   a thread_local object of the main thread, then exit(13): it ends first, then the handlers
//   thread_local_cexit  the same object, then _cexit(): the same order, and main then returns 14
// The thread_local destructor reaches the CRT through _register_thread_local_exe_atexit_callback, which the EXE's
// startup calls only when the image has one (this program has one in every mode, constructed only where used).
//   at_quick_exit    two at_quick_exit handlers, then quick_exit(15): they run newest first, the atexit ones do not
//   at_quick_exit_exit  the same handlers, then exit(16): only the atexit handlers run
//   at_quick_exit__exit the same handlers, then _exit(17): none runs
//   concurrent_cexit    a second thread (with a thread_local of its own) calls _cexit() while main's _cexit() runs a
//                       slow handler: the CRT's exit lock holds it until the drain is done; main then returns 18
//   quick_exit_register a second thread calls at_quick_exit() while quick_exit(19) runs a slow handler: it waits
//                       for the drain, so its handler never runs
//   quick_exit_throw    an at_quick_exit handler throws: terminate(), so abort() and exit code 3, nothing else runs
//   exit_throw          an atexit handler throws: the same
// One handler registers another while the handlers run: it must still run, next.
#include <windows.h>
#include <process.h>  // _cexit, _c_exit

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void say(const char* what) {
    std::printf("tiny_atexit: %s\n", what);
    std::fflush(stdout);  // every line reaches the pipe before any way out, natively and under the loader
}

struct Tracer {
    const char* name;
    explicit Tracer(const char* n) : name(n) {
        std::printf("tiny_atexit: ctor %s\n", name);
        std::fflush(stdout);
    }
    ~Tracer() {
        std::printf("tiny_atexit: dtor %s\n", name);
        std::fflush(stdout);
    }
};
Tracer g_first("first");
Tracer g_second("second");

void handler_one() { say("atexit one"); }
void handler_two() { say("atexit two"); }
void handler_late() { say("atexit late"); }
void handler_registers() {
    say("atexit registers late");
    std::atexit(handler_late);
}

// The concurrent modes: a slow handler on the main thread starts a second thread's call and waits until that thread
// is about to make it (g_second_calling, set just before the call), so every line the second thread prints before
// the call comes before "... done" under any scheduling. With the exit lock, the call then waits for the main
// thread's cleanup, and the sequence is fixed. The Sleep only gives the second thread time to reach the lock: if it
// were held off longer, a missing lock would go unseen (a false pass); a present one can never fail for it.
HANDLE g_slow_started = nullptr;
HANDLE g_second_calling = nullptr;
void wait_for_second_call() {
    WaitForSingleObject(g_second_calling, INFINITE);
    Sleep(300);
}

// concurrent_exit: while exit() runs this handler on the main thread, a second thread calls _exit(). The CRT lets
// it go once the handlers are done, so either call may set the exit code: both use 7.
void handler_slow() {
    say("slow handler started");
    SetEvent(g_slow_started);
    wait_for_second_call();
    say("slow handler done");
}
DWORD WINAPI quick_exit_thread(void*) {
    WaitForSingleObject(g_slow_started, INFINITE);
    SetEvent(g_second_calling);
    _exit(7);
}

void quick_one() { say("at_quick_exit one"); }
void quick_two() { say("at_quick_exit two"); }
void quick_late() { say("at_quick_exit late"); }

// A thread_local of a second thread: the CRT's thread-local callback ends it when that thread calls _cexit().
Tracer& second_thread_tracer() {
    thread_local Tracer tracer("thread_local second");
    return tracer;
}

// concurrent_cexit: main's _cexit() is running the slow handler when a second thread, with a thread_local of its
// own, calls _cexit(). The exit lock holds it until main's drain is done: its thread_local destructor then runs,
// the table is empty, and it returns. main waits for it before its last line, so the order is fixed.
HANDLE g_second_done = nullptr;
DWORD WINAPI cexit_thread(void*) {
    WaitForSingleObject(g_slow_started, INFINITE);
    (void)second_thread_tracer();
    say("second _cexit entered");
    SetEvent(g_second_calling);
    _cexit();
    say("second _cexit returned");
    SetEvent(g_second_done);
    return 0;
}

// quick_exit_register: quick_exit() is running this slow handler when a second thread registers another one. The
// exit lock holds the registration until the drain is done, so the new handler never runs (natively, the process
// is gone before the registration returns).
void quick_slow() {
    say("quick slow started");
    SetEvent(g_slow_started);
    wait_for_second_call();
    say("quick slow done");
}
DWORD WINAPI register_thread(void*) {
    WaitForSingleObject(g_slow_started, INFINITE);
    say("second registering");
    SetEvent(g_second_calling);
    std::at_quick_exit(quick_late);
    return 0;
}

// quick_exit_throw, exit_throw: a handler that lets a C++ exception escape. The CRT calls terminate(), whose default
// handler aborts: exit code 3. No error box or fault report for the native run (a headless test).
struct Thrown {};
void quick_throws() {
    say("quick throw");
    throw Thrown{};
}
void handler_throws() {
    say("atexit throw");
    throw Thrown{};
}
void headless_abort() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
}

}  // namespace

int main(int argc, char** argv) {
    const char* mode = argc > 1 ? argv[1] : "return";
    std::atexit(handler_one);
    std::atexit(handler_registers);
    std::atexit(handler_two);
    say("main");
    g_slow_started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_second_calling = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!std::strcmp(mode, "concurrent_exit")) {
        std::atexit(handler_slow);
        CloseHandle(CreateThread(nullptr, 0, quick_exit_thread, nullptr, 0, nullptr));
        std::exit(7);
    }
    if (!std::strncmp(mode, "thread_local_", 13)) {
        thread_local Tracer main_thread_local("thread_local");  // constructed here, on the main thread only
        if (!std::strcmp(mode, "thread_local_exit")) std::exit(13);
        if (!std::strcmp(mode, "thread_local_cexit")) {
            _cexit();
            say("after _cexit");
            return 14;
        }
    }
    if (!std::strcmp(mode, "exit")) std::exit(7);
    if (!std::strcmp(mode, "_exit")) _exit(8);
    if (!std::strcmp(mode, "quick_exit")) std::quick_exit(9);
    if (!std::strcmp(mode, "ExitProcess")) ExitProcess(10);
    if (!std::strcmp(mode, "cexit")) {
        _cexit();
        say("after _cexit");
        return 11;
    }
    if (!std::strcmp(mode, "c_exit")) {
        _c_exit();
        say("after _c_exit");
        return 12;
    }
    if (!std::strncmp(mode, "at_quick_exit", 13)) {
        std::at_quick_exit(quick_one);
        std::at_quick_exit(quick_two);
        if (!std::strcmp(mode, "at_quick_exit")) std::quick_exit(15);
        if (!std::strcmp(mode, "at_quick_exit_exit")) std::exit(16);
        if (!std::strcmp(mode, "at_quick_exit__exit")) _exit(17);
    }
    if (!std::strcmp(mode, "concurrent_cexit")) {
        g_second_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::atexit(handler_slow);
        CloseHandle(CreateThread(nullptr, 0, cexit_thread, nullptr, 0, nullptr));
        _cexit();
        WaitForSingleObject(g_second_done, INFINITE);
        say("after _cexit");
        return 18;
    }
    if (!std::strcmp(mode, "quick_exit_register")) {
        std::at_quick_exit(quick_one);
        std::at_quick_exit(quick_slow);
        CloseHandle(CreateThread(nullptr, 0, register_thread, nullptr, 0, nullptr));
        std::quick_exit(19);
    }
    if (!std::strcmp(mode, "quick_exit_throw")) {
        headless_abort();
        std::at_quick_exit(quick_one);
        std::at_quick_exit(quick_throws);
        std::quick_exit(20);
    }
    if (!std::strcmp(mode, "exit_throw")) {
        headless_abort();
        std::atexit(handler_throws);
        std::exit(21);
    }
    return 3;
}
