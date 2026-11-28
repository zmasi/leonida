// Checks the registered xapu.dll and chad.dll shims (runtime/shims/xapu_queue.cpp and runtime/shims/xapu_chad.cpp)
// in the call shapes the executable uses and against the layouts xapu_queue.cpp documents.
//
// xapu.dll: the connection's limits and its zeroed memory, writable at the offsets the title reaches; one result per
// accepted command, in command order, with its id echoed; the per-stream queue limit; the error codes, returned or in
// the result; a process command's frame count taken from the Opus packet's table-of-contents byte (RFC 6716, section
// 3.1) and clipped, with silence written; and the result event. The packets are hand-made TOC bytes, not audio.
// chad.dll: XDspActivate hands out zeroed status and stream memory, and both disconnect.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "shims.h"

namespace {

using XApuConnectFn = HRESULT(__stdcall*)(const void*, void*, void**);
using XApuDisconnectFn = HRESULT(__stdcall*)(void*, uint32_t);
using XApuEnqueueCommandFn = HRESULT(__stdcall*)(void*, const void*, uint32_t*);
using XApuGetResultCountFn = uint32_t(__stdcall*)(void*, uint32_t*);
using XApuDequeueResultFn = HRESULT(__stdcall*)(void*, void*);
using XDspConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void**);
using XDspActivateFn = HRESULT(__stdcall*)(void*, const void*, void**, void**);
using XDspDeactivateFn = HRESULT(__stdcall*)(void*, uint64_t, uint64_t, uint64_t);
using XDspDisconnectFn = HRESULT(__stdcall*)(void*);
using XDspSubmitCommandFn = HRESULT(__stdcall*)(void*, void*, uint32_t*);

constexpr HRESULT kQueueFull = static_cast<HRESULT>(0x8AC81000u);
constexpr HRESULT kNoResult = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kInvalidStream = static_cast<HRESULT>(0x8AC81003u);
constexpr HRESULT kAlreadyActive = static_cast<HRESULT>(0x8AC81005u);
constexpr HRESULT kBadPacket = static_cast<HRESULT>(0x8AC81008u);

#pragma pack(push, 1)
struct ConnectInput {
    uint32_t processing, streams, commands_per_stream, memory_bytes, frames_per_output, options;
};
struct ConnectOutput {
    void* memory;
    HANDLE result_event;
};
struct CommandId {
    uint32_t kind, stream, sequence, options;
    uint64_t context;
};
struct ActivateCommand {
    CommandId id;
    uint32_t channels, init_bytes;
    const void* init;
    float start_pitch;
    uint32_t processing_bytes;
    void* processing;
};
struct ProcessCommand {
    CommandId id;
    float pitch_ramp, target_pitch;
    uint32_t first_frame, frame_count, input_bytes;
    const uint8_t* input;
    uint32_t output_bytes;
    uint8_t* output;
};
struct Result {
    CommandId id;
    uint32_t state;
    uint64_t state_data, usec;
    HRESULT error;
    uint32_t frames, output_bytes;
    uint8_t* output;
    float pitch;
    uint32_t input_bytes;
    const uint8_t* input;
};
#pragma pack(pop)
static_assert(sizeof(ActivateCommand) == 56 && sizeof(ProcessCommand) == 64 && sizeof(Result) == 0x50, "layouts");

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

template <typename T>
T get(const char* dll, const char* name) {
    void* fn = rt::shims::find(dll, name);
    check(fn != nullptr, name);
    return reinterpret_cast<T>(fn);
}

CommandId id(uint32_t kind, uint32_t stream, uint32_t options = 0) { return {kind, stream, 7, options, 0x1234}; }

}  // namespace

int main() {
    std::printf("xapu_chad_test: registered xapu.dll and chad.dll shims\n");
    const auto connect = get<XApuConnectFn>("xapu.dll", "XApuConnect");
    const auto disconnect = get<XApuDisconnectFn>("xapu.dll", "XApuDisconnect");
    const auto enqueue = get<XApuEnqueueCommandFn>("xapu.dll", "XApuEnqueueCommand");
    const auto count = get<XApuGetResultCountFn>("xapu.dll", "XApuGetResultCount");
    const auto dequeue = get<XApuDequeueResultFn>("xapu.dll", "XApuDequeueResult");
    const auto dsp_connect = get<XDspConnectFn>("chad.dll", "XDspConnectWithMaximumStreamLimit");
    const auto dsp_activate = get<XDspActivateFn>("chad.dll", "XDspActivate");
    const auto dsp_deactivate = get<XDspDeactivateFn>("chad.dll", "XDspDeactivate");
    const auto dsp_disconnect = get<XDspDisconnectFn>("chad.dll", "XDspDisconnect");
    const auto dsp_submit = get<XDspSubmitCommandFn>("chad.dll", "XDspSubmitCommand");
    check(rt::shims::owns_dll("xapu.dll") && rt::shims::owns_dll("chad.dll"), "the shims own xapu.dll and chad.dll");
    if (g_failures) return 1;

    // XApuConnect(const ConnectInput*, ConnectOutput*, handle*): the first argument is read, never written.
    ConnectOutput out{};
    void* apu = nullptr;
    ConnectInput bad{1, 0, 2, 0x10000, 0, 0};
    check(connect(&bad, &out, &apu) == E_INVALIDARG, "zero streams is E_INVALIDARG");
    bad = {1, 501, 2, 0x10000, 0, 0};
    check(connect(&bad, &out, &apu) == E_INVALIDARG, "more than 500 streams is E_INVALIDARG");
    bad = {1, 4, 17, 0x10000, 0, 0};
    check(connect(&bad, &out, &apu) == E_INVALIDARG, "more than 16 commands per stream is E_INVALIDARG");
    bad = {1, 4, 2, 0, 0, 0};
    check(connect(&bad, &out, &apu) == E_INVALIDARG, "no memory is E_INVALIDARG");
    bad = {1, 4, 2, 0x10000, 256, 0};
    check(connect(&bad, &out, &apu) == E_INVALIDARG, "frames per output for decoding is E_INVALIDARG");
    bad = {2, 4, 2, 0x10000, 0, 0};
    check(connect(&bad, &out, &apu) == static_cast<HRESULT>(rt::shims::kENotImpl),
          "a processing kind other than Opus decoding is E_NOTIMPL");
    check(apu == nullptr, "a failed XApuConnect hands out no handle");

    // The memory the title addresses: input at base + slot * 0x280, output at base + 0x14000 + slot * 0x1E00.
    constexpr uint32_t kMemory = 0x14000 + 128 * 0x1E00;
    const ConnectInput in{1, 128, 2, kMemory, 0, 8};
    const ConnectInput in_copy = in;
    check(SUCCEEDED(connect(&in, &out, &apu)) && apu, "XApuConnect for Opus decoding, 128 streams, 2 commands each");
    check(std::memcmp(&in, &in_copy, sizeof in) == 0, "XApuConnect leaves its input alone");
    check(out.memory != nullptr && out.result_event != nullptr, "it hands out memory and a result event");
    if (!apu || !out.memory) return 1;
    auto* base = static_cast<uint8_t*>(out.memory);
    const size_t last_output = 0x14000 + 127 * 0x1E00 + 0x1E00 - 1;  // the end of slot 127's output
    check(base[0] == 0 && base[last_output] == 0, "the memory is zeroed");
    base[127 * 0x280] = 1;
    base[last_output] = 1;
    check(base[127 * 0x280] == 1 && base[last_output] == 1, "the memory is writable at slot 127");
    base[127 * 0x280] = 0;
    base[last_output] = 0;

    Result r{};
    check(dequeue(apu, &r) == kNoResult, "XApuDequeueResult with nothing queued is 0x8AC81001");
    uint32_t dequeued = 99;
    check(count(apu, &dequeued) == 0 && dequeued == 0, "XApuGetResultCount: nothing ready, none dequeued");

    // Activate stream 3 (stereo) and process one packet; the title's sites pass a null third argument.
    const ActivateCommand activate{id(1, 3), 2, 0, nullptr, 1.0f, 0, nullptr};
    check(enqueue(apu, &activate, nullptr) == S_OK, "activate stream 3 with a null count pointer");
    check(WaitForSingleObject(out.result_event, 0) == WAIT_TIMEOUT, "a command not asking for it leaves the event");
    uint8_t* const packet = base + 3 * 0x280;
    uint8_t* const pcm = base + 0x14000 + 3 * 0x1E00;
    packet[0] = (31 << 3) | 0;  // one 20 ms CELT frame (config 31, code 0): 960 frames per channel
    std::memset(pcm, 0x55, 0x1E00 + 16);  // the slot and the start of the next, to see nothing past it is written
    ProcessCommand process{id(3, 3, 1), 0.0f, 1.0f, 0, 960, 40, packet, 960 * 8, pcm};
    uint32_t total = 0;
    check(enqueue(apu, &process, &total) == S_OK && total == 2, "process on stream 3; two commands accepted");
    check(WaitForSingleObject(out.result_event, 0) == WAIT_OBJECT_0, "a command asking for it signals the event");
    check(WaitForSingleObject(out.result_event, 0) == WAIT_TIMEOUT, "the event resets itself");
    check(enqueue(apu, &process, &total) == kQueueFull && total == 2,
          "a third command on a stream of two is 0x8AC81000 and not counted");
    check(count(apu, nullptr) == 2, "two results ready, with a null dequeued pointer");

    check(dequeue(apu, &r) == S_OK && r.id.kind == 1 && r.id.stream == 3 && r.id.sequence == 7 &&
              r.id.context == 0x1234 && r.error == S_OK && r.pitch == 1.0f,
          "results come in command order: the activate first, its id echoed");
    check(dequeue(apu, &r) == S_OK && r.id.kind == 3 && r.error == S_OK, "the process result follows");
    uint32_t dword34 = 0;
    std::memcpy(&dword34, reinterpret_cast<const uint8_t*>(&r) + 0x34, 4);
    check(r.frames == 960 && dword34 == 960 * 8 && r.output == pcm && r.input == packet && r.input_bytes == 40,
          "960 stereo float frames, the byte count at +0x34, the buffers echoed");
    bool silent = true;
    for (int i = 0; i < 960 * 8; ++i) silent = silent && pcm[i] == 0;
    check(silent && pcm[960 * 8] == 0x55, "the output range is silence, and nothing past it is written");
    check(count(apu, &dequeued) == 0 && dequeued == 2, "nothing ready, two dequeued");

    // The TOC byte's frame size and count (RFC 6716, section 3.1), one stereo packet each.
    struct Toc {
        uint8_t toc, count;
        uint32_t frames;
        const char* what;
    } const tocs[] = {
        {(0 << 3) | 0, 0, 480, "SILK 10 ms, code 0: 480 frames"},
        {(3 << 3) | 1, 0, 5760, "SILK 60 ms, code 1 (two frames): 5760 frames"},
        {(13 << 3) | 2, 0, 1920, "hybrid 20 ms, code 2 (two frames): 1920 frames"},
        {(16 << 3) | 3, 5, 600, "CELT 2.5 ms, code 3 with five frames: 600 frames"},
    };
    for (const Toc& t : tocs) {
        packet[0] = t.toc;
        packet[1] = t.count;
        process = {id(3, 3), 0.0f, 1.0f, 0, 6000, 40, packet, 0x1E00 * 4, pcm};
        r = {};
        check(enqueue(apu, &process, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.error == S_OK &&
                  r.frames == (t.frames < 0x1E00 * 4 / 8 ? t.frames : 0x1E00 * 4 / 8),
              t.what);
    }

    // first_frame and frame_count clip like a decode: three 20 ms frames (code 3, count 3) = 2880, from frame 2000.
    packet[0] = (31 << 3) | 3;
    packet[1] = 3;
    process = {id(3, 3), 0.0f, 1.0f, 2000, 4000, 40, packet, 0x1E00, pcm};
    enqueue(apu, &process, nullptr);
    dequeue(apu, &r);
    check(r.error == S_OK && r.frames == 880 && r.output_bytes == 880 * 8, "frames from first_frame to the packet's end");
    process.frame_count = 300;
    enqueue(apu, &process, nullptr);
    dequeue(apu, &r);
    check(r.frames == 300, "no more than frame_count");
    process.frame_count = 4000;
    process.output_bytes = 100 * 8 + 7;
    enqueue(apu, &process, nullptr);
    dequeue(apu, &r);
    check(r.frames == 100 && r.output_bytes == 800, "and no more than the output holds");

    // Errors carried in the result: the command is accepted and its result says what went wrong.
    packet[1] = 7;  // seven 20 ms frames: 140 ms, longer than an Opus packet may be
    enqueue(apu, &process, nullptr);
    dequeue(apu, &r);
    check(r.error == kBadPacket && r.frames == 0, "an impossible packet: 0x8AC81008 in the result");
    packet[0] = (31 << 3) | 3;
    packet[1] = 0;
    enqueue(apu, &process, nullptr);
    dequeue(apu, &r);
    check(r.error == kBadPacket, "a code 3 packet of zero frames: 0x8AC81008");
    ProcessCommand empty = process;
    empty.input_bytes = 0;
    check(enqueue(apu, &empty, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.error == E_INVALIDARG,
          "an empty input: E_INVALIDARG in the result");
    empty = process;
    empty.output = nullptr;
    check(enqueue(apu, &empty, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.error == E_INVALIDARG,
          "a null output: E_INVALIDARG in the result");
    ProcessCommand stray = process;
    stray.id.stream = 4;
    check(enqueue(apu, &stray, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.error == kInvalidStream,
          "a process on a stream never activated: 0x8AC81003 in the result");
    check(enqueue(apu, &activate, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.error == kAlreadyActive,
          "activating an active stream: 0x8AC81005 in the result");
    ActivateCommand surround = activate;
    surround.id.stream = 5;
    surround.channels = 6;
    check(enqueue(apu, &surround, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.error == E_INVALIDARG,
          "six channels: E_INVALIDARG in the result");

    // Errors returned at once, with no result.
    stray.id.stream = 128;
    check(enqueue(apu, &stray, nullptr) == kInvalidStream, "a stream past the connection's is 0x8AC81003");
    const CommandId unknown = id(9, 3);
    check(enqueue(apu, &unknown, nullptr) == kInvalidCommand, "an unknown kind is 0x8AC81002");
    check(count(apu, nullptr) == 0, "neither left a result");

    // Notify and reset give a result; deactivate gives one and closes the stream.
    for (uint32_t kind : {4u, 5u, 2u}) {
        const CommandId command = id(kind, 3);
        check(enqueue(apu, &command, nullptr) == S_OK && dequeue(apu, &r) == S_OK && r.id.kind == kind &&
                  r.id.stream == 3 && r.error == S_OK,
              "notify, reset and deactivate each give a result of their kind for their stream");
    }
    process.id.stream = 3;
    enqueue(apu, &process, nullptr);
    dequeue(apu, &r);
    check(r.error == kInvalidStream, "after deactivate the stream takes no process command");
    check(enqueue(apu, &activate, &total) == S_OK && dequeue(apu, &r) == S_OK && r.error == S_OK,
          "and it can be activated again");
    check(disconnect(apu, 0x20) == S_OK, "XApuDisconnect(handle, 0x20)");
    // A handle that is not a live connection is refused, not freed or read (kevincommits' Finding 19 in #1608,
    // item 5): a second disconnect used to free it again and close its event twice.
    {
        alignas(16) static uint8_t foreign[256] = {};
        uint32_t ignored = 0;
        check(disconnect(apu, 0x20) == E_INVALIDARG, "a second XApuDisconnect of the handle is E_INVALIDARG");
        check(enqueue(apu, &activate, &ignored) == E_INVALIDARG && dequeue(apu, &r) == E_INVALIDARG &&
                  count(apu, &ignored) == 0,
              "a disconnected handle takes no command and has no results");
        check(disconnect(foreign, 0x20) == E_INVALIDARG && enqueue(foreign, &activate, &ignored) == E_INVALIDARG,
              "a handle XApuConnect never made is refused");
    }

    // XDspConnectWithMaximumStreamLimit(buffer, u32, u32, 0x80, 1, handle*), success read as a return of 0.
    alignas(64) static uint8_t dsp_buffer[4096];
    void* dsp = nullptr;
    check(dsp_connect(dsp_buffer, 0, 0, 0x80, 1, &dsp) == 0 && dsp, "XDspConnectWithMaximumStreamLimit returns 0 and a handle");
    const uint8_t description[0x1c] = {};
    uint32_t* status = nullptr;
    void* stream = nullptr;
    check(dsp_activate(dsp, description, reinterpret_cast<void**>(&status), &stream) == 0, "XDspActivate returns 0");
    check(status && stream && static_cast<void*>(status) != stream, "XDspActivate writes both outputs");
    uint32_t dsp_id = 0;
    check(dsp_submit(stream, dsp_buffer, &dsp_id) == 0, "XDspSubmitCommand returns 0");
    // What the title's wait reads: no error at +0xc and the submitted id at +4.
    check(status && status[3] == 0 && status[1] == dsp_id, "the status block reports the submitted command as done");
    check(dsp_deactivate(dsp, 0, 0, 0) == 0, "XDspDeactivate returns 0");
    check(dsp_disconnect(dsp) == 0, "XDspDisconnect returns 0");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
