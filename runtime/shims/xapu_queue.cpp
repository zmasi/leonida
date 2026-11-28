// xapu.dll: the console's audio processing unit (APU) command queue. No PC has one, so this keeps the queue the title
// talks to - one result per accepted command, in command order, with the console's error codes. With
// runtime/opus_decoder built (LEONIDA_HAVE_OPUS_DECODER) a process command decodes its Opus packet to PCM; without
// it the result carries the frame count its table-of-contents byte gives and silence in its output.
//
// Read from the executable's call sites (counts in docs/runtime_imports.md, shapes in docs/audio_hal_exe_calls.md),
// statically, never run:
//   XApuConnect(input24*, out16*, handle*)    1 site; success is SUCCEEDED(hr). The first qword of the 16-byte
//                                             output is read later as the base of a CPU-visible buffer: per-stream
//                                             input at base + slot * 0x280 and output at base + 0x14000 +
//                                             slot * 0x1E00. The first argument is a 24-byte global of the title
//                                             that other title code also addresses.
//   XApuEnqueueCommand(handle, cmd*, 0)       4 sites; the return value is never read. cmd's first dword is a type
//                                             (1, 2 and 3 seen), the second a stream slot.
//   XApuGetResultCount(handle, 0)             1 site; 0 skips the loop that dequeues.
//   XApuDequeueResult(handle, out*)           1 site, called count times into a zeroed 0x50-byte local; a negative
//                                             HRESULT skips that result. The loop reads a type at +0, a stream slot
//                                             at +4 and a dword at +0x34. Its per-slot state machine takes a type 1
//                                             result as "opened" and returns the slot to the title's free list only
//                                             on a type 2 result, after the title enqueued a type 2 command.
//   XApuDisconnect(handle, 0x20)              1 site; the return value is never read.
//
// Where the layouts come from. The signatures, structure layouts, command kinds, option bits and error codes below
// are facts recorded by Marvisak/VIctory (GPL-3.0-or-later, crates/victory-hle/src/modules/apu.rs at commit ffe310e),
// whose runtime runs the retail executable through these calls; see docs/related_projects.md. Nothing of its code is
// copied; this file is written from those facts. They agree with every shape above: the first argument of
// XApuConnect is an input, the dword the title reads at +0x34 of a result is its output byte count, and a type 1
// (activate) or type 2 (deactivate) command gives a result of the same kind for the same stream.
//   HRESULT XApuConnect(const ConnectInput*, ConnectOutput*, Connection** out)
//   HRESULT XApuDisconnect(Connection*, uint32_t)          the second argument is not read
//   HRESULT XApuEnqueueCommand(Connection*, const void* command, uint32_t* accepted_so_far)   may be null
//   HRESULT XApuDequeueResult(Connection*, Result* out)    0x8AC81001 while no result is ready
//   uint32_t XApuGetResultCount(Connection*, uint32_t* dequeued_so_far)   may be null; returns the results ready
// A connection is for one kind of processing; kind 1 is Opus decoding to 32-bit float PCM at 48 kHz, and any other
// kind is E_NOTIMPL. Every command starts with the 24-byte CommandId. A command refused at once (unknown kind,
// stream out of range, or the stream already holding `commands_per_stream` results not dequeued) gives no result;
// every other command gives one, with its error, if any, in the result.
//
// Without runtime/opus_decoder a process command's frame count comes from the Opus packet's table-of-contents byte
// (RFC 6716, section 3.1), clipped from first_frame, to frame_count and to what output_bytes holds, and that output
// range is written with zeros, so the title sees streams advance at the right rate and the audio stays silent. With
// the crate, fill() decodes the packet and writes its PCM instead.
#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <set>
#include <shared_mutex>
#include <string>
#include <vector>

#if defined(LEONIDA_HAVE_OPUS_DECODER)
#include "leonida_opus_decoder.h"
#endif

#include "shims.h"
#include "startup_trace.h"

namespace {

constexpr HRESULT kNotImpl = static_cast<HRESULT>(rt::shims::kENotImpl);
constexpr HRESULT kQueueFull = static_cast<HRESULT>(0x8AC81000u);
constexpr HRESULT kNoResult = static_cast<HRESULT>(0x8AC81001u);
constexpr HRESULT kInvalidCommand = static_cast<HRESULT>(0x8AC81002u);
constexpr HRESULT kInvalidStream = static_cast<HRESULT>(0x8AC81003u);
constexpr HRESULT kAlreadyActive = static_cast<HRESULT>(0x8AC81005u);
constexpr HRESULT kBadPacket = static_cast<HRESULT>(0x8AC81008u);

constexpr uint32_t kDecodeOpus = 1;  // ConnectInput::processing

constexpr uint32_t kActivate = 1;  // CommandId::kind
constexpr uint32_t kDeactivate = 2;
constexpr uint32_t kProcess = 3;
constexpr uint32_t kReset = 5;  // 4 is notify; neither carries anything to do while nothing is decoded

constexpr uint32_t kSignalWhenDone = 0x1;  // CommandId::options: set the result event when the result is ready
constexpr uint32_t kResultEvent = 0x8;     // ConnectInput::options: create an auto-reset event and hand it out
constexpr uint32_t kDeinterleavedOutput = 0x4;  // ConnectInput::options: split stereo into two planes
constexpr uint32_t kOpusSampleRate = 48000;

constexpr uint32_t kMaxStreams = 500;
constexpr uint32_t kMaxCommandsPerStream = 16;
constexpr uint32_t kMaxFrames = 5760;  // 120 ms at 48 kHz, the longest Opus packet (RFC 6716, section 3.2.5)

#pragma pack(push, 1)
struct ConnectInput {
    uint32_t processing;
    uint32_t streams;
    uint32_t commands_per_stream;
    uint32_t memory_bytes;
    uint32_t frames_per_output;  // 0 for decoding
    uint32_t options;
};
struct ConnectOutput {
    void* memory;  // zeroed, for the title's packets and output
    HANDLE result_event;
};
struct CommandId {
    uint32_t kind;
    uint32_t stream;
    uint32_t sequence;
    uint32_t options;
    uint64_t context;
};
struct ActivateCommand {
    CommandId id;
    uint32_t channels;  // 1 or 2
    uint32_t init_bytes;
    const void* init;
    float start_pitch;
    uint32_t processing_bytes;
    void* processing;
};
struct ProcessCommand {
    CommandId id;
    float pitch_ramp;
    float target_pitch;
    uint32_t first_frame;
    uint32_t frame_count;
    uint32_t input_bytes;
    const uint8_t* input;
    uint32_t output_bytes;
    uint8_t* output;
};
struct Result {
    CommandId id;
    uint32_t state;
    uint64_t state_data;
    uint64_t usec;
    HRESULT error;
    uint32_t frames;
    uint32_t output_bytes;
    uint8_t* output;
    float pitch;
    uint32_t input_bytes;
    const uint8_t* input;
};
#pragma pack(pop)
static_assert(sizeof(ConnectInput) == 24, "six dwords");
static_assert(sizeof(ConnectOutput) == 16, "memory and event");
static_assert(sizeof(CommandId) == 24, "kind, stream, sequence, options, context");
static_assert(sizeof(ActivateCommand) == 56, "CommandId and the activate fields");
static_assert(sizeof(ProcessCommand) == 64, "CommandId and the process fields");
static_assert(sizeof(Result) == 0x50, "the result the title dequeues");
static_assert(offsetof(Result, state) == 0x18 && offsetof(Result, error) == 0x2C &&
                  offsetof(Result, output_bytes) == 0x34 && offsetof(Result, pitch) == 0x40 &&
                  offsetof(Result, input) == 0x48,
              "the offsets the title and the layout agree on");

struct Stream {
    bool active = false;
    uint32_t channels = 0;
    uint32_t pending = 0;  // results queued and not dequeued
#if defined(LEONIDA_HAVE_OPUS_DECODER)
    LeonidaOpusDecoder* decoder = nullptr;
#endif
};

struct Connection {
    void* memory = nullptr;
    HANDLE event = nullptr;
    uint32_t commands_per_stream = 0;
    bool deinterleaved = false;
    std::mutex mutex;
    std::vector<Stream> streams;
    std::deque<Result> results;
    uint32_t accepted = 0;
    uint32_t dequeued = 0;
};

// The connections XApuConnect handed out and XApuDisconnect has not ended. Every call checks its handle here under the
// shared lock and keeps it for the call, so a second XApuDisconnect, or a call with a stale or foreign handle, is
// refused instead of freeing or reading freed memory, and a disconnect waits for the calls in flight.
std::shared_mutex g_live_mu;
std::set<Connection*> g_live;

bool live(Connection* c) { return c && g_live.count(c) != 0; }  // g_live_mu held

// gta6rt --trace FILE: the first call of each function and the first XApuEnqueueCommand of each command kind, with
// the caller, so the first boot shows what the title sends without a line per call.
void note(const char* api, uint64_t value, const void* caller) {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::pair<std::string, uint64_t>> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert({api, value}).second) return;
    }
    rt::startup_trace::event(
        "audio_queue", std::string("\"api\":") + rt::startup_trace::quote(api) +
                           ",\"value\":" + rt::startup_trace::quote(rt::startup_trace::hex(value)) + ",\"caller\":" +
                           rt::startup_trace::quote(rt::startup_trace::where(reinterpret_cast<uint64_t>(caller))));
}

// Samples per channel at 48 kHz in one Opus packet, from its table-of-contents byte and frame count code (RFC 6716,
// section 3.1); 0 for a packet that is not valid. The no-decoder path counts frames this way.
uint32_t opus_packet_frames(const uint8_t* packet, uint32_t bytes) {
    if (!packet || bytes == 0) return 0;
    static constexpr uint32_t kSilk[4] = {480, 960, 1920, 2880};  // 10, 20, 40, 60 ms
    static constexpr uint32_t kHybrid[2] = {480, 960};            // 10, 20 ms
    static constexpr uint32_t kCelt[4] = {120, 240, 480, 960};    // 2.5, 5, 10, 20 ms
    const uint32_t config = packet[0] >> 3;
    const uint32_t per_frame = config < 12 ? kSilk[config & 3] : config < 16 ? kHybrid[config & 1] : kCelt[config & 3];
    uint32_t count = 0;
    switch (packet[0] & 3) {
        case 0: count = 1; break;
        case 1:
        case 2: count = 2; break;
        default:
            if (bytes < 2) return 0;
            count = packet[1] & 0x3F;
            break;
    }
    if (count == 0 || count * per_frame > kMaxFrames) return 0;
    return count * per_frame;
}

// Fills a process command's result: the packet's PCM when runtime/opus_decoder is built, silence (with the frame
// count from the table-of-contents byte) otherwise.
void fill(Connection& connection, Stream& stream, const ProcessCommand& c, Result& r) {
    r.input = c.input;
    r.input_bytes = c.input_bytes;
    r.output = c.output;
    if (!c.input || !c.output || c.input_bytes == 0) {
        r.error = E_INVALIDARG;
        return;
    }
#if defined(LEONIDA_HAVE_OPUS_DECODER)
    if (stream.decoder) {
        const uint32_t channels = leonida_opus_decoder_channels(stream.decoder);
        if (channels == 0) {
            r.error = kInvalidStream;
            return;
        }
        std::vector<float> pcm;
        try {
            pcm.resize(size_t(kMaxFrames) * channels);
        } catch (...) {
            r.error = E_OUTOFMEMORY;
            return;
        }
        uint32_t decoded = 0;
        if (leonida_opus_decode_packet(stream.decoder, c.input, c.input_bytes, pcm.data(), kMaxFrames, &decoded) !=
            LEONIDA_OPUS_OK) {
            r.error = kBadPacket;
            return;
        }
        const uint32_t first = c.first_frame < decoded ? c.first_frame : decoded;
        uint32_t frames = decoded - first;
        if (frames > c.frame_count) frames = c.frame_count;
        const uint32_t room = c.output_bytes / (4 * channels);
        if (frames > room) frames = room;
        const uint32_t bytes = frames * channels * 4;
        if (connection.deinterleaved && channels == 2) {
            float* out = reinterpret_cast<float*>(c.output);
            for (uint32_t i = 0; i < frames; ++i) {
                out[i] = pcm[(first + i) * 2];
                out[frames + i] = pcm[(first + i) * 2 + 1];
            }
        } else if (bytes != 0) {
            std::memcpy(c.output, pcm.data() + first * channels, bytes);
        }
        r.frames = frames;
        r.output_bytes = bytes;
        return;
    }
#endif
    const uint32_t decoded = opus_packet_frames(c.input, c.input_bytes);
    if (decoded == 0) {
        r.error = kBadPacket;
        return;
    }
    const uint32_t first = c.first_frame < decoded ? c.first_frame : decoded;
    uint32_t frames = decoded - first;
    if (frames > c.frame_count) frames = c.frame_count;
    const uint32_t room = c.output_bytes / (4 * stream.channels);
    if (frames > room) frames = room;
    const uint32_t bytes = frames * stream.channels * 4;
    std::memset(c.output, 0, bytes);  // interleaved or not, silence is zeros
    r.frames = frames;
    r.output_bytes = bytes;
}

HRESULT __stdcall XApuConnect(const ConnectInput* input, ConnectOutput* output, Connection** out) {
    note("XApuConnect", input ? input->processing : ~0ull, _ReturnAddress());
    if (!input || !output || !out) return E_INVALIDARG;
    *out = nullptr;
    ConnectInput in;
    std::memcpy(&in, input, sizeof(in));
    if (in.streams == 0 || in.streams > kMaxStreams || in.commands_per_stream == 0 ||
        in.commands_per_stream > kMaxCommandsPerStream || in.memory_bytes == 0)
        return E_INVALIDARG;
    if (in.processing != kDecodeOpus) {
        std::fprintf(stderr, "[rt] xapu: XApuConnect for processing kind %u: only Opus decoding (1); E_NOTIMPL\n",
                     in.processing);
        return kNotImpl;
    }
    if (in.frames_per_output != 0) return E_INVALIDARG;

    auto* c = new (std::nothrow) Connection;  // a throwing new would unwind into the title
    if (!c) return E_OUTOFMEMORY;
    try {
        c->streams.resize(in.streams);
    } catch (...) {
        delete c;
        return E_OUTOFMEMORY;
    }
    c->memory = VirtualAlloc(nullptr, in.memory_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);  // zeroed
    if (!c->memory) {
        delete c;
        return E_OUTOFMEMORY;
    }
    if (in.options & kResultEvent) {
        c->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
        if (!c->event) {
            const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
            VirtualFree(c->memory, 0, MEM_RELEASE);
            delete c;
            return hr;
        }
    }
    c->commands_per_stream = in.commands_per_stream;
    c->deinterleaved = (in.options & kDeinterleavedOutput) != 0;
    try {
        std::unique_lock lock(g_live_mu);
        g_live.insert(c);
    } catch (...) {
        if (c->event) CloseHandle(c->event);
        VirtualFree(c->memory, 0, MEM_RELEASE);
        delete c;
        return E_OUTOFMEMORY;
    }
    const ConnectOutput o{c->memory, c->event};
    std::memcpy(output, &o, sizeof(o));
    *out = c;
#if defined(LEONIDA_HAVE_OPUS_DECODER)
    std::fprintf(stderr, "[rt] xapu: connected for Opus decoding, %u streams; packets decoded\n", in.streams);
#else
    std::fprintf(stderr, "[rt] xapu: connected for Opus decoding, %u streams; packets counted, not decoded\n",
                 in.streams);
#endif
    return S_OK;
}

HRESULT __stdcall XApuDisconnect(Connection* c, uint32_t /*0x20 at the measured site*/) {
    note("XApuDisconnect", 0, _ReturnAddress());
    {
        std::unique_lock lock(g_live_mu);
        if (!live(c)) return E_INVALIDARG;
        g_live.erase(c);
    }
#if defined(LEONIDA_HAVE_OPUS_DECODER)
    for (Stream& s : c->streams)
        if (s.decoder) leonida_opus_decoder_destroy(s.decoder);
#endif
    if (c->event) CloseHandle(c->event);
    VirtualFree(c->memory, 0, MEM_RELEASE);
    delete c;
    return S_OK;
}

HRESULT __stdcall XApuEnqueueCommand(Connection* c, const void* command, uint32_t* accepted_so_far) {
    if (!c || !command) return E_INVALIDARG;
    CommandId id;
    std::memcpy(&id, command, sizeof(id));
    note("XApuEnqueueCommand", id.kind, _ReturnAddress());
    if (id.kind < kActivate || id.kind > kReset) return kInvalidCommand;

    const std::shared_lock live_lock(g_live_mu);
    if (!live(c)) return E_INVALIDARG;
    const std::lock_guard lock(c->mutex);
    if (id.stream >= c->streams.size()) return kInvalidStream;
    Stream& stream = c->streams[id.stream];
    if (stream.pending >= c->commands_per_stream) return kQueueFull;
    // The result's slot first: out of memory refuses the command before it changes the stream.
    try {
        c->results.emplace_back();
    } catch (...) {  // nothing may unwind into the title
        return E_OUTOFMEMORY;
    }

    Result r{};
    r.id = id;
    r.pitch = 1.0f;
    if (id.kind == kActivate) {
        ActivateCommand a;
        std::memcpy(&a, command, sizeof(a));
        if (stream.active) {
            r.error = kAlreadyActive;
        } else if (a.channels < 1 || a.channels > 2) {
            r.error = E_INVALIDARG;
        } else {
            stream.active = true;
            stream.channels = a.channels;
#if defined(LEONIDA_HAVE_OPUS_DECODER)
            if (leonida_opus_decoder_create(kOpusSampleRate, a.channels, &stream.decoder) != LEONIDA_OPUS_OK) {
                stream.active = false;
                stream.channels = 0;
                r.error = E_INVALIDARG;
            }
#endif
        }
    } else if (id.kind == kDeactivate) {
        stream.active = false;
#if defined(LEONIDA_HAVE_OPUS_DECODER)
        if (stream.decoder) {
            leonida_opus_decoder_destroy(stream.decoder);
            stream.decoder = nullptr;
        }
#endif
    } else if (id.kind == kReset) {
#if defined(LEONIDA_HAVE_OPUS_DECODER)
        if (stream.decoder) leonida_opus_decoder_reset(stream.decoder);
#endif
    } else if (id.kind == kProcess) {
        ProcessCommand p;
        std::memcpy(&p, command, sizeof(p));
        if (!stream.active) r.error = kInvalidStream;
        else fill(*c, stream, p, r);
    }
    c->results.back() = r;
    ++stream.pending;
    ++c->accepted;
    if (accepted_so_far) *accepted_so_far = c->accepted;
    if ((id.options & kSignalWhenDone) && c->event) SetEvent(c->event);
    return S_OK;
}

HRESULT __stdcall XApuDequeueResult(Connection* c, Result* out) {
    note("XApuDequeueResult", 0, _ReturnAddress());
    if (!c || !out) return E_INVALIDARG;
    const std::shared_lock live_lock(g_live_mu);
    if (!live(c)) return E_INVALIDARG;
    const std::lock_guard lock(c->mutex);
    if (c->results.empty()) return kNoResult;
    const Result r = c->results.front();
    c->results.pop_front();
    --c->streams[r.id.stream].pending;
    ++c->dequeued;
    std::memcpy(out, &r, sizeof(r));
    return S_OK;
}

uint32_t __stdcall XApuGetResultCount(Connection* c, uint32_t* dequeued_so_far) {
    note("XApuGetResultCount", 0, _ReturnAddress());
    const std::shared_lock live_lock(g_live_mu);
    if (!live(c)) return 0;
    const std::lock_guard lock(c->mutex);
    if (dequeued_so_far) *dequeued_so_far = c->dequeued;
    return static_cast<uint32_t>(c->results.size());
}

}  // namespace

SHIM_TABLE(xapu,
    {"xapu.dll", "XApuConnect", reinterpret_cast<void*>(&XApuConnect)},
    {"xapu.dll", "XApuDequeueResult", reinterpret_cast<void*>(&XApuDequeueResult)},
    {"xapu.dll", "XApuDisconnect", reinterpret_cast<void*>(&XApuDisconnect)},
    {"xapu.dll", "XApuEnqueueCommand", reinterpret_cast<void*>(&XApuEnqueueCommand)},
    {"xapu.dll", "XApuGetResultCount", reinterpret_cast<void*>(&XApuGetResultCount)},
);
