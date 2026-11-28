#include "thread_stacks.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

#include "../shims/shims.h"
#include "../shims/startup_trace.h"

namespace rt {
namespace {

using Clock = std::chrono::steady_clock;
using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using QueryThread = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);

struct CopiedStack {
    DWORD64 copy = 0;      // Where the bytes are now.
    DWORD64 original = 0;  // Where they were in the thread: [original, original + size).
    size_t size = 0;
};

// Suspended section: system calls and a copy only.
bool copy_context_and_stack(HANDLE thread, CONTEXT& context, std::byte* buffer, CopiedStack& stack) {
    if (SuspendThread(thread) == DWORD(-1)) return false;
    context.ContextFlags = CONTEXT_FULL;
    MEMORY_BASIC_INFORMATION region{};
    bool copied = false;
    if (GetThreadContext(thread, &context) &&
        VirtualQuery(reinterpret_cast<void*>(context.Rsp), &region, sizeof(region)) &&
        region.State == MEM_COMMIT && !(region.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        // From RSP (rounded down to keep the 16-byte alignment the copy buffer also has) to the top of the committed
        // run of stack pages, at most kMaxStackBytes.
        const DWORD64 base = reinterpret_cast<DWORD64>(region.BaseAddress);
        stack.original = context.Rsp & ~DWORD64(15);
        if (region.RegionSize > 0 && region.RegionSize <= std::numeric_limits<DWORD64>::max() - base) {
            const DWORD64 top = base + region.RegionSize;
            if (stack.original >= base && top > stack.original) {
                stack.size = static_cast<size_t>(std::min<DWORD64>(StackSampler::kMaxStackBytes, top - stack.original));
                SIZE_T read = 0;
                copied = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(stack.original), buffer,
                                           stack.size, &read) &&
                         read == stack.size;
            }
        }
    }
    ResumeThread(thread);
    stack.copy = reinterpret_cast<DWORD64>(buffer);
    return copied;
}

// A frame's code can be unloaded after its thread resumes and a damaged stack can hand the unwinder a bad pointer:
// both are access violations here, and end the walk.
bool unwind_frame(CONTEXT& context) {
    __try {
        DWORD64 image_base = 0;
        if (const auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr)) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, const_cast<PRUNTIME_FUNCTION>(entry),
                             &context, &handler_data, &establisher, nullptr);
        } else {
            // No function entry: a leaf (or a thunk without unwind data) that has not moved RSP.
            context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
            context.Rsp += sizeof(DWORD64);
        }
    } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR
                    ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return false;
    }
    return true;
}

size_t walk_copy(CONTEXT& context, const CopiedStack& stack, std::uint64_t* frames) {
    // An address in the copied range now names the same word in the copy.
    const auto rebase = [&](DWORD64& address) {
        if (address >= stack.original && address - stack.original < stack.size) address += stack.copy - stack.original;
    };
    const auto in_copy = [&](DWORD64 address) {
        return stack.size >= sizeof(DWORD64) && address >= stack.copy &&
               address - stack.copy <= stack.size - sizeof(DWORD64);
    };
    size_t count = 0;
    rebase(context.Rsp);
    while (context.Rip && count < StackSampler::kMaxFrames) {
        frames[count++] = context.Rip;
        if (!in_copy(context.Rsp)) break;
        const DWORD64 rsp = context.Rsp;
        // Any nonvolatile register can be a frame pointer, including one just restored from the copy's bytes.
        for (DWORD64* reg : {&context.Rbx, &context.Rbp, &context.Rsi, &context.Rdi,
                             &context.R12, &context.R13, &context.R14, &context.R15})
            rebase(*reg);
        if (!unwind_frame(context)) break;
        rebase(context.Rsp);  // A machine frame restores RSP from the copy as well.
        if (context.Rsp <= rsp) break;
    }
    return count;
}

std::string thread_label(HANDLE thread, GetDescription describe, QueryThread query) {
    if (describe) {
        PWSTR description = nullptr;
        if (SUCCEEDED(describe(thread, &description)) && description) {
            const size_t wide_len = wcslen(description);
            const int clamped = static_cast<int>(
                std::min<size_t>(wide_len, static_cast<size_t>(std::numeric_limits<int>::max())));
            std::string name = startup_trace::detail::utf8_from_wide(description, clamped);
            LocalFree(description);
            if (!name.empty()) return name;
        }
    }
    constexpr int kWin32StartAddress = 9;  // ThreadQuerySetWin32StartAddress
    DWORD64 start = 0;
    if (query && query(thread, kWin32StartAddress, &start, sizeof(start), nullptr) >= 0 && start)
        return "start " + shims::module_offset(start);
    return "start unknown";
}

struct ThreadTrace {
    DWORD tid = 0;
    std::string label;
    std::array<std::uint64_t, StackSampler::kMaxFrames> frames{};
    size_t count = 0;
};

void print_traces(const std::string& tag, const std::vector<ThreadTrace>& traces) {
    std::string out = "[rt] stacks " + tag + ": " + std::to_string(traces.size()) + " threads\n";
    for (auto it = traces.begin(); it != traces.end(); ++it) {
        out += "[rt] stacks " + tag + " thread " + std::to_string(it->tid) + " (" + it->label + "):\n";
        const auto same = std::find_if(traces.begin(), it, [&](const ThreadTrace& earlier) {
            return it->count && it->count == earlier.count &&
                   std::equal(it->frames.begin(), it->frames.begin() + it->count, earlier.frames.begin());
        });
        if (same != it) {
            out += "   same as thread " + std::to_string(same->tid) + "\n";
        } else if (!it->count) {
            out += "   stack unavailable\n";
        } else {
            for (size_t i = 0; i < it->count; ++i)
                out += "   #" + std::to_string(i) + " " + shims::module_offset(it->frames[i]) + "\n";
        }
    }
    std::fputs(out.c_str(), stderr);
    std::fflush(stderr);
}

void snapshot_threads(const std::string& tag, StackSampler& sampler, GetDescription describe, QueryThread query,
                      DWORD only = 0) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "[rt] stacks %s: thread list failed (error %lu)\n", tag.c_str(), GetLastError());
        return;
    }
    std::vector<ThreadTrace> traces;
    traces.reserve(512);
    const DWORD pid = GetCurrentProcessId();
    const DWORD self = GetCurrentThreadId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        entry.dwSize = sizeof(entry);
        if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
        if (only && entry.th32ThreadID != only) continue;
        const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                         FALSE, entry.th32ThreadID);
        if (!thread) continue;
        if (GetProcessIdOfThread(thread) == pid) {  // The listed thread may have exited and its ID been reused.
            ThreadTrace& trace = traces.emplace_back();
            trace.tid = entry.th32ThreadID;
            trace.label = thread_label(thread, describe, query);  // Before sampling: it allocates.
            trace.count = sampler.sample(thread, trace.frames.data());
        }
        CloseHandle(thread);
    }
    CloseHandle(snapshot);
    print_traces(tag, traces);
}

}  // namespace

StackSampler::StackSampler() : stack_(std::make_unique_for_overwrite<std::byte[]>(kMaxStackBytes)) {}

std::size_t StackSampler::sample(void* thread, std::uint64_t* frames) {
    if (!thread || !frames) return 0;
    CONTEXT context{};
    CopiedStack stack;
    if (!copy_context_and_stack(static_cast<HANDLE>(thread), context, stack_.get(), stack)) {
        if (!context.Rip) return 0;
        frames[0] = context.Rip;  // The context without its stack: the innermost frame only.
        return 1;
    }
    return walk_copy(context, stack, frames);
}

bool parse_stack_times(std::string_view text, std::vector<unsigned>& seconds) {
    seconds.clear();
    for (;;) {
        const auto comma = text.find(',');
        const auto token = text.substr(0, comma);
        unsigned value = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
        if (token.empty() || error != std::errc{} || end != token.data() + token.size()) {
            seconds.clear();
            return false;
        }
        seconds.push_back(value);
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    std::sort(seconds.begin(), seconds.end());
    seconds.erase(std::unique(seconds.begin(), seconds.end()), seconds.end());
    return true;
}

void print_thread_stacks_now(std::uint32_t tid, const char* why) noexcept {
    static std::mutex mutex;
    try {
        std::lock_guard lock(mutex);
        static StackSampler sampler;
        const auto describe = reinterpret_cast<GetDescription>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
        const auto query = reinterpret_cast<QueryThread>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
        snapshot_threads(why ? why : "now", sampler, describe, query, tid);
    } catch (...) {
    }
}

std::jthread start_thread_stacks() {
    const auto start = Clock::now();
    char spec[1024];
    const DWORD length = GetEnvironmentVariableA("LEONIDA_STACKS", spec, sizeof(spec));
    if (!length) return {};
    std::vector<unsigned> times;
    if (length >= sizeof(spec) || !parse_stack_times(std::string_view(spec, length), times)) {
        std::fprintf(stderr, "[rt] LEONIDA_STACKS: expected comma-separated whole seconds\n");
        return {};
    }
    return std::jthread([start, times = std::move(times)](std::stop_token stop) {
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto describe =
            kernel32 ? reinterpret_cast<GetDescription>(
                           reinterpret_cast<void*>(GetProcAddress(kernel32, "GetThreadDescription")))
                     : nullptr;
        const auto query =
            ntdll ? reinterpret_cast<QueryThread>(
                        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationThread")))
                  : nullptr;
        StackSampler sampler;
        std::condition_variable_any wake;
        std::mutex mutex;
        std::unique_lock lock(mutex);
        for (const unsigned at : times) {
            wake.wait_until(lock, stop, start + std::chrono::seconds(at), [] { return false; });
            if (stop.stop_requested()) break;
            snapshot_threads("t=" + std::to_string(at), sampler, describe, query);
        }
    });
}

}  // namespace rt
