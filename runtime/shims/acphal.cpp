// acphal.dll: Audio Control Processor HAL. Scalable Hardware Audio Processing Engine
// (SHAPE) and APU (Audio Processing Unit) physical memory management for Xbox Series.
//
// Provides:
//   - A VirtualAlloc-backed APU heap (cached + non-cached) with 32-bit device addresses;
//   - SHAPE context arrays sized to the documented GDK strides;
//   - The IAcpHal command and message queue (registration, completion, disconnect,
//     flowgraph acknowledgement, and APU notification word writes);
//   - A background XMA2 decoder pumping enabled SHAPE_XMA_CONTEXT output rings.
//
// Signatures from the public GDK reference (devdocs.xbox.com):
//   AcpHal, APU reference, SHAPE_CONTEXTS, APU_ADDRESS.
// Measured call-site shapes and findings from the retail executable (1.0.2.1):
//   - docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md (Stop 28)
//   - PR #6885 (call sites and parameter analysis), PR #7009 (audio/HAL bring-up).

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "shims.h"
#include "startup_trace.h"
#if defined(LEONIDA_HAVE_XMA2_DECODER)
#include "leonida_xma2_decoder.h"
#endif

namespace {

// ---- APU_ADDRESS (from the GDK) ----

// The console APU uses a 40-bit physical address space.
// On PC we translate to a 32-bit APU address offset.
struct APU_ADDRESS {
    uint64_t value;  // 40-bit physical address
};

// ---- SHAPE context structures (from the public GDK documentation at devdocs.xbox.com) ----
//
// These structures are defined here with their documented GDK field layouts so that
// sizeof() returns the correct per-context byte count. Without the real layouts the
// empty structs would be 1 byte each, and the title's writes (e.g. memset with the
// real stride) would overflow the allocation.

// UINT32 bitfields packed into 32-bit words
struct SHAPE_SRC_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t blocksToSkip : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t samplingIncrement : 21;
    uint32_t reserved0 : 1;
    uint32_t cmd : 2;
    uint32_t mixBufPeakMag2 : 4;
    uint32_t peakMag2 : 4;
    uint32_t sampleCount;
    uint32_t samplePointer : 25;
    uint32_t reserved1 : 7;
    uint32_t samplingIncrementTarget : 21;
    uint32_t reserved2 : 11;
    uint32_t reserved[3];
};
static_assert(sizeof(SHAPE_SRC_CONTEXT) == 32, "SHAPE_SRC_CONTEXT is 32 bytes per GDK");

// The EQ/Comp context: 88 UINT32s = 352 bytes per GDK definition.
#pragma pack(push, 1)
struct alignas(4) SHAPE_EQCOMP_CONTEXT {
    uint8_t data[352];
};
#pragma pack(pop)
static_assert(sizeof(SHAPE_EQCOMP_CONTEXT) == 352, "SHAPE_EQCOMP_CONTEXT is 352 bytes per GDK");

struct SHAPE_FILTVOL_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 2;
    uint32_t internalSaturate : 1;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t delay1 : 30;
    uint32_t headroom : 2;
    uint32_t delay2 : 30;
    uint32_t svfMode : 2;
    uint32_t gainTarget : 16;
    uint32_t gain : 16;
    uint32_t qRecip : 24;
    uint32_t reserved1 : 8;
    uint32_t qRecipTarget : 24;
    uint32_t reserved2 : 8;
    uint32_t fc : 24;
    uint32_t reserved3 : 8;
    uint32_t fcTarget : 24;
    uint32_t reserved4 : 8;
};
static_assert(sizeof(SHAPE_FILTVOL_CONTEXT) == 32, "SHAPE_FILTVOL_CONTEXT is 32 bytes per GDK");

struct SHAPE_DMA_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 3;
    uint32_t mixBufPeakMag : 4;
    uint32_t peakMag : 4;
    uint32_t readPointer : 5;
    uint32_t reserved1 : 3;
    uint32_t writePointer : 5;
    uint32_t reserved2 : 3;
    uint32_t full : 1;
    uint32_t reserved3 : 15;
    uint32_t reserved4 : 9;
    uint32_t address : 23;
    uint32_t numFrames : 5;
    uint32_t reserved5 : 3;
    uint32_t floatConvert : 1;
    uint32_t reserved6 : 7;
    uint32_t numChannels : 3;
    uint32_t reserved7 : 5;
    uint32_t channel : 3;
    uint32_t reserved8 : 5;
};
static_assert(sizeof(SHAPE_DMA_CONTEXT) == 16, "SHAPE_DMA_CONTEXT is 16 bytes per GDK");

struct SHAPE_XMA_CONTEXT {
    uint32_t words[16];  // See the shared word layout above the XMA engine.
};
static_assert(sizeof(SHAPE_XMA_CONTEXT) == 64, "SHAPE_XMA_CONTEXT is 64 bytes per GDK");

struct SHAPE_PCM_CONTEXT {
    uint32_t timestamp : 21;
    uint32_t reserved0 : 11;
    uint32_t loopCount : 8;
    uint32_t reserved1 : 8;
    uint32_t full : 1;
    uint32_t reserved2 : 15;
    uint32_t readPointer : 21;
    uint32_t reserved3 : 11;
    uint32_t loopStartWritePointer : 21;
    uint32_t reserved4 : 11;
    uint32_t loopEnd : 21;
    uint32_t reserved5 : 11;
    uint32_t mode : 1;
    uint32_t reserved6 : 15;
    uint32_t format : 2;
    uint32_t reserved7 : 14;
    uint32_t reserved[2];  // the GDK's 32-byte context: the published field list leaves one trailing word
};
static_assert(sizeof(SHAPE_PCM_CONTEXT) == 32, "SHAPE_PCM_CONTEXT is 32 bytes per GDK");

// The SHAPE_CONTEXTS structure (from the GDK)
struct SHAPE_CONTEXTS {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    SHAPE_SRC_CONTEXT* srcContextArray;
    SHAPE_EQCOMP_CONTEXT* eqCompContextArray;
    SHAPE_FILTVOL_CONTEXT* filtVolContextArray;
    SHAPE_DMA_CONTEXT* dmaContextArray;
    SHAPE_XMA_CONTEXT* xmaContextArray;
    SHAPE_PCM_CONTEXT* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(SHAPE_CONTEXTS) == 120, "SHAPE_CONTEXTS is 120 bytes per GDK");
static_assert(offsetof(SHAPE_CONTEXTS, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38 per retail binary measurement");

// ---- Startup trace helper ----

void trace_call(const char* api, std::string_view detail = {}) noexcept try {
    if (!rt::startup_trace::enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    {
        const std::lock_guard lock(mutex);
        if (!seen.insert(std::string(api).append(detail)).second) return;
    }
    rt::startup_trace::event("acphal",
                             (std::string("\"api\":\"") + api + "\"").append(detail));
} catch (...) {
    // Tracing cannot change the result of a guest-facing call.
}

// ---- Genuine APU Heap Implementation ----

constexpr uint32_t kApuBaseAddress = 0x40000000u;
constexpr uint64_t kApuRegionAlignment = 0x10000u;

uint64_t align_apu_region(uint64_t size) {
    return (size + kApuRegionAlignment - 1) & ~(kApuRegionAlignment - 1);
}

struct ApuRegion {
    void* base = nullptr;
    uint64_t size = 0;
    std::map<uint32_t, uint32_t> blocks;  // offset -> size of active allocations

    bool contains(const void* ptr) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        return p >= b && p < b + size;
    }

    bool is_valid_address(const void* ptr, uint32_t alignment) const {
        if (!base || !ptr) return false;
        const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        const uintptr_t b = reinterpret_cast<uintptr_t>(base);
        if (p < b || p >= b + size) return false;
        if (alignment == 0) alignment = 1;
        return ((p - b) & (static_cast<uintptr_t>(alignment) - 1)) == 0;
    }

    uint32_t to_offset(const void* ptr) const {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ptr) -
                                     reinterpret_cast<uintptr_t>(base));
    }

    void* from_offset(uint32_t offset) const {
        if (offset >= size) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    uint8_t* span_from_offset(uint32_t offset, std::size_t len) const {
        if (!base || len == 0 || offset >= size) return nullptr;
        const uint64_t remaining = size - offset;
        if (len > remaining) return nullptr;
        return static_cast<uint8_t*>(base) + offset;
    }

    void* allocate(uint32_t alloc_size, uint32_t alignment, uint32_t* out_offset) {
        if (alloc_size == 0 || alignment == 0) return nullptr;
        uint64_t at = 0;
        for (auto it = blocks.begin();; ++it) {
            at = (at + alignment - 1) & ~static_cast<uint64_t>(alignment - 1);
            const uint64_t limit = (it == blocks.end()) ? size : it->first;
            if (at + alloc_size <= limit) break;
            if (it == blocks.end()) return nullptr;  // out of memory
            at = static_cast<uint64_t>(it->first) + it->second;
        }
        const uint32_t offset = static_cast<uint32_t>(at);
        blocks.emplace(offset, alloc_size);
        uint8_t* ptr = static_cast<uint8_t*>(base) + offset;
        std::memset(ptr, 0, alloc_size);
        if (out_offset) *out_offset = offset;
        return ptr;
    }

    bool free_block(void* ptr) {
        if (!contains(ptr)) return false;
        const uint32_t offset = to_offset(ptr);
        return blocks.erase(offset) > 0;
    }

    void release() {
        if (base) {
            VirtualFree(base, 0, MEM_RELEASE);
            base = nullptr;
        }
        size = 0;
        blocks.clear();
    }
};

std::mutex g_apu_mutex;
bool g_apu_heap_created = false;
ApuRegion g_cached_heap;
ApuRegion g_noncached_heap;

// Tracked SHAPE context arrays
struct ShapeArrayFree {
    void operator()(void* array) const noexcept { std::free(array); }
};

using ShapeAllocation = std::array<std::unique_ptr<void, ShapeArrayFree>, 6>;

std::mutex g_shape_mutex;
std::vector<ShapeAllocation> g_shape_allocations;

// Resolves a contiguous [apu, apu + len) byte range inside the APU heap.
uint8_t* apu_span(uint32_t apu, std::size_t len) {
    if (apu < kApuBaseAddress || len == 0) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    const uint32_t offset = apu - kApuBaseAddress;
    if (g_cached_heap.base && offset < g_cached_heap.size) {
        return g_cached_heap.span_from_offset(offset, len);
    }
    if (g_noncached_heap.base) {
        const uint64_t cached_span = align_apu_region(g_cached_heap.size);
        if (offset >= cached_span && offset - cached_span < g_noncached_heap.size) {
            return g_noncached_heap.span_from_offset(
                static_cast<uint32_t>(offset - cached_span), len);
        }
    }
    return nullptr;
}

// ---- ACP Command / Message Protocol Constants ----

constexpr uint32_t kCmdLoadFlowgraph = 1;
constexpr uint32_t kCmdRegisterMessages = 2;
constexpr uint32_t kCmdUnregisterMessages = 3;
constexpr uint32_t kCmdEnableXma = 5;
constexpr uint32_t kCmdEnableXmas = 6;
constexpr uint32_t kCmdDisableXma = 7;
constexpr uint32_t kCmdDisableXmas = 8;
constexpr uint32_t kCmdAdvanceXmaRead = 18;

constexpr uint32_t kMsgFlowgraphCompleted = 0x02u;
constexpr uint32_t kMsgCommandCompleted = 0x10u;
constexpr uint32_t kMsgDisconnected = 0x80u;

// 128 samples per audio frame at 48 kHz = 375 audio frames/second.
constexpr uint64_t kAudioFramesPerSecond = 375;

// ---- XMA2 Hardware Context Decoder Engine ----
//
// Each 64-byte SHAPE_XMA_CONTEXT holds 16 32-bit words shared with the title:
//   Word 0: input 0 packets (0..11), loop count (12..19, 255 = forever),
//           valid input bits (20..21), output ring blocks (22..26), write block (27..31).
//   Word 1: input 1 packets (0..11), loop end subframe (12..13), loop start skip subframes
//           (17..19), initial skip subframes (24..26), rate index (27..28: 24/32/44.1/48 kHz),
//           stereo (29), output writable (31).
//   Word 2: read bit offset (0..25), error status (26..30), error flag (31).
//   Word 3: loop start bit offset (0..25).
//   Word 4: loop end bit offset (0..25), current input buffer index (31).
//   Word 5: input 0 APU address.
//   Word 6: input 1 APU address.
//   Word 7: output ring APU address.
//   Word 8: overlap buffer APU address.
//   Word 9: output read block index (0..4).
//
// Packets are decoded by the Rust decoder in runtime/xma_decoder when CMake found cargo (LEONIDA_HAVE_XMA2_DECODER);
// otherwise each packet becomes silence for its frame count. The loop points name frame bit positions within
// the input buffers. On a pass that rewinds, the loop-end frame plays through the selected 128-sample subframe.

constexpr std::size_t kXmaBlockBytes = 256;
constexpr std::size_t kXmaSamplesPerBlock = kXmaBlockBytes / 2;
constexpr std::size_t kXmaPacketBytes = 2048;
constexpr std::size_t kXmaPacketBits = kXmaPacketBytes * 8;
constexpr std::size_t kXmaHeaderBits = 32;
constexpr std::size_t kXmaFrameLengthBits = 15;
constexpr std::size_t kXmaSubframeSamples = 128;
constexpr std::size_t kXmaFrameSamples = 512;
constexpr uint32_t kXmaErrorInputInvalid = 4;

// XMA2's big-endian packet header has the count of frames that start in this packet, then the bit count of
// the preceding frame's continuation. Each newly started frame has a 15-bit length, including the prefix.
struct XmaPacketFrames {
    std::array<std::size_t, 63> starts{};
    std::size_t count = 0;
    std::size_t complete = 0;
    std::size_t leading = 0;

    std::optional<std::size_t> index(std::size_t bit) const {
        const auto end = starts.begin() + static_cast<std::ptrdiff_t>(count);
        const auto found = std::find(starts.begin(), end, bit);
        if (found == end) return std::nullopt;
        return static_cast<std::size_t>(found - starts.begin());
    }
};

std::size_t xma_bits(const uint8_t* packet, std::size_t bit, std::size_t count) {
    std::size_t value = 0;
    for (std::size_t i = 0; i < count; ++i, ++bit) {
        value = (value << 1) | ((packet[bit / 8] >> (7 - bit % 8)) & 1u);
    }
    return value;
}

std::size_t xma_leading_bits(const uint8_t* packet) {
    return ((packet[0] & 3u) << 13) | (packet[1] << 5) | (packet[2] >> 3);
}

std::optional<XmaPacketFrames> xma_packet_frames(const uint8_t* packet) {
    XmaPacketFrames frames;
    frames.count = packet[0] >> 2;
    frames.leading = xma_leading_bits(packet);
    std::size_t bit = kXmaHeaderBits + frames.leading;
    for (std::size_t i = 0; i < frames.count; ++i) {
        if (bit >= kXmaPacketBits) return std::nullopt;
        frames.starts[i] = bit;
        const std::size_t remaining = kXmaPacketBits - bit;
        if (remaining < kXmaFrameLengthBits) break;
        const std::size_t length = xma_bits(packet, bit, kXmaFrameLengthBits);
        if (length <= kXmaFrameLengthBits) return std::nullopt;
        if (length > remaining) break;
        ++frames.complete;
        bit += length;
    }
    if (frames.count - frames.complete > 1) return std::nullopt;
    return frames;
}

bool xma_finishes_split_frame(const uint8_t* packet, std::size_t start, const uint8_t* next) {
    const std::size_t head = kXmaPacketBits - start;
    const std::size_t tail = xma_leading_bits(next);
    if (tail == 0 || tail > kXmaPacketBits - kXmaHeaderBits) return false;
    const std::size_t prefix = std::min(head, kXmaFrameLengthBits);
    const std::size_t missing = kXmaFrameLengthBits - prefix;
    if (tail < missing) return false;
    const std::size_t length = (xma_bits(packet, start, prefix) << missing) | xma_bits(next, kXmaHeaderBits, missing);
    return length == head + tail;
}

void xma_start_at_frame(uint8_t* packet, std::size_t bit, std::size_t remaining_frames) {
    const auto header = static_cast<uint32_t>((remaining_frames << 26) | ((bit - kXmaHeaderBits) << 11) |
                                               ((packet[2] & 7u) << 8) | packet[3]);
    for (std::size_t i = 0; i < 4; ++i) packet[i] = static_cast<uint8_t>(header >> (24 - 8 * i));
}

#if defined(LEONIDA_HAVE_XMA2_DECODER)
constexpr uint32_t kXmaRates[4] = {24000, 32000, 44100, 48000};
struct Xma2DecoderFree {
    void operator()(LeonidaXma2Decoder* d) const noexcept { leonida_xma2_decoder_destroy(d); }
};
struct Xma2PcmFree {
    void operator()(LeonidaXma2PcmBlock* b) const noexcept { leonida_xma2_pcm_destroy(b); }
};
#endif

struct XmaContextView {
    uint32_t* words = nullptr;

    uint32_t get(std::size_t index) const { return std::atomic_ref(words[index]).load(); }

    uint32_t field(std::size_t index, uint32_t shift, uint32_t bits) const {
        const uint32_t mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        return (get(index) >> shift) & mask;
    }

    uint32_t set_field(std::size_t index, uint32_t shift, uint32_t bits, uint32_t value) const {
        const uint32_t raw_mask = (bits >= 32) ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        const uint32_t mask = raw_mask << shift;
        const uint32_t field_value = (value << shift) & mask;
        std::atomic_ref word(words[index]);
        uint32_t previous = word.load();
        while (!word.compare_exchange_weak(previous, (previous & ~mask) | field_value)) {}
        return previous;
    }

};

struct XmaVoice {
    explicit XmaVoice(uint32_t format_bits) : pcm(0), format(format_bits) {}
    std::size_t channels() const { return 1u + (format >> 2); }
    std::size_t queued_samples() const { return pcm.size() - pcm_read; }

#if defined(LEONIDA_HAVE_XMA2_DECODER)
    std::unique_ptr<LeonidaXma2Decoder, Xma2DecoderFree> decoder;
#endif
    std::vector<int16_t> pcm;
    std::size_t pcm_read = 0;
    std::size_t loop_skip = 0;  // samples per channel to drop after a rewind
    uint32_t format;  // SHAPE word 1 bits 27..29, rate and channel layout
    bool loop_restart = false;
    bool ended = false;
};

struct XmaVoiceSlot {
    std::mutex mutex;
    std::optional<XmaVoice> voice;
};

class XmaEngine {
public:
    ~XmaEngine() { unbind(); }

    void bind(SHAPE_XMA_CONTEXT* array, uint32_t count) {
        // Construct with a size so MSVC's Debug iterator allocations can also fail
        // through this call instead of the noexcept default constructor.
        std::vector<std::unique_ptr<XmaVoiceSlot>> slots(count);
        for (auto& slot : slots) slot = std::make_unique<XmaVoiceSlot>();
        unbind();
        std::unique_lock lock(table_mutex_);
        array_ = array;
        voices_.swap(slots);
    }

    void unbind() {
        {
            std::unique_lock lock(table_mutex_);
            array_ = nullptr;
            voices_.clear();
            active_count_.store(0, std::memory_order_relaxed);
        }
        // Once detached, commands cannot start a worker for this table.
        stop_worker();
    }

    bool handle_command(uint32_t command, const void* config) {
        auto read_word = [config](std::size_t index) -> uint32_t {
            if (!config) return 0u;
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(config) + index * sizeof(uint32_t),
                        sizeof(uint32_t));
            return value;
        };

        switch (command) {
            case kCmdEnableXma:
                if (config) enable(read_word(0));
                return true;
            case kCmdDisableXma:
                if (config) disable(read_word(0));
                return true;
            case kCmdEnableXmas:
            case kCmdDisableXmas: {
                std::shared_lock lock(table_mutex_);
                const std::size_t count = voices_.size();
                lock.unlock();
                if (!config) return true;
                for (std::size_t i = 0; i < count; ++i) {
                    const uint32_t bits = read_word(i / 32);
                    if ((bits & (1u << (i % 32))) != 0) {
                        if (command == kCmdEnableXmas) {
                            enable(static_cast<uint32_t>(i));
                        } else {
                            disable(static_cast<uint32_t>(i));
                        }
                    }
                }
                return true;
            }
            case kCmdAdvanceXmaRead:
                if (config) advance_read(read_word(0), read_word(1));
                return true;
            default:
                return false;
        }
    }

    void enable(uint32_t index) {
        std::shared_lock table_lock(table_mutex_);
        XmaContextView ctx = context_locked(index);
        if (!ctx.words) return;

        {
            std::lock_guard slot_lock(voices_[index]->mutex);
            auto& voice = voices_[index]->voice;
            const uint32_t format = ctx.field(1, 27, 3);
            // The worker cannot access the voice until the slot is unlocked.
            ensure_worker_and_wake();
            if (!voice) {
                voice.emplace(format);
                active_count_.fetch_add(1, std::memory_order_relaxed);
            } else if (voice->format != format) {
                voice->pcm.clear();
                voice->pcm_read = 0;
                voice->loop_skip = 0;
                voice->loop_restart = false;
                voice->format = format;
#if defined(LEONIDA_HAVE_XMA2_DECODER)
                voice->decoder.reset();
#endif
            }
            voice->ended = false;
            ctx.set_field(2, 26, 6, 0);
        }
    }

    void disable(uint32_t index) {
        std::shared_lock table_lock(table_mutex_);
        if (index >= voices_.size()) return;
        std::lock_guard slot_lock(voices_[index]->mutex);
        if (voices_[index]->voice.has_value()) {
            voices_[index]->voice.reset();
            active_count_.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    void advance_read(uint32_t index, uint32_t blocks) {
        std::shared_lock table_lock(table_mutex_);
        XmaContextView ctx = context_locked(index);
        if (!ctx.words) return;

        bool wake = false;
        {
            std::lock_guard slot_lock(voices_[index]->mutex);
            const uint32_t size = ctx.field(0, 22, 5);
            if (size == 0) return;
            const uint32_t read = (ctx.field(9, 0, 5) + blocks % size) % size;
            ctx.set_field(9, 0, 5, read);
            if (blocks > 0) {
                ctx.set_field(1, 31, 1, 1);
            }
            wake = voices_[index]->voice.has_value();
        }
        if (wake) ensure_worker_and_wake();
    }

private:
    XmaContextView context_locked(uint32_t index) const {
        if (!array_ || index >= voices_.size()) return {};
        return XmaContextView{array_[index].words};
    }

    // Appends interleaved s16 for one 2048-byte packet. Without the Rust decoder, or when it
    // fails, the packet becomes silence for its frame count (header bits 26..31) so timing holds.
    static bool decode_packet(XmaVoice& voice, const uint8_t* bytes) {
        const std::size_t channels = voice.channels();
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        if (!voice.decoder) {
            LeonidaXma2Decoder* raw = nullptr;
            if (leonida_xma2_decoder_create_hardware(kXmaRates[voice.format & 3u],
                                                     static_cast<uint32_t>(channels), &raw) == LEONIDA_XMA2_OK) {
                voice.decoder.reset(raw);
            }
        }
        if (voice.decoder) {
            LeonidaXma2PcmBlock* raw = nullptr;
            if (leonida_xma2_decode_buffer(voice.decoder.get(), bytes, kXmaPacketBytes, &raw) == LEONIDA_XMA2_OK) {
                const std::unique_ptr<LeonidaXma2PcmBlock, Xma2PcmFree> block(raw);
                const int16_t* samples = leonida_xma2_pcm_data(block.get());
                const std::size_t count = leonida_xma2_pcm_sample_count(block.get());
                if (count != 0) voice.pcm.insert(voice.pcm.end(), samples, samples + count);
                return true;
            }
            voice.decoder.reset();  // Resume with a clean state after replacing a packet with silence.
        }
#endif
        voice.pcm.resize(voice.pcm.size() + static_cast<std::size_t>(bytes[0] >> 2) * kXmaFrameSamples * channels);
        return false;
    }

    // Keep only PCM through the selected loop-end subframe. The Rust decoder can prepend one frame finished from
    // the previous packet. Compare its returned count with the packet's completed new frames to locate this one.
    static bool trim_loop_end(XmaVoice& voice, std::size_t packet_start, bool decoded, const XmaPacketFrames& frames,
                              std::size_t end, std::size_t subframe, const uint8_t* continuation) {
        const std::size_t frame_samples = kXmaFrameSamples * voice.channels();
        const std::size_t keep_tail = (subframe + 1) * kXmaSubframeSamples * voice.channels();
        if (!decoded) {
            voice.pcm.resize(packet_start + end * frame_samples + keep_tail);
            return true;
        }

        const std::size_t before_continuation = voice.pcm.size();
        const std::size_t current_samples = before_continuation - packet_start;
        const std::size_t complete_samples = frames.complete * frame_samples;
        const bool has_previous = frames.leading != 0 && current_samples == complete_samples + frame_samples;
        if (!has_previous && current_samples != complete_samples) return false;
        if (end == frames.complete) {
            if (!continuation) return false;
            const auto next = xma_packet_frames(continuation);
            if (!next || !decode_packet(voice, continuation) ||
                voice.pcm.size() - before_continuation != (next->complete + 1) * frame_samples) {
                voice.pcm.resize(before_continuation);
                return false;
            }
        }
        const std::size_t prefix = has_previous ? frame_samples : 0;
        voice.pcm.resize(packet_start + prefix + end * frame_samples + keep_tail);
        return true;
    }

    // Decodes the packet at the read offset of the current input buffer and queues its PCM.
    static bool decode_one_packet(XmaContextView ctx, XmaVoice& voice) {
        if (voice.ended) return false;

        const std::size_t channels = voice.channels();
        uint32_t current = ctx.field(4, 31, 1);
        const uint32_t valid = ctx.field(0, 20, 2);

        if (valid == 0) {
            voice.ended = true;
            ctx.set_field(2, 26, 6, kXmaErrorInputInvalid | 32u);
            return voice.queued_samples() != 0;
        }
        if ((valid & (1u << current)) == 0) {
            current ^= 1u;
            ctx.set_field(4, 31, 1, current);
        }

        const uint32_t address = (current == 0) ? ctx.get(5) : ctx.get(6);
        const std::size_t packets = (current == 0) ? ctx.field(0, 0, 12) : ctx.field(1, 0, 12);
        const std::size_t offset = ctx.field(2, 0, 26);
        const std::size_t packet = offset / kXmaPacketBits;
        const uint8_t* data = apu_span(address & ~0x7FFu, packets * kXmaPacketBytes);
        if (!data || packet >= packets) {
            finish_input(ctx, current, packet >= packets ? packet - packets : 0);
            return true;
        }

        const uint8_t* packet_data = data + packet * kXmaPacketBytes;
        const uint8_t packet_skip = packet_data[3];
        // XMA2 packet headers link packets from the same stream; 0xff ends the chain.
        std::size_t next = packet_skip == 0xFFu ? packets : packet + 1 + packet_skip;
        const uint32_t loops = ctx.field(0, 12, 8);
        const std::size_t loop_start = ctx.field(3, 0, 26);
        const std::size_t loop_end = ctx.field(4, 0, 26);
        const bool loop_packet = loops > 0 && loop_end != 0 && packet == loop_end / kXmaPacketBits;
        const std::size_t start_bit = offset % kXmaPacketBits;
        auto frames = (loop_packet || voice.loop_restart) ? xma_packet_frames(packet_data) : std::nullopt;
        // When resuming at a later frame, present its prefix as continuation data to the freshly reset codec.
        // It skips the prefix on its first packet without decoding earlier PCM into the overlap state.
        std::array<uint8_t, kXmaPacketBytes> start_packet;
        if (frames && voice.loop_restart) {
            if (const auto start = frames->index(start_bit); start && *start > 0) {
                std::memcpy(start_packet.data(), packet_data, start_packet.size());
                xma_start_at_frame(start_packet.data(), start_bit, frames->count - *start);
                packet_data = start_packet.data();
                frames = xma_packet_frames(packet_data);
            }
        }
        const auto end = loop_packet && frames ? frames->index(loop_end % kXmaPacketBits) : std::nullopt;
        const uint8_t* continuation = nullptr;
        if (end && *end == frames->complete) {
            if (next < packets) {
                continuation = data + next * kXmaPacketBytes;
            } else if (packet_skip != 0xFFu) {
                const uint32_t other = 1u - current;
                const std::size_t other_packets = ctx.field(other, 0, 12);
                const std::size_t follow = next - packets;
                if (follow < other_packets) {
                    // A declared continuation may arrive after the current input. Keep that input for the rewind.
                    if ((valid & (1u << other)) == 0) return false;
                    const auto* other_data = apu_span(ctx.get(5 + other) & ~0x7FFu, other_packets * kXmaPacketBytes);
                    if (other_data) continuation = other_data + follow * kXmaPacketBytes;
                }
            }
            if (continuation && !xma_finishes_split_frame(packet_data, frames->starts[*end], continuation)) {
                continuation = nullptr;
            }
        }
        // Decoding is only requested when less than a ring block remains. Keep that
        // tail and reuse the packet storage as the voice consumes subsequent packets.
        voice.pcm.erase(voice.pcm.begin(), voice.pcm.begin() + static_cast<std::ptrdiff_t>(voice.pcm_read));
        voice.pcm_read = 0;
        voice.loop_restart = false;
        const std::size_t packet_start = voice.pcm.size();
        const bool decoded = decode_packet(voice, packet_data);
        const std::size_t loop_start_packet = loop_start / kXmaPacketBits;
        if (end && (*end < frames->complete || continuation) && loop_start_packet < packets) {
            const auto rewind = xma_packet_frames(data + loop_start_packet * kXmaPacketBytes);
            if (rewind && rewind->index(loop_start % kXmaPacketBits)) {
                voice.loop_restart = trim_loop_end(voice, packet_start, decoded, *frames, *end,
                                                   ctx.field(1, 12, 2), continuation);
            }
        }
        const std::size_t packet_samples = voice.pcm.size() - packet_start;
        const std::size_t subframe_samples = kXmaSubframeSamples * channels;
        const std::size_t initial_skip = ctx.field(1, 24, 3);
        const std::size_t skipped_subframes = std::min(initial_skip, packet_samples / subframe_samples);
        const std::size_t initial_samples = skipped_subframes * subframe_samples;
        const std::size_t loop_samples = std::min(voice.loop_skip * channels, packet_samples - initial_samples);
        const std::size_t skip_samples = initial_samples + loop_samples;
        if (skip_samples != 0) {
            const auto first = voice.pcm.begin() + static_cast<std::ptrdiff_t>(packet_start);
            voice.pcm.erase(first, first + static_cast<std::ptrdiff_t>(skip_samples));
        }
        // Update the context only after the PCM append succeeds.
        if (skipped_subframes != 0) {
            ctx.set_field(1, 24, 3, static_cast<uint32_t>(initial_skip - skipped_subframes));
        }
        voice.loop_skip -= loop_samples / channels;

        std::size_t next_offset = 0;
        if (loop_packet) {
            next = loop_start_packet;
            if (voice.loop_restart) next_offset = loop_start % kXmaPacketBits;
            voice.loop_skip = static_cast<std::size_t>(ctx.field(1, 17, 3)) * kXmaSubframeSamples;
#if defined(LEONIDA_HAVE_XMA2_DECODER)
            // A rewind starts a different packet sequence. Discard the old overlap and any state advanced by
            // inspecting a continuation, including when its PCM count required a packet-level fallback.
            voice.decoder.reset();
#endif
            if (loops != 255u) ctx.set_field(0, 12, 8, loops - 1u);
        }
        if (next >= packets) {
            finish_input(ctx, current, next - packets);
        } else {
            ctx.set_field(2, 0, 26, static_cast<uint32_t>(next * kXmaPacketBits + next_offset));
        }
        return true;
    }

    static void finish_input(XmaContextView ctx, uint32_t current,
                             std::size_t skip_packets) {
        // Publish the next offset before telling the title it can refill this input.
        ctx.set_field(2, 0, 26, static_cast<uint32_t>(skip_packets * kXmaPacketBits));
        const uint32_t other = 1u - current;
        const uint32_t retired = ctx.set_field(0, 20 + current, 1, 0);
        // Take the other input if it was queued at retirement. Otherwise a double
        // refill of the empty pair starts at input 0. A lone input can still run.
        ctx.set_field(4, 31, 1, (retired & (1u << (20 + other))) != 0 ? other : 0u);
    }

    static void pump_voice(XmaContextView ctx, XmaVoice& voice) {
        const uint32_t size = ctx.field(0, 22, 5);
        if (size == 0 || ctx.field(1, 31, 1) == 0) return;
        auto* ring = apu_span(ctx.get(7), size * kXmaBlockBytes);
        if (!ring) return;

        for (uint32_t blocks = 0; blocks < size && ctx.field(1, 31, 1) == 1; ++blocks) {
            int decode_guard = 0;
            while (voice.queued_samples() < kXmaSamplesPerBlock && decode_guard++ < 16) {
                if (!decode_one_packet(ctx, voice)) break;
            }
            if (voice.queued_samples() == 0) return;

            const uint32_t write = ctx.field(0, 27, 5) % size;
            uint8_t* out = ring + write * kXmaBlockBytes;

            for (std::size_t i = 0; i < kXmaSamplesPerBlock; ++i) {
                int16_t sample = 0;
                if (voice.pcm_read != voice.pcm.size()) {
                    sample = voice.pcm[voice.pcm_read++];
                }
                const uint16_t raw = static_cast<uint16_t>(sample);
                out[2 * i] = static_cast<uint8_t>(raw & 0xFFu);
                out[2 * i + 1] = static_cast<uint8_t>((raw >> 8) & 0xFFu);
            }

            const uint32_t next_write = (write + 1u) % size;
            ctx.set_field(0, 27, 5, next_write);
            if (next_write == ctx.field(9, 0, 5) % size) {
                ctx.set_field(1, 31, 1, 0);
            }
        }
    }

    void pump_all() {
        std::shared_lock table_lock(table_mutex_);
        if (!array_) return;
        for (std::size_t i = 0; i < voices_.size(); ++i) {
            std::lock_guard slot_lock(voices_[i]->mutex);
            auto& voice = voices_[i]->voice;
            if (!voice) continue;
            auto ctx = context_locked(static_cast<uint32_t>(i));
            try {
                pump_voice(ctx, *voice);
            } catch (const std::bad_alloc&) {
                // Only this voice failed. Retire it and allow the title to enable it again.
                voice.reset();
                active_count_.fetch_sub(1, std::memory_order_relaxed);
                ctx.set_field(2, 26, 6, kXmaErrorInputInvalid | 32u);
            }
        }
    }

    void ensure_worker_and_wake() {
        std::lock_guard lock(worker_mutex_);
        if (!worker_.joinable()) {
            worker_ = std::jthread([this](std::stop_token stop) { worker_loop(stop); });
        }
        wake_requested_ = true;
        worker_cv_.notify_one();
    }

    // Each worker owns its stop_token, so a stop only ever reaches the thread it was
    // sent to; a worker started after this call is unaffected by it.
    void stop_worker() {
        std::jthread to_join;
        {
            std::lock_guard lock(worker_mutex_);
            if (!worker_.joinable()) return;
            worker_.request_stop();
            worker_cv_.notify_all();
            to_join = std::move(worker_);
        }
        if (to_join.joinable()) to_join.join();
    }

    void worker_loop(std::stop_token stop) {
        for (;;) {
            {
                std::unique_lock lock(worker_mutex_);
                if (stop.stop_requested()) return;
                if (active_count_.load(std::memory_order_relaxed) == 0 && !wake_requested_) {
                    worker_cv_.wait(lock, stop, [this]() { return wake_requested_; });
                    if (stop.stop_requested()) return;
                }
                wake_requested_ = false;
            }

            pump_all();

            std::unique_lock lock(worker_mutex_);
            if (stop.stop_requested()) return;
            if (!wake_requested_ && active_count_.load(std::memory_order_relaxed) > 0) {
                // One 256-byte output block lasts ~2.67 ms at 48 kHz mono.
                worker_cv_.wait_for(lock, stop, std::chrono::milliseconds(2),
                                    [this]() { return wake_requested_; });
            }
        }
    }

    mutable std::shared_mutex table_mutex_;
    SHAPE_XMA_CONTEXT* array_ = nullptr;
    std::vector<std::unique_ptr<XmaVoiceSlot>> voices_;
    std::atomic<uint32_t> active_count_{0};

    std::mutex worker_mutex_;
    std::condition_variable_any worker_cv_;
    std::jthread worker_;
    bool wake_requested_ = false;
};

XmaEngine g_xma_engine;

// ---- IAcpHal Interface Implementation ----

#pragma pack(push, 1)
struct AcpMessage {
    uint32_t kind = 0;
    uint32_t dropped = 0;
    uint32_t usec = 0;
    uint8_t data[16]{};
};
#pragma pack(pop)
static_assert(sizeof(AcpMessage) == 28, "AcpMessage is 28 bytes");

void write_u32_le(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

void write_u64_le(uint8_t* dst, uint64_t value) {
    write_u32_le(dst, static_cast<uint32_t>(value & 0xFFFFFFFFu));
    write_u32_le(dst + 4, static_cast<uint32_t>(value >> 32));
}

struct IAcpHal {
    virtual HRESULT __stdcall connect(uint32_t commands, uint32_t messages) = 0;
    virtual HRESULT __stdcall disconnect() = 0;
    virtual HRESULT __stdcall submit_command(uint32_t cmd, uint64_t id, uint32_t frame,
                                             const void* config, uint32_t notification) = 0;
    virtual uint32_t __stdcall pop_message(void* out) = 0;
    virtual uint32_t __stdcall get_num_messages() = 0;
    virtual HRESULT __stdcall release() = 0;
};

struct AcpHalImpl final : IAcpHal {
    std::mutex mutex_;
    bool connected_ = false;
    uint32_t mask_ = 0;
    std::deque<AcpMessage> messages_;
    std::size_t capacity_ = 0;
    uint32_t dropped_ = 0;
    std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();

    uint64_t elapsed_usec_locked() const {
        const auto dt = std::chrono::steady_clock::now() - since_;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(dt).count());
    }

    uint32_t audio_frame_locked() const {
        return static_cast<uint32_t>((elapsed_usec_locked() * kAudioFramesPerSecond) / 1000000ull);
    }

    void post_locked(uint32_t kind, const uint8_t* payload, std::size_t len) {
        if ((mask_ & kind) == 0) return;
        if (messages_.size() >= capacity_) {
            if (dropped_ < UINT32_MAX) ++dropped_;
            return;
        }
        AcpMessage msg{};
        msg.kind = kind;
        msg.dropped = dropped_;
        msg.usec = static_cast<uint32_t>(elapsed_usec_locked());
        if (payload && len > 0) {
            std::memcpy(msg.data, payload, std::min(len, sizeof(msg.data)));
        }
        try {
            messages_.push_back(msg);
            dropped_ = 0;
        } catch (const std::bad_alloc&) {
            if (dropped_ < UINT32_MAX) ++dropped_;
        }
    }

    // Slot 0: Connect (called at boot with (0x1000, 0x1000), result checked)
    HRESULT __stdcall connect(uint32_t commands, uint32_t messages) override {
        trace_call("AcpHal::Connect", "");
        if (commands == 0) return E_INVALIDARG;
        std::lock_guard lock(mutex_);
        connected_ = true;
        capacity_ = messages;
        return S_OK;
    }

    // Slot 1: Disconnect (called at shutdown)
    HRESULT __stdcall disconnect() override {
        trace_call("AcpHal::Disconnect", "");
        std::lock_guard lock(mutex_);
        if (!connected_) return E_FAIL;
        post_locked(kMsgDisconnected, nullptr, 0);
        connected_ = false;
        return S_OK;
    }

    // Slot 2: SubmitCommand
    // Acknowledges command completion, updates registered message masks, dispatches
    // XMA voice commands, and writes any requested APU notification word.
    HRESULT __stdcall submit_command(uint32_t cmd, uint64_t id, uint32_t frame,
                                     const void* config, uint32_t notification) override try {
        (void)frame;
        auto read_word = [config](std::size_t index) -> uint32_t {
            if (!config) return 0u;
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(config) + index * sizeof(uint32_t),
                        sizeof(uint32_t));
            return value;
        };

        const bool handled_xma = g_xma_engine.handle_command(cmd, config);
        {
            std::lock_guard lock(mutex_);
            if (!handled_xma) {
                switch (cmd) {
                    case kCmdRegisterMessages:
                        mask_ |= read_word(0);
                        break;
                    case kCmdUnregisterMessages:
                        mask_ &= ~read_word(0);
                        break;
                    case kCmdLoadFlowgraph: {
                        uint8_t payload[4];
                        write_u32_le(payload, read_word(1));
                        post_locked(kMsgFlowgraphCompleted, payload, sizeof(payload));
                        break;
                    }
                    default:
                        break;
                }
            }
            if (id != 0) {
                uint8_t done[16];
                write_u32_le(done, cmd);
                write_u64_le(done + 4, id);
                write_u32_le(done + 12, audio_frame_locked());
                post_locked(kMsgCommandCompleted, done, sizeof(done));
            }
        }

        if (notification != 0) {
            if (uint8_t* word_bytes = apu_span(notification, sizeof(uint32_t))) {
                if ((reinterpret_cast<uintptr_t>(word_bytes) & 3u) == 0) {
                    *reinterpret_cast<volatile uint32_t*>(word_bytes) = 1u;
                } else {
                    static constexpr uint8_t kOneLe[4] = {1, 0, 0, 0};
                    volatile uint8_t* dst = word_bytes;
                    for (std::size_t i = 0; i < 4; ++i) dst[i] = kOneLe[i];
                }
            }
        }

        trace_call("AcpHal::SubmitCommand", "");
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (const std::system_error&) {
        return E_FAIL;
    }

    // Slot 3: PopMessage — returns 0 without touching `out` when the queue is empty.
    uint32_t __stdcall pop_message(void* out) override {
        std::lock_guard lock(mutex_);
        if (messages_.empty()) return 0u;
        const AcpMessage msg = messages_.front();
        messages_.pop_front();
        if (out) std::memcpy(out, &msg, sizeof(msg));
        return 1u;
    }

    // Slot 4: GetNumMessages
    uint32_t __stdcall get_num_messages() override {
        std::lock_guard lock(mutex_);
        return static_cast<uint32_t>(messages_.size());
    }

    // Slot 5: Release
    HRESULT __stdcall release() override {
        trace_call("AcpHal::Release", "");
        delete this;
        return S_OK;
    }
};

// ---- AcpHalCreate ----

HRESULT __stdcall AcpHalCreate(void** ppAcpHal) {
    if (!ppAcpHal) return E_POINTER;
    *ppAcpHal = nullptr;
    try {
        *ppAcpHal = new AcpHalImpl();
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    trace_call("AcpHalCreate", "");
    return S_OK;
}

// ---- AcpHalAllocateShapeContexts ----

HRESULT __stdcall AcpHalAllocateShapeContexts(SHAPE_CONTEXTS* ctx) {
    if (!ctx) return E_POINTER;

    std::lock_guard lock(g_shape_mutex);

    ShapeAllocation pending{};
    std::size_t array_index = 0;
    auto alloc_array = [&](uint32_t count, size_t elem_size) -> void* {
        if (count == 0) return nullptr;
        void* ptr = std::calloc(count, elem_size);
        if (!ptr) throw std::bad_alloc();
        pending[array_index++].reset(ptr);
        return ptr;
    };

    SHAPE_CONTEXTS result{};
    result.numSrcContexts = ctx->numSrcContexts;
    result.numEqCompContexts = ctx->numEqCompContexts;
    result.numFiltVolContexts = ctx->numFiltVolContexts;
    result.numDmaContexts = ctx->numDmaContexts;
    result.numXmaContexts = ctx->numXmaContexts;
    result.numPcmContexts = ctx->numPcmContexts;
    try {
        result.srcContextArray = static_cast<SHAPE_SRC_CONTEXT*>(
            alloc_array(result.numSrcContexts, sizeof(SHAPE_SRC_CONTEXT)));
        result.eqCompContextArray = static_cast<SHAPE_EQCOMP_CONTEXT*>(
            alloc_array(result.numEqCompContexts, sizeof(SHAPE_EQCOMP_CONTEXT)));
        result.filtVolContextArray = static_cast<SHAPE_FILTVOL_CONTEXT*>(
            alloc_array(result.numFiltVolContexts, sizeof(SHAPE_FILTVOL_CONTEXT)));
        result.dmaContextArray = static_cast<SHAPE_DMA_CONTEXT*>(
            alloc_array(result.numDmaContexts, sizeof(SHAPE_DMA_CONTEXT)));
        result.xmaContextArray = static_cast<SHAPE_XMA_CONTEXT*>(
            alloc_array(result.numXmaContexts, sizeof(SHAPE_XMA_CONTEXT)));
        result.pcmContextArray = static_cast<SHAPE_PCM_CONTEXT*>(
            alloc_array(result.numPcmContexts, sizeof(SHAPE_PCM_CONTEXT)));

        // Assign synthetic APU addresses to the host-side context arrays.
        result.apuSrcContextArray = {result.srcContextArray ? kApuBaseAddress : 0};
        result.apuEqCompContextArray = {result.eqCompContextArray ? kApuBaseAddress + 0x1000 : 0};
        result.apuFiltVolContextArray = {result.filtVolContextArray ? kApuBaseAddress + 0x2000 : 0};
        result.apuDmaContextArray = {result.dmaContextArray ? kApuBaseAddress + 0x3000 : 0};
        result.apuXmaContextArray = {result.xmaContextArray ? kApuBaseAddress + 0x4000 : 0};
        result.apuPcmContextArray = {result.pcmContextArray ? kApuBaseAddress + 0x5000 : 0};

        g_shape_allocations.reserve(g_shape_allocations.size() + 1);
        if (result.xmaContextArray) g_xma_engine.bind(result.xmaContextArray, result.numXmaContexts);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }

    // Both the backing arrays and the engine's slots are now fully allocated.
    g_shape_allocations.push_back(std::move(pending));
    *ctx = result;
    if (rt::startup_trace::enabled()) {
        try {
            trace_call("AcpHalAllocateShapeContexts",
                       ",\"src\":" + std::to_string(result.numSrcContexts) +
                       ",\"xma\":" + std::to_string(result.numXmaContexts) +
                       ",\"pcm\":" + std::to_string(result.numPcmContexts));
        } catch (const std::bad_alloc&) {
            // The allocation succeeded. A missing diagnostic does not invalidate it.
        }
    }
    return S_OK;
}

// ---- AcpHalReleaseShapeContexts ----

HRESULT __stdcall AcpHalReleaseShapeContexts(void* /*hal*/) {
    std::lock_guard lock(g_shape_mutex);
    // Stop and detach the XMA worker before freeing backing arrays.
    g_xma_engine.unbind();

    // The export releases the process-wide SHAPE arrays. The argument is an
    // opaque HAL handle, and allocation descriptors are owned by the caller.
    g_shape_allocations.clear();

    trace_call("AcpHalReleaseShapeContexts", "");
    return S_OK;
}

// ---- ApuCreateHeap ----

HRESULT __stdcall ApuCreateHeap(uint32_t cachedSizeInBytes,
                                uint32_t nonCachedSizeInBytes) {
    std::lock_guard lock(g_apu_mutex);
    if (g_apu_heap_created) return E_FAIL;
    const uint64_t address_bytes = (nonCachedSizeInBytes ? align_apu_region(cachedSizeInBytes)
                                                       : cachedSizeInBytes) + nonCachedSizeInBytes;
    if (address_bytes > static_cast<uint64_t>(UINT32_MAX) + 1 - kApuBaseAddress) return E_INVALIDARG;

    if (cachedSizeInBytes > 0) {
        g_cached_heap.base = VirtualAlloc(
            nullptr, cachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_cached_heap.base) return E_OUTOFMEMORY;
        g_cached_heap.size = cachedSizeInBytes;
    }

    if (nonCachedSizeInBytes > 0) {
        g_noncached_heap.base = VirtualAlloc(
            nullptr, nonCachedSizeInBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_noncached_heap.base) {
            g_cached_heap.release();
            return E_OUTOFMEMORY;
        }
        g_noncached_heap.size = nonCachedSizeInBytes;
    }

    g_apu_heap_created = true;
    if (rt::startup_trace::enabled()) {
        try {
            trace_call("ApuCreateHeap",
                       ",\"cachedSize\":" + std::to_string(cachedSizeInBytes) +
                       ",\"nonCachedSize\":" + std::to_string(nonCachedSizeInBytes));
        } catch (const std::bad_alloc&) {
            // The heap is ready even if its diagnostic cannot be built.
        }
    }
    return S_OK;
}

// ---- ApuAlloc ----

HRESULT __stdcall ApuAlloc(void** virtualAddress, uint32_t* physicalAddress,
                           uint32_t sizeInBytes, uint32_t alignmentInBytes,
                           uint32_t flags) try {
    if (!virtualAddress) return E_POINTER;
    *virtualAddress = nullptr;
    if (physicalAddress) *physicalAddress = 0;

    if (sizeInBytes == 0) return E_INVALIDARG;
    if (alignmentInBytes == 0) alignmentInBytes = 1;
    if ((alignmentInBytes & (alignmentInBytes - 1)) != 0) return E_INVALIDARG;

    std::lock_guard lock(g_apu_mutex);

    // Routing: flags=2 routes to non-cached heap; flags=0 or 1 routes to cached heap.
    // If non-cached heap wasn't created or has no room, fall back to cached heap
    // because PC memory is unified and cache-coherent.
    const bool prefer_noncached = (flags == 2);
    ApuRegion* target = prefer_noncached ? &g_noncached_heap : &g_cached_heap;

    if (!target->base) {
        if (!prefer_noncached) {
            if (g_apu_heap_created) return E_OUTOFMEMORY;
            // Lazy default cached heap if ApuCreateHeap wasn't called yet
            constexpr uint32_t kDefaultHeapSize = 64 * 1024 * 1024;
            target->base = VirtualAlloc(nullptr, kDefaultHeapSize,
                                        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!target->base) return E_OUTOFMEMORY;
            target->size = kDefaultHeapSize;
            g_apu_heap_created = true;
        } else {
            // Fall back to cached heap
            target = &g_cached_heap;
            if (!target->base) return E_OUTOFMEMORY;
        }
    }

    uint32_t offset = 0;
    void* ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    if (!ptr && prefer_noncached && g_cached_heap.base) {
        // Fall back to cached heap if non-cached heap is full
        target = &g_cached_heap;
        ptr = target->allocate(sizeInBytes, alignmentInBytes, &offset);
    }

    if (!ptr) return E_OUTOFMEMORY;

    *virtualAddress = ptr;
    if (physicalAddress) {
        // Leave alignment padding between the two synthetic address ranges.
        uint32_t apu = kApuBaseAddress + offset;
        if (target == &g_noncached_heap) {
            apu += static_cast<uint32_t>(align_apu_region(g_cached_heap.size));
        }
        *physicalAddress = apu;
    }

    return S_OK;
} catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
}

// ---- ApuFree ----

void __stdcall ApuFree(void* virtualAddress) {
    if (!virtualAddress) return;
    std::lock_guard lock(g_apu_mutex);
    if (!g_cached_heap.free_block(virtualAddress)) {
        g_noncached_heap.free_block(virtualAddress);
    }
    trace_call("ApuFree", "");
}

// ---- ApuIsVirtualAddressValid ----

BOOL __stdcall ApuIsVirtualAddressValid(void* virtualAddress, uint32_t alignment) {
    if (!virtualAddress) return FALSE;
    if (alignment == 0) alignment = 1;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    if (g_noncached_heap.is_valid_address(virtualAddress, alignment)) return TRUE;
    return FALSE;
}

// ---- ApuMapApuAddress ----

void* __stdcall ApuMapApuAddress(uint32_t apu) {
    if (apu == 0 || apu < kApuBaseAddress) return nullptr;
    std::lock_guard lock(g_apu_mutex);
    const uint32_t offset = apu - kApuBaseAddress;
    if (g_cached_heap.base && offset < g_cached_heap.size) {
        return g_cached_heap.from_offset(offset);
    }
    if (g_noncached_heap.base) {
        const uint32_t cached_span = static_cast<uint32_t>(align_apu_region(g_cached_heap.size));
        if (offset >= cached_span && offset - cached_span < g_noncached_heap.size) {
            return g_noncached_heap.from_offset(offset - cached_span);
        }
    }
    return nullptr;
}

// ---- ApuMapVirtualAddress ----

uint32_t __stdcall ApuMapVirtualAddress(void* virtualAddress) {
    if (!virtualAddress) return 0;
    std::lock_guard lock(g_apu_mutex);
    if (g_cached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + g_cached_heap.to_offset(virtualAddress);
    }
    if (g_noncached_heap.contains(virtualAddress)) {
        return kApuBaseAddress + static_cast<uint32_t>(align_apu_region(g_cached_heap.size)) +
               g_noncached_heap.to_offset(virtualAddress);
    }
    return 0;
}

}  // namespace

SHIM_TABLE(acphal,
    {"acphal.dll", "AcpHalAllocateShapeContexts", reinterpret_cast<void*>(&AcpHalAllocateShapeContexts)},
    {"acphal.dll", "AcpHalCreate", reinterpret_cast<void*>(&AcpHalCreate)},
    {"acphal.dll", "AcpHalReleaseShapeContexts", reinterpret_cast<void*>(&AcpHalReleaseShapeContexts)},
    {"acphal.dll", "ApuAlloc", reinterpret_cast<void*>(&ApuAlloc)},
    {"acphal.dll", "ApuCreateHeap", reinterpret_cast<void*>(&ApuCreateHeap)},
    {"acphal.dll", "ApuFree", reinterpret_cast<void*>(&ApuFree)},
    {"acphal.dll", "ApuIsVirtualAddressValid", reinterpret_cast<void*>(&ApuIsVirtualAddressValid)},
    {"acphal.dll", "ApuMapApuAddress", reinterpret_cast<void*>(&ApuMapApuAddress)},
    {"acphal.dll", "ApuMapVirtualAddress", reinterpret_cast<void*>(&ApuMapVirtualAddress)},
);
