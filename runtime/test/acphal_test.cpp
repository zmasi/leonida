// Checks the registered acphal.dll shim against the measured call shapes
// (docs/retail_boot_chain.md, docs/retail_boot_frame_loop.md Stop 28, PR #6885, PR #7009).
//
// All 9 exports are resolved through rt::shims::find so the test exercises
// the exact shim table and calling convention the runtime installs.

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <vector>

#include "shims.h"

// Fail only the calling test thread's designated allocation.
static thread_local int allocations_before_failure = -1;
static std::atomic<bool> fail_worker_allocation{false};
static std::atomic<bool> count_worker_allocations{false};
static std::atomic<unsigned> worker_allocations{0};
static DWORD test_thread_id;
void* operator new(std::size_t size) {
    if (allocations_before_failure == 0) {
        allocations_before_failure = -1;
        throw std::bad_alloc();
    }
    if (fail_worker_allocation.load() && GetCurrentThreadId() != test_thread_id &&
        fail_worker_allocation.exchange(false)) {
        throw std::bad_alloc();
    }
    if (count_worker_allocations.load() && GetCurrentThreadId() != test_thread_id) {
        worker_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    if (allocations_before_failure > 0) --allocations_before_failure;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using AcpHalCreateFn = HRESULT(__stdcall*)(void**);
using AcpHalAllocShapeCtxFn = HRESULT(__stdcall*)(void*);
using AcpHalReleaseShapeCtxFn = HRESULT(__stdcall*)(void*);
using ApuCreateHeapFn = HRESULT(__stdcall*)(uint32_t, uint32_t);
// Measured: ApuAlloc(void** va, uint32_t* apu, uint32_t size, uint32_t align, uint32_t flags)
using ApuAllocFn = HRESULT(__stdcall*)(void**, uint32_t*, uint32_t, uint32_t, uint32_t);
using ApuFreeFn = void(__stdcall*)(void*);
// Measured: ApuIsVirtualAddressValid(void* va, uint32_t align)
using ApuIsValidFn = BOOL(__stdcall*)(void*, uint32_t);
using ApuMapApuAddrFn = void*(__stdcall*)(uint32_t);
using ApuMapVirtualAddrFn = uint32_t(__stdcall*)(void*);

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
    ++g_checks;
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

// Test mirror of SHAPE_CONTEXTS matching devdocs.xbox.com GDK spec
struct APU_ADDRESS {
    uint64_t value;
};

struct TestShapeContexts {
    uint32_t numSrcContexts;
    uint32_t numEqCompContexts;
    uint32_t numFiltVolContexts;
    uint32_t numDmaContexts;
    uint32_t numXmaContexts;
    uint32_t numPcmContexts;
    void* srcContextArray;
    void* eqCompContextArray;
    void* filtVolContextArray;
    void* dmaContextArray;
    void* xmaContextArray;
    void* pcmContextArray;
    APU_ADDRESS apuSrcContextArray;
    APU_ADDRESS apuEqCompContextArray;
    APU_ADDRESS apuFiltVolContextArray;
    APU_ADDRESS apuDmaContextArray;
    APU_ADDRESS apuXmaContextArray;
    APU_ADDRESS apuPcmContextArray;
};

static_assert(sizeof(TestShapeContexts) == 120, "TestShapeContexts is 120 bytes");
static_assert(offsetof(TestShapeContexts, xmaContextArray) == 0x38,
              "xmaContextArray is at offset +0x38");

struct XmaWords {
    uint32_t* data;

    std::atomic_ref<uint32_t> operator[](std::size_t index) const {
        return std::atomic_ref(data[index]);
    }
};

// Synthetic XMA2 with one nonzero spectral coefficient per channel and frame.
void write_xma_packet(uint8_t* packet, uint32_t frames, uint8_t packet_skip,
                      uint32_t rate_index = 3, bool stereo = false) {
    constexpr uint32_t scale_bands[] = {23, 24, 23, 23};
    const uint32_t bands = scale_bands[rate_index];
    const uint32_t channels = stereo ? 2u : 1u;
    const uint32_t frame_bits = stereo ? 90 + 2 * bands : 59 + bands;
    std::memset(packet, 0xFF, 2048);
    std::size_t at = 0;
    auto put = [&](uint32_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i, ++at) {
            const auto mask = static_cast<uint8_t>(0x80u >> (at % 8));
            packet[at / 8] &= static_cast<uint8_t>(~mask);
            if ((value >> (width - 1 - i)) & 1u) packet[at / 8] |= mask;
        }
    };
    put((frames << 26) | (1u << 8) | packet_skip, 32);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        put(frame_bits, 15);
        put(0b10u, 2);                    // Fixed layout, one 512-sample subframe.
        if (stereo) put(0, 1);             // No optional decorrelation matrix.
        put(0, 11);                       // Gain, trim, fill and subframe error flag.
        if (stereo) put(0b010u, 3);        // Two channels without decorrelation.
        put((1u << channels) - 1, channels); // Coefficients are present in each channel.
        put(1, 1);                        // Explicit vector coefficient count.
        put(0, 8 * channels);             // Use only run/level coding.
        put(0, 6);                        // Quantizer adjustment.
        if (stereo) put(0, 5);             // No per-channel quantizer modifiers.
        for (uint32_t channel = 0; channel < channels; ++channel) {
            put(0, 2);                   // Scale factor step.
            put((1u << bands) - 1, bands); // One zero delta per scale band.
        }
        for (uint32_t channel = 0; channel < channels; ++channel) {
            put(0, 1);                   // Run/level table 0.
            put(0b00u, 2);                // Run 0, level 1.
            put(channel == 0 ? 1u : 0u, 1);
            put(0b0101011u, 7);           // End of coefficients.
        }
        put(0, 1);                        // Frame padding bit.
        put(frame + 1 < frames ? 1 : 0, 1);
    }
}

constexpr uint32_t kTestXmaPacketBits = 2048 * 8;
constexpr uint32_t kTestXmaHeaderBits = 32;

uint32_t first_xma_frame_length(const uint8_t* packet) {
    return (static_cast<uint32_t>(packet[4]) << 7) | (packet[5] >> 1);
}

void copy_xma_bits(uint8_t* dest, uint32_t dest_bit, const uint8_t* source, uint32_t source_bit, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i, ++dest_bit, ++source_bit) {
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (dest_bit % 8));
        const bool bit = (source[source_bit / 8] & (0x80u >> (source_bit % 8))) != 0;
        dest[dest_bit / 8] = static_cast<uint8_t>((dest[dest_bit / 8] & ~mask) | (bit ? mask : 0u));
    }
}

void write_xma_header(uint8_t* packet, uint32_t frames, uint32_t leading_bits, uint8_t packet_skip) {
    const uint32_t header = (frames << 26) | (leading_bits << 11) | (1u << 8) | packet_skip;
    for (uint32_t i = 0; i < 4; ++i) {
        packet[i] = static_cast<uint8_t>(header >> (24 - 8 * i));
    }
}

struct SpanningFrame {
    uint32_t start;
    uint32_t next_frame;
};

// Split one compressed frame between two linked packets, then put a complete
// frame after the continuation. Input starts at the split frame in packet 0.
SpanningFrame write_spanning_xma_frame(uint8_t* packet, uint8_t* next_packet,
                                      uint32_t first_piece_bits = 32, bool following_frame = true) {
    const uint32_t start = kTestXmaPacketBits - first_piece_bits;
    std::array<uint8_t, kTestXmaPacketBits / 8> source{};
    write_xma_packet(source.data(), 1, 0);
    const uint32_t frame_bits = first_xma_frame_length(source.data());
    const uint32_t continuation_bits = frame_bits - first_piece_bits;
    std::memset(packet, 0xFF, source.size());
    std::memset(next_packet, 0xFF, source.size());
    write_xma_header(packet, 1, start - kTestXmaHeaderBits, 1);
    write_xma_header(next_packet, following_frame ? 1 : 0, continuation_bits, 0);
    copy_xma_bits(packet, start, source.data(), kTestXmaHeaderBits, first_piece_bits);
    copy_xma_bits(next_packet, kTestXmaHeaderBits, source.data(),
                  kTestXmaHeaderBits + first_piece_bits, continuation_bits);
    if (following_frame) {
        copy_xma_bits(next_packet, kTestXmaHeaderBits + continuation_bits, source.data(),
                      kTestXmaHeaderBits, frame_bits);
    }
    return {start, kTestXmaHeaderBits + continuation_bits};
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    test_thread_id = GetCurrentThreadId();
    std::printf("acphal_test: registered acphal.dll shim verification\n");

    const auto create = reinterpret_cast<AcpHalCreateFn>(
        rt::shims::find("acphal.dll", "AcpHalCreate"));
    const auto alloc_ctx = reinterpret_cast<AcpHalAllocShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalAllocateShapeContexts"));
    const auto release_ctx = reinterpret_cast<AcpHalReleaseShapeCtxFn>(
        rt::shims::find("acphal.dll", "AcpHalReleaseShapeContexts"));
    const auto create_heap = reinterpret_cast<ApuCreateHeapFn>(
        rt::shims::find("acphal.dll", "ApuCreateHeap"));
    const auto alloc = reinterpret_cast<ApuAllocFn>(
        rt::shims::find("acphal.dll", "ApuAlloc"));
    const auto free_mem = reinterpret_cast<ApuFreeFn>(
        rt::shims::find("acphal.dll", "ApuFree"));
    const auto is_valid = reinterpret_cast<ApuIsValidFn>(
        rt::shims::find("acphal.dll", "ApuIsVirtualAddressValid"));
    const auto map_apu = reinterpret_cast<ApuMapApuAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapApuAddress"));
    const auto map_virtual = reinterpret_cast<ApuMapVirtualAddrFn>(
        rt::shims::find("acphal.dll", "ApuMapVirtualAddress"));

    check(create != nullptr, "AcpHalCreate is registered in registry");
    check(alloc_ctx != nullptr, "AcpHalAllocateShapeContexts is registered in registry");
    check(release_ctx != nullptr, "AcpHalReleaseShapeContexts is registered in registry");
    check(create_heap != nullptr, "ApuCreateHeap is registered in registry");
    check(alloc != nullptr, "ApuAlloc is registered in registry");
    check(free_mem != nullptr, "ApuFree is registered in registry");
    check(is_valid != nullptr, "ApuIsVirtualAddressValid is registered in registry");
    check(map_apu != nullptr, "ApuMapApuAddress is registered in registry");
    check(map_virtual != nullptr, "ApuMapVirtualAddress is registered in registry");
    check(rt::shims::owns_dll("acphal.dll"), "the registry owns acphal.dll");

    if (!create || !alloc_ctx || !release_ctx || !create_heap || !alloc ||
        !free_mem || !is_valid || !map_apu || !map_virtual) {
        std::printf("Fatal: Missing export bindings for acphal.dll\n");
        return 1;
    }

    // Heap configuration is process-wide, so CTest exercises this mode separately.
    if (argc == 2 && std::strcmp(argv[1], "--noncached-only") == 0) {
        check(create_heap(0, 0x6000u) == S_OK, "Create only the non-cached APU heap");
        void* noncached = nullptr;
        uint32_t noncached_address = 0;
        check(alloc(&noncached, &noncached_address, 0x1000, 0x800, 2) == S_OK && noncached != nullptr,
              "Allocate from the non-cached heap");
        void* cached = noncached;
        uint32_t cached_address = noncached_address;
        check(alloc(&cached, &cached_address, 0x1000, 0x800, 1) == E_OUTOFMEMORY &&
                  cached == nullptr && cached_address == 0,
              "A missing cached heap stays unavailable after explicit configuration");
        check(map_apu(noncached_address) == noncached && map_virtual(noncached) == noncached_address,
              "A failed cached allocation leaves the non-cached mapping unchanged");
        free_mem(noncached);
        std::printf("acphal_test complete: %d checks, %d failures\n", g_checks, g_failures);
        return g_failures != 0;
    }

    // 1. AcpHalCreate
    void* acp = nullptr;
    check(create(&acp) == S_OK && acp != nullptr,
          "AcpHalCreate returns S_OK with a non-null object");
    check(create(nullptr) == E_POINTER,
          "AcpHalCreate(nullptr) returns E_POINTER");
    void* failed_hal = acp;
    allocations_before_failure = 0;
    const HRESULT create_failure = create(&failed_hal);
    allocations_before_failure = -1;
    check(create_failure == E_OUTOFMEMORY && failed_hal == nullptr,
          "AcpHalCreate returns E_OUTOFMEMORY and clears the output");

    if (acp) {
        using Slot0Fn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using Slot1Fn = HRESULT(__stdcall*)(void*);
        using Slot2Fn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using Slot3Fn = uint32_t(__stdcall*)(void*, void*);
        using Slot4Fn = uint32_t(__stdcall*)(void*);
        using Slot5Fn = HRESULT(__stdcall*)(void*);

        auto vtable = *static_cast<void* const* const*>(acp);
        check(vtable[0] != nullptr, "IAcpHal vtable slot 0 (connect) is present");
        check(vtable[1] != nullptr, "IAcpHal vtable slot 1 (disconnect) is present");
        check(vtable[2] != nullptr, "IAcpHal vtable slot 2 (submit_command) is present");
        check(vtable[3] != nullptr, "IAcpHal vtable slot 3 (pop_message) is present");
        check(vtable[4] != nullptr, "IAcpHal vtable slot 4 (get_num_messages) is present");
        check(vtable[5] != nullptr, "IAcpHal vtable slot 5 (release) is present");

        // Slot 0: Connect (called at boot with (0x1000, 0x1000))
        check(reinterpret_cast<Slot0Fn>(vtable[0])(acp, 0x1000, 0x1000) == S_OK,
              "IAcpHal slot 0 (connect) returns S_OK");

        // Slot 2: SubmitCommand. Critical boot requirement: The title calls
        // slot 2 as (2, 0xf0f0f0f0, -1, config, 0) and treats failure as fatal int3.
        check(reinterpret_cast<Slot2Fn>(vtable[2])(acp, 2, 0xf0f0f0f0, 0xffffffffu, nullptr, 0) == S_OK,
              "IAcpHal slot 2 (submit_command) returns S_OK (vital boot unblocker)");

        // Slots 3 and 4: Message polling returns 0
        check(reinterpret_cast<Slot3Fn>(vtable[3])(acp, nullptr) == 0, "IAcpHal slot 3 returns 0");
        check(reinterpret_cast<Slot4Fn>(vtable[4])(acp) == 0, "IAcpHal slot 4 returns 0");

        // Slot 1: Disconnect returns S_OK
        check(reinterpret_cast<Slot1Fn>(vtable[1])(acp) == S_OK, "IAcpHal slot 1 returns S_OK");

        // Slot 5: Release returns S_OK
        check(reinterpret_cast<Slot5Fn>(vtable[5])(acp) == S_OK, "IAcpHal slot 5 returns S_OK");
    }

    // 2. ApuCreateHeap: title passes (cachedSize, nonCachedSize=0x226e00)
    check(create_heap(4 * 1024 * 1024 + 0x400, 0x226e00) == S_OK,
          "ApuCreateHeap succeeds with an unaligned cached heap size");
    check(create_heap(4 * 1024 * 1024, 0x226e00) != S_OK,
          "ApuCreateHeap second call fails (heap already created)");

    // 3. ApuAlloc argument validation
    void* dummy = nullptr;
    check(alloc(nullptr, nullptr, 1024, 0x800, 0) == E_POINTER,
          "ApuAlloc(nullptr, ...) returns E_POINTER");
    check(alloc(&dummy, nullptr, 0, 0x800, 0) == E_INVALIDARG,
          "ApuAlloc with size=0 returns E_INVALIDARG");
    check(alloc(&dummy, nullptr, 1024, 3, 0) == E_INVALIDARG,
          "ApuAlloc with unaligned alignment (3) returns E_INVALIDARG");

    // 4. ApuAlloc real boot sequence:
    // First call: flags=2, size=0x222e00, align=0x800 (noncached heap)
    void* va_nc = nullptr;
    uint32_t apu_nc = 0;
    check(alloc(&va_nc, &apu_nc, 0x222e00, 0x800, 2) == S_OK && va_nc != nullptr,
          "ApuAlloc flags=2 at title's non-cached size 0x222e00 succeeds");
    check(apu_nc != 0, "ApuAlloc provides non-zero physical APU address");

    // Second call: flags=1, size=4096, align=0x800 (cached heap)
    void* va_c = nullptr;
    uint32_t apu_c = 0;
    check(alloc(&va_c, &apu_c, 4096, 0x800, 1) == S_OK && va_c != nullptr,
          "ApuAlloc flags=1 succeeds on cached heap");
    check(apu_c != 0, "ApuAlloc flags=1 provides non-zero APU address");

    // Address verification & disjoint ranges
    check(va_nc != va_c, "Cached and non-cached allocations return different virtual pointers");
    check(apu_nc != apu_c, "Cached and non-cached APU addresses are distinct");
    check(is_valid(va_nc, 0x800) == TRUE, "Non-cached allocation is in a valid heap");
    check(is_valid(va_c, 0x800) == TRUE, "Cached allocation is in a valid heap");
    check(is_valid(nullptr, 0x800) == FALSE, "ApuIsVirtualAddressValid(nullptr) returns FALSE");
    check(is_valid(reinterpret_cast<void*>(0x1234), 0x800) == FALSE,
          "ApuIsVirtualAddressValid(bogus) returns FALSE");

    // Round-trip mapping
    check(map_virtual(va_nc) == apu_nc, "ApuMapVirtualAddress matches apu_nc");
    check(map_virtual(va_c) == apu_c, "ApuMapVirtualAddress matches apu_c");
    check(map_apu(apu_nc) == va_nc, "ApuMapApuAddress round-trips non-cached address");
    check(map_apu(apu_c) == va_c, "ApuMapApuAddress round-trips cached address");
    check(map_apu(0) == nullptr, "ApuMapApuAddress(0) returns nullptr");
    check(map_virtual(nullptr) == 0, "ApuMapVirtualAddress(nullptr) returns 0");

    // 5. Genuine State Tracking: Free & Memory Reuse
    void* reusable_block1 = nullptr;
    check(alloc(&reusable_block1, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block1 != nullptr,
          "Allocate 4 KiB reusable block");
    free_mem(reusable_block1);
    check(true, "ApuFree on valid address succeeds without error");
    free_mem(nullptr);
    check(true, "ApuFree(nullptr) is a safe no-op");

    // Reallocate in the freed slot:
    void* reusable_block2 = nullptr;
    check(alloc(&reusable_block2, nullptr, 0x1000, 0x800, 1) == S_OK && reusable_block2 != nullptr,
          "Re-allocate in cached heap succeeds (memory successfully reused)");
    check(reusable_block2 == reusable_block1,
          "Free block was genuinely reused at the same offset by the allocator");
    free_mem(reusable_block2);

    // 6. AcpHalAllocateShapeContexts & AcpHalReleaseShapeContexts
    check(alloc_ctx(nullptr) == E_POINTER,
          "AcpHalAllocateShapeContexts(nullptr) returns E_POINTER");

    // Real executable call: 0 SRC, 350 XMA (+0x38), 0 PCM
    TestShapeContexts ctx{};
    ctx.numXmaContexts = 350;
    check(alloc_ctx(&ctx) == S_OK, "AcpHalAllocateShapeContexts succeeds");
    check(ctx.xmaContextArray != nullptr,
          "xmaContextArray (+0x38) is non-null after allocation");
    check(ctx.srcContextArray == nullptr, "srcContextArray is null when count is 0");

    // Verify zeroed memory across all 350 * 64 bytes
    const uint8_t* xma_bytes = static_cast<const uint8_t*>(ctx.xmaContextArray);
    bool all_zero = true;
    for (size_t i = 0; i < 350 * 64; ++i) {
        if (xma_bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    check(all_zero, "SHAPE context buffer is zero-initialized for title's stride");

    // Test multi-context allocation
    TestShapeContexts ctx_all{};
    ctx_all.numSrcContexts = 10;
    ctx_all.numEqCompContexts = 5;
    ctx_all.numFiltVolContexts = 8;
    ctx_all.numDmaContexts = 4;
    ctx_all.numXmaContexts = 12;
    ctx_all.numPcmContexts = 6;
    check(alloc_ctx(&ctx_all) == S_OK, "AcpHalAllocateShapeContexts with all 6 types succeeds");
    check(ctx_all.srcContextArray != nullptr, "srcContextArray is allocated");
    check(ctx_all.eqCompContextArray != nullptr, "eqCompContextArray is allocated");
    check(ctx_all.filtVolContextArray != nullptr, "filtVolContextArray is allocated");
    check(ctx_all.dmaContextArray != nullptr, "dmaContextArray is allocated");
    check(ctx_all.xmaContextArray != nullptr, "xmaContextArray is allocated");
    check(ctx_all.pcmContextArray != nullptr, "pcmContextArray is allocated");

    TestShapeContexts retry_ctx{};
    retry_ctx.numXmaContexts = 2;
    allocations_before_failure = 1;  // The tracked allocation can grow, then voice slot allocation fails.
    HRESULT allocation_result = E_UNEXPECTED;
    try {
        allocation_result = alloc_ctx(&retry_ctx);
    } catch (const std::bad_alloc&) {
        // An exception across the AcpHal boundary is a regression.
    }
    allocations_before_failure = -1;
    check(allocation_result == E_OUTOFMEMORY && retry_ctx.xmaContextArray == nullptr,
          "A failed XMA slot allocation returns E_OUTOFMEMORY without publishing a context");
    check(alloc_ctx(&retry_ctx) == S_OK && retry_ctx.xmaContextArray != nullptr,
          "SHAPE allocation can be retried after memory pressure");

    // Release shape contexts
    std::memset(&ctx_all, 0x55, sizeof(ctx_all));
    check(release_ctx(&ctx_all) == S_OK, "AcpHalReleaseShapeContexts succeeds");
    const auto* released_argument = reinterpret_cast<const uint8_t*>(&ctx_all);
    check(std::all_of(released_argument, released_argument + sizeof(ctx_all),
                      [](uint8_t b) { return b == 0x55; }),
          "SHAPE release does not write through an opaque or recycled argument");
    check(release_ctx(nullptr) == S_OK, "AcpHalReleaseShapeContexts(nullptr) succeeds");

    // 7. ACP command/message queue protocol (register, complete, overflow drops, disconnect, notify word)
    {
        #pragma pack(push, 1)
        struct TestAcpMessage {
            uint32_t kind;
            uint32_t dropped;
            uint32_t usec;
            uint8_t data[16];
        };
        #pragma pack(pop)
        static_assert(sizeof(TestAcpMessage) == 28, "ACP message is 28 bytes");

        using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using DisconnectFn = HRESULT(__stdcall*)(void*);
        using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using PopFn = uint32_t(__stdcall*)(void*, TestAcpMessage*);
        using CountFn = uint32_t(__stdcall*)(void*);
        using ReleaseFn = HRESULT(__stdcall*)(void*);

        void* q_acp = nullptr;
        check(create(&q_acp) == S_OK && q_acp != nullptr, "AcpHalCreate for queue test");
        auto vt = *static_cast<void* const* const*>(q_acp);
        const auto fn_connect = reinterpret_cast<ConnectFn>(vt[0]);
        const auto fn_disconnect = reinterpret_cast<DisconnectFn>(vt[1]);
        const auto fn_submit = reinterpret_cast<SubmitFn>(vt[2]);
        const auto fn_pop = reinterpret_cast<PopFn>(vt[3]);
        const auto fn_count = reinterpret_cast<CountFn>(vt[4]);
        const auto fn_release = reinterpret_cast<ReleaseFn>(vt[5]);

        check(fn_connect(q_acp, 0, 16) == E_INVALIDARG, "Connect(commands=0) returns E_INVALIDARG");
        check(fn_disconnect(q_acp) == E_FAIL, "Disconnect before Connect returns E_FAIL");
        check(fn_connect(q_acp, 16, 2) == S_OK, "Connect(16, 2) succeeds");

        // Allocate a 4-byte notification word in APU memory.
        void* notify_va = nullptr;
        uint32_t notify_apu = 0;
        check(alloc(&notify_va, &notify_apu, 16, 16, 1) == S_OK && notify_va != nullptr,
              "ApuAlloc notification word succeeds");
        *static_cast<uint32_t*>(notify_va) = 0;

        const uint32_t reg_mask = 0x92u;  // FLOWGRAPH_COMPLETED(0x02) | COMMAND_COMPLETED(0x10) | DISCONNECTED(0x80)
        check(fn_submit(q_acp, 2, 0x1122334455667788ull, 0xffffffffu, &reg_mask, notify_apu) == S_OK,
              "REGISTER_MESSAGES with id and notify word succeeds");
        check(*static_cast<volatile uint32_t*>(notify_va) == 1u,
              "SubmitCommand writes 1 to APU notification word");
        check(fn_count(q_acp) == 1u, "COMMAND_COMPLETED queued after REGISTER_MESSAGES");

        TestAcpMessage msg{};
        check(fn_pop(q_acp, &msg) == 1u, "PopMessage retrieves COMMAND_COMPLETED");
        uint32_t ack_cmd = 0;
        uint64_t ack_id = 0;
        std::memcpy(&ack_cmd, msg.data, 4);
        std::memcpy(&ack_id, msg.data + 4, 8);
        check(msg.kind == 0x10u && msg.dropped == 0u && ack_cmd == 2u && ack_id == 0x1122334455667788ull,
              "COMMAND_COMPLETED payload carries command id and 64-bit token");

        // Overflow queue of capacity 2 with 4 commands -> 2 dropped, reported on the 5th.
        for (uint64_t tok = 1; tok <= 4; ++tok) {
            check(fn_submit(q_acp, 12, tok, 0xffffffffu, nullptr, 0) == S_OK,
                  "SubmitCommand during overflow succeeds");
        }
        check(fn_count(q_acp) == 2u, "Message queue caps at capacity 2");
        check(fn_pop(q_acp, &msg) == 1u && fn_pop(q_acp, &msg) == 1u,
              "Drained the 2 queued messages");
        check(fn_submit(q_acp, 12, 5, 0xffffffffu, nullptr, 0) == S_OK,
              "SubmitCommand 5 after drain succeeds");
        check(fn_pop(q_acp, &msg) == 1u && msg.dropped == 2u,
              "Next queued message reports dropped == 2");

        // LOAD_FLOWGRAPH posts FLOWGRAPH_COMPLETED carrying word(1).
        const uint32_t fg_cfg[2] = {0, 0x42u};
        check(fn_submit(q_acp, 1, 0, 0xffffffffu, fg_cfg, 0) == S_OK, "LOAD_FLOWGRAPH succeeds");
        check(fn_pop(q_acp, &msg) == 1u && msg.kind == 0x02u, "FLOWGRAPH_COMPLETED queued");
        uint32_t fg_id = 0;
        std::memcpy(&fg_id, msg.data, 4);
        check(fg_id == 0x42u, "FLOWGRAPH_COMPLETED carries flowgraph word");

        const uint32_t command_completed_mask = 0x10u;
        check(fn_submit(q_acp, 3, 0x100u, 0xffffffffu, &command_completed_mask, 0) == S_OK &&
                  fn_count(q_acp) == 0u,
              "UNREGISTER_MESSAGES suppresses command-completed messages");

        // Disconnect posts DISCONNECTED.
        check(fn_disconnect(q_acp) == S_OK, "Disconnect succeeds when connected");
        check(fn_count(q_acp) == 1u, "Disconnect queued DISCONNECTED message");
        check(fn_pop(q_acp, &msg) == 1u && msg.kind == 0x80u, "Popped DISCONNECTED message");

        free_mem(notify_va);
        check(fn_release(q_acp) == S_OK, "Release queue AcpHal succeeds");
    }

    // 8. End-to-end XMA2 context ring decoding via background decoder thread
    {
        // Two synthetic XMA2 packets with three mono frames each. Each frame
        // transmits one positive spectral coefficient so decoded PCM differs from fallback silence.
        TestShapeContexts xma_ctx{};
        xma_ctx.numXmaContexts = 4;
        check(alloc_ctx(&xma_ctx) == S_OK && xma_ctx.xmaContextArray != nullptr,
              "Allocate 4 XMA SHAPE contexts");

        void* in_va = nullptr;
        uint32_t in_apu = 0;
        check(alloc(&in_va, &in_apu, 0x1000, 0x800, 1) == S_OK && in_va != nullptr,
              "ApuAlloc 2-packet XMA2 input buffer");
        auto* packets = static_cast<uint8_t*>(in_va);
        write_xma_packet(packets, 3, 0);
        write_xma_packet(packets + 2048, 3, 0);

        void* out_va = nullptr;
        uint32_t out_apu = 0;
        check(alloc(&out_va, &out_apu, 0x400, 0x100, 2) == S_OK && out_va != nullptr,
              "ApuAlloc 4-block XMA2 output ring");
        std::memset(out_va, 0x55, 0x400);

        // Configure voice index 1: 2 packets in input 0, input 0 valid, 4 output blocks,
        // writable = 1, 48 kHz mono, read_bit = 32.
        const XmaWords words{static_cast<uint32_t*>(xma_ctx.xmaContextArray) + 16};
        words[0] = 2u | (1u << 20) | (4u << 22);
        words[1] = (1u << 31) | (3u << 27);
        words[2] = 32u;
        words[5] = in_apu;
        words[7] = out_apu;

        using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using ReleaseFn = HRESULT(__stdcall*)(void*);

        void* x_acp = nullptr;
        check(create(&x_acp) == S_OK && x_acp != nullptr, "AcpHalCreate for XMA decode test");
        auto vt = *static_cast<void* const* const*>(x_acp);
        const auto fn_connect = reinterpret_cast<ConnectFn>(vt[0]);
        const auto fn_submit = reinterpret_cast<SubmitFn>(vt[2]);
        const auto fn_release = reinterpret_cast<ReleaseFn>(vt[5]);

        check(fn_connect(x_acp, 0x1000, 0x1000) == S_OK, "Connect for XMA decode test");

        // ENABLE_XMA (cmd 5) on voice 1.
        const uint32_t enable_arg = 1u;
        worker_allocations.store(0);
        count_worker_allocations.store(true);
        check(fn_submit(x_acp, 5, 0xf1, 0xffffffffu, &enable_arg, 0) == S_OK,
              "ENABLE_XMA voice 1 succeeds");

        // Wait up to 500 ms for the background decoder thread to fill the 4-block ring.
        for (int wait = 0; wait < 500 && ((words[1] >> 31) & 1u) == 1u; ++wait) {
            Sleep(1);
        }
        const bool ring_full = (words[1] >> 31) == 0u;
        check(ring_full, "Background XMA2 decoder fills ring and clears writable flag");
        check(((words[0] >> 27) & 0x1Fu) == 0u,
              "Write block wrapped back to 0 after filling 4 blocks");

        const auto* ring_bytes = static_cast<const uint8_t*>(out_va);
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        check(ring_full && std::any_of(ring_bytes, ring_bytes + 0x400,
                                       [](uint8_t b) { return b != 0 && b != 0x55; }),
              "The Rust decoder writes non-silent PCM");
#else
        check(ring_full && std::all_of(ring_bytes, ring_bytes + 0x400, [](uint8_t b) { return b == 0; }),
              "The fallback writes silence");
#endif

        // Advance read pointer 4 blocks at a time until input exhausts and INPUT_INVALID fires.
        const uint32_t advance_args[2] = {1u, 4u};
        int written_blocks = ring_full ? 4 : 0;
        bool advanced = true;
        for (int step = 0; step < 6; ++step) {
            advanced &= fn_submit(x_acp, 18, 0xf2 + static_cast<uint64_t>(step),
                                  0xffffffffu, advance_args, 0) == S_OK;
            // Wait until either ring fills again (writable == 0) or stream hits error bit.
            for (int wait = 0; wait < 200; ++wait) {
                const uint32_t w1 = words[1];
                const uint32_t w2 = words[2];
                if (((w1 >> 31) & 1u) == 0u || ((w2 >> 31) & 1u) == 1u) break;
                Sleep(1);
            }
            if (((words[2] >> 31) & 1u) == 1u && ((words[1] >> 31) & 1u) == 1u) {
                break;
            }
            if ((words[1] >> 31) != 0u) break;
            written_blocks += 4;
        }
        check(advanced, "ADVANCE_XMA_READ succeeds");
        check(written_blocks == 24, "Six frames write exactly 24 blocks before input exhaustion");
        count_worker_allocations.store(false);
        const unsigned allocation_count = worker_allocations.load();
        std::printf("C++ worker allocations for six PCM frames: %u\n", allocation_count);
        check(allocation_count > 0 && allocation_count < 16,
              "PCM storage is reused across packets instead of allocating per sample block");
        check(((words[0] >> 20) & 3u) == 0u, "XMA input buffer valid bits cleared on exhaustion");
        check(((words[2] >> 31) & 1u) == 1u, "XMA error flag set on exhaustion");
        check(((words[2] >> 26) & 0x1Fu) == 4u, "XMA error status is INPUT_INVALID (4)");

        // DISABLE_XMA (cmd 7) on voice 1.
        const uint32_t disable_arg = 1u;
        check(fn_submit(x_acp, 7, 0xf9, 0xffffffffu, &disable_arg, 0) == S_OK,
              "DISABLE_XMA voice 1 succeeds");
        check(fn_release(x_acp) == S_OK, "Release XMA AcpHal succeeds");
        free_mem(in_va);
        free_mem(out_va);
        check(release_ctx(&xma_ctx) == S_OK, "Release XMA SHAPE contexts joins decoder cleanly");
    }

    // 8b. Loop positions address frames and subframes. A mono frame is four 128-sample ring blocks.
    {
        TestShapeContexts xma_ctx{};
        xma_ctx.numXmaContexts = 4;
        check(alloc_ctx(&xma_ctx) == S_OK && xma_ctx.xmaContextArray != nullptr,
              "Allocate 4 XMA SHAPE contexts for the packet loop test");

        void* in_va = nullptr;
        uint32_t in_apu = 0;
        check(alloc(&in_va, &in_apu, 0x2000, 0x800, 2) == S_OK && in_va != nullptr &&
                  (in_apu & 0x7FFu) == 0 && map_apu(in_apu) == in_va,
              "Non-cached XMA input keeps its requested alignment and round trips");
        auto* packets = static_cast<uint8_t*>(in_va);
        write_xma_packet(packets, 1, 1);  // Packet 1 belongs to another stream.
        write_xma_packet(packets + 0x800, 2, 0);
        write_xma_packet(packets + 0x1000, 1, 0);

        void* out_va = nullptr;
        uint32_t out_apu = 0;
        check(alloc(&out_va, &out_apu, 0x2000, 0x100, 2) == S_OK && out_va != nullptr,
              "ApuAlloc output storage for the packet loop tests");

        // Voice index 2.
        const XmaWords words{static_cast<uint32_t*>(xma_ctx.xmaContextArray) + 32};

        using ConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using SubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using ReleaseFn = HRESULT(__stdcall*)(void*);

        void* x_acp = nullptr;
        check(create(&x_acp) == S_OK && x_acp != nullptr, "AcpHalCreate for packet loop test");
        auto vt = *static_cast<void* const* const*>(x_acp);
        const auto fn_connect = reinterpret_cast<ConnectFn>(vt[0]);
        const auto fn_submit = reinterpret_cast<SubmitFn>(vt[2]);
        const auto fn_release = reinterpret_cast<ReleaseFn>(vt[5]);
        check(fn_connect(x_acp, 0x1000, 0x1000) == S_OK, "Connect for packet loop test");

        auto configure_voice = [&](uint32_t loops, uint32_t input_0_packets, uint32_t ring_size,
                                   uint32_t write, uint32_t read) {
            std::memset(out_va, 0x55, 0x2000);
            const uint32_t input_1_packets = 3u - input_0_packets;
            words[0] = input_0_packets | (loops << 12) |
                       ((input_1_packets != 0 ? 3u : 1u) << 20) | (ring_size << 22) | (write << 27);
            words[1] = (1u << 31) | (3u << 27) | input_1_packets;
            words[2] = 32u;                       // read at packet 0
            words[3] = 32u;                       // loop start: packet 0
            words[4] = 32u;                       // loop end: first frame of packet 0
            words[5] = in_apu;
            words[6] = in_apu + input_0_packets * 2048u;
            words[7] = out_apu;
            words[9] = read;
        };

        const uint32_t voice_index = 2u;
        struct LoopBounds {
            uint32_t read = kTestXmaHeaderBits;
            uint32_t start = kTestXmaHeaderBits;
            uint32_t end = kTestXmaHeaderBits;
            uint32_t end_subframe = 3;
        };
        // Returns the number of blocks written before input exhaustion, or -1 on timeout.
        auto run_blocks = [&](uint32_t loops, uint32_t input_0_packets = 3,
                              uint32_t initial_skip = 0, uint32_t loop_skip = 0,
                              LoopBounds bounds = {}) -> int {
            configure_voice(loops, input_0_packets, 31, 0, 0);
            words[1].fetch_or((initial_skip << 24) | (loop_skip << 17) | (bounds.end_subframe << 12));
            words[2] = bounds.read;
            words[3] = bounds.start;
            words[4] = bounds.end;
            check(fn_submit(x_acp, 5, 0xf3, 0xffffffffu, &voice_index, 0) == S_OK,
                  "ENABLE_XMA voice 2 succeeds");
            int wait = 0;
            for (; wait < 1000 && ((words[2] >> 31) & 1u) == 0u; ++wait) {
                Sleep(1);
            }
            const int blocks = ((words[2] >> 31) & 1u) != 0u
                                   ? static_cast<int>((words[0] >> 27) & 0x1Fu)
                                   : -1;
            check(fn_submit(x_acp, 7, 0xf4, 0xffffffffu, &voice_index, 0) == S_OK,
                  "DISABLE_XMA voice 2 succeeds");
            return blocks;
        };

        check(run_blocks(0) == 8, "Packets 0 and 2 write 8 blocks, skipping packet 1");
        check(run_blocks(1) == 12, "One loop of packet 0 writes 12 blocks (packets 0, 0, 2)");
        check(((words[0] >> 12) & 0xFFu) == 0u, "Loop count is consumed by the single loop");
        const auto* ring_bytes = static_cast<const uint8_t*>(out_va);
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        auto has_signal = [](const uint8_t* start) {
            return std::any_of(start, start + 0x400, [](uint8_t b) { return b != 0 && b != 0x55; });
        };
        check(has_signal(ring_bytes) && has_signal(ring_bytes + 0x400) && has_signal(ring_bytes + 0x800),
              "Rust produces signal before and after rewinding the loop");
        check(std::memcmp(ring_bytes, ring_bytes + 0x400, 0x400) == 0,
              "Rewind decodes the first packet from fresh decoder state");
#endif
        check(run_blocks(1, 3, 2, 1) == 9 && ((words[1] >> 24) & 7u) == 0,
              "Initial and loop subframe skips consume 3 blocks and clear the initial skip");
        check(run_blocks(0, 2) == 8,
              "A skip to the end of input 0 continues at input 1");
        check(run_blocks(0, 1) == 8,
              "An overshooting skip starts at packet 1 of input 1");
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        check(has_signal(ring_bytes) && has_signal(ring_bytes + 0x400),
              "Rust produces signal on both sides of the buffer switch");
#endif

        for (uint32_t end = 0; end < 4; ++end) {
            const int expected = 9 + static_cast<int>(end);
            const int blocks = run_blocks(1, 3, 0, 0, {.end_subframe = end});
            std::printf("XMA loop end subframe %u: %d blocks, expected %d\n", end, blocks, expected);
            check(blocks == expected, "Only the repeated pass is truncated at the loop end subframe");
        }

        write_xma_packet(packets, 2, 1);
        const uint32_t second_frame = kTestXmaHeaderBits + first_xma_frame_length(packets);
        const int multi_frame = run_blocks(1, 3, 0, 0, {.start = second_frame, .end = second_frame, .end_subframe = 1});
        std::printf("XMA two frames in one loop packet: %d blocks, expected 14\n", multi_frame);
        check(multi_frame == 14, "Loop start and end select the second frame without repeating the first");

        const auto spanning = write_spanning_xma_frame(packets, packets + 0x1000);
        const LoopBounds split_bounds{
            .read = spanning.start, .start = spanning.start, .end = spanning.start, .end_subframe = 1};
        const int across_packet = run_blocks(1, 3, 0, 0, split_bounds);
        std::printf("XMA loop end frame spanning linked packets: %d blocks, expected 10\n", across_packet);
        check(across_packet == 10, "Finish the spanning frame before rewinding without playing the next frame early");
        const int across_input = run_blocks(1, 1, 0, 0, split_bounds);
        std::printf("XMA loop end frame spanning two input buffers: %d blocks, expected 10\n", across_input);
        check(across_input == 10, "Keep the loop-end input until its continuation in the other input is decoded");
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        constexpr std::size_t two_blocks = 2 * 256;
        check(has_signal(ring_bytes) && std::memcmp(ring_bytes, ring_bytes + two_blocks, two_blocks) == 0,
              "Rust PCM after a spanning rewind starts with the same two decoded subframes");
#endif

        const uint32_t next_frame = 2 * kTestXmaPacketBits + spanning.next_frame;
        const int followed_split = run_blocks(1, 3, 0, 0,
            {.read = spanning.start, .start = next_frame, .end = next_frame, .end_subframe = 1});
        std::printf("XMA loop end after a pending continuation: %d blocks, expected 10\n", followed_split);
        check(followed_split == 10, "Keep the carried frame before the loop-end frame's two subframes");

        const auto length_split = write_spanning_xma_frame(packets, packets + 0x1000, 9, false);
        const int split_prefix = run_blocks(1, 3, 0, 0,
            {.read = length_split.start, .start = length_split.start, .end = length_split.start, .end_subframe = 1});
        std::printf("XMA frame length split across packets: %d blocks, expected 6\n", split_prefix);
        check(split_prefix == 6, "A continuation-only packet can finish a frame whose length prefix is split");
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        const auto rejected = write_spanning_xma_frame(packets, packets + 0x1000);
        // The next frame still has a valid length. Its subframe error bit makes the codec emit only the
        // carried frame, so the continuation probe cannot use the returned PCM count for precise looping.
        const uint32_t error_bit = rejected.next_frame + 27u;
        packets[0x1000 + error_bit / 8] |= static_cast<uint8_t>(0x80u >> (error_bit % 8));
        const int reference_blocks = run_blocks(0, 3, 0, 0, {.read = rejected.start});
        std::array<uint8_t, 4 * 256> reference_pcm{};
        std::memcpy(reference_pcm.data(), ring_bytes, reference_pcm.size());
        const int fallback_blocks = run_blocks(1, 3, 0, 0,
            {.read = rejected.start, .start = rejected.start, .end = rejected.start, .end_subframe = 1});
        check(reference_blocks == 4 && fallback_blocks == reference_blocks && has_signal(ring_bytes) &&
                  std::memcmp(ring_bytes, reference_pcm.data(), reference_pcm.size()) == 0,
              "A rejected continuation probe does not change fallback playback after the rewind");
#endif
        write_xma_packet(packets, 1, 1);
        write_xma_packet(packets + 0x1000, 1, 0);

        configure_voice(255, 3, 4, 7, 9);
        check(fn_submit(x_acp, 5, 0xf3, 0xffffffffu, &voice_index, 0) == S_OK,
              "Enable infinite loop with out-of-range ring indices");
        for (int wait = 0; wait < 1000 && (words[1] >> 31) != 0u; ++wait) Sleep(1);
        check((words[1] >> 31) == 0u && ((words[0] >> 27) & 0x1Fu) == 1u,
              "Ring stops at the normalized read index");
        check(fn_submit(x_acp, 7, 0xf4, 0xffffffffu, &voice_index, 0) == S_OK,
              "Disable infinite loop after filling bounded ring");
        check(((words[0] >> 12) & 0xFFu) == 255u, "Infinite loop does not consume its loop count");
        check(std::all_of(ring_bytes + 0x400, ring_bytes + 0x1000,
                          [](uint8_t b) { return b == 0x55; }),
              "Output never writes beyond the configured four-block ring");

        words[0] = 3u << 22;  // three ring blocks
        words[9] = 2u;
        const uint32_t advance_args[2] = {2u, UINT32_MAX};
        check(fn_submit(x_acp, 18, 0xf5, 0xffffffffu, advance_args, 0) == S_OK && words[9] == 2u,
              "ADVANCE_XMA_READ wraps at the ring size without overflowing");

        auto wait_for_ring = [&]() {
            for (int wait = 0; wait < 1000 && (words[1] >> 31) != 0u; ++wait) Sleep(1);
            return (words[1] >> 31) == 0u;
        };
        const uint32_t refill_read_args[2] = {voice_index, 4};

        configure_voice(0, 3, 4, 0, 0);
        words[0] = 1u | (1u << 20) | (4u << 22);
        write_xma_packet(packets, 3, 0);
        check(fn_submit(x_acp, 5, 0xf6, 0xffffffffu, &voice_index, 0) == S_OK && wait_for_ring(),
              "First packet fills the ring and leaves queued PCM");
        TestShapeContexts source_only{};
        source_only.numSrcContexts = 1;
        check(alloc_ctx(&source_only) == S_OK && source_only.srcContextArray != nullptr,
              "A non-XMA allocation can coexist with the active XMA table");
        words[0].fetch_or(1u << 20);
        check(fn_submit(x_acp, 5, 0xf7, 0xffffffffu, &voice_index, 0) == S_OK,
              "Re-enable while refilling the current input");
        int refills = 0;
        for (int i = 0; i < 5; ++i) {
            if (fn_submit(x_acp, 18, 0xf8, 0xffffffffu, refill_read_args, 0) != S_OK || !wait_for_ring()) break;
            ++refills;
        }
        check(refills == 5 && (words[2] >> 31) == 0u && ((words[0] >> 20) & 3u) == 0u,
              "Refill and repeated enable preserve all six frames and consume the reused input");
        check(fn_submit(x_acp, 7, 0xf9, 0xffffffffu, &voice_index, 0) == S_OK,
              "Disable the single-buffer refill voice");

        // The next same-stream packet is in the first input's refill, after a short second input.
        write_xma_packet(packets, 1, 2);
        write_xma_packet(packets + 0x1000, 2, 0);
        write_xma_packet(packets + 0x1800, 1, 0);
        configure_voice(0, 1, 4, 0, 0);
        words[1] = (words[1] & ~0xFFFu) | 1u;
        check(fn_submit(x_acp, 5, 0xfa, 0xffffffffu, &voice_index, 0) == S_OK && wait_for_ring(),
              "Voice fills the ring before the skip crosses two buffers");
        words[5] = in_apu + 0x1000u;
        words[0] = (words[0] & ~0xFFFu) | 2u | (1u << 20);
        check(fn_submit(x_acp, 18, 0xfb, 0xffffffffu, refill_read_args, 0) == S_OK && wait_for_ring(),
              "The same-stream packet fills the ring after two buffer crossings");
        check(((words[0] >> 20) & 3u) == 0u && (words[4] >> 31) == 0u,
              "A skip across both buffers lands on the last packet of the reloaded input");
        check(fn_submit(x_acp, 7, 0xfc, 0xffffffffu, &voice_index, 0) == S_OK,
              "Disable the two-buffer refill voice");

        // Both sides were empty. When both are refilled, the title's first input wins.
        write_xma_packet(packets, 1, 0);
        configure_voice(0, 3, 4, 0, 0);
        words[0] = 1u | (1u << 20) | (4u << 22);
        const uint32_t voice_bit = 1u << voice_index;
        check(fn_submit(x_acp, 6, 0, 0xffffffffu, &voice_bit, 0) == S_OK && wait_for_ring(),
              "ENABLE_XMAS starts the selected voice and exhausts the first input");
        write_xma_packet(packets, 2, 0);
        write_xma_packet(packets + 0x800, 1, 0);
        words[6] = in_apu + 0x800u;
        words[1].fetch_or(1u);
        words[0].fetch_or(3u << 20);
        check(fn_submit(x_acp, 18, 0, 0xffffffffu, refill_read_args, 0) == S_OK && wait_for_ring(),
              "A double refill supplies the next PCM blocks");
        check(((words[0] >> 20) & 3u) == 2u && (words[4] >> 31) == 1u,
              "A double refill plays input 0 before input 1");
#if defined(LEONIDA_HAVE_XMA2_DECODER)
        check(has_signal(ring_bytes), "Rust decodes non-silent PCM after a double refill");
#endif

        check(fn_submit(x_acp, 8, 0, 0xffffffffu, &voice_bit, 0) == S_OK,
              "Reset the voice before exhausting both inputs");
        write_xma_packet(packets, 1, 0);
        write_xma_packet(packets + 0x800, 1, 0);
        configure_voice(0, 1, 8, 0, 0);
        words[1] = (words[1] & ~0xFFFu) | 1u;
        check(fn_submit(x_acp, 6, 0, 0xffffffffu, &voice_bit, 0) == S_OK && wait_for_ring() &&
                  ((words[0] >> 20) & 3u) == 0u,
              "The voice exhausts input 0 and then input 1");
        write_xma_packet(packets, 2, 0);
        words[0].fetch_or(3u << 20);
        const uint32_t full_ring[2] = {voice_index, 8};
        check(fn_submit(x_acp, 18, 0, 0xffffffffu, full_ring, 0) == S_OK && wait_for_ring() &&
                  ((words[0] >> 20) & 3u) == 2u,
              "A double refill plays input 0 first even after exhausting input 1");

        // Reconfigure the still-enabled slot and verify that no queued PCM or old format survives.
        bool reconfigured = true;
        for (uint32_t rate = 0; rate < 4; ++rate) {
            const bool stereo = (rate & 1u) == 0;
            write_xma_packet(packets, 1, 0, rate, stereo);
            words[0] = 1u | (1u << 20) | (16u << 22);
            words[2] = 32u;
            words[4] = 0u;
            words[9] = 12u;
            std::memset(out_va, 0, 0x1000);
            // Enable applies the new format before making the output writable.
            words[1] = ((rate | (stereo ? 4u : 0u)) << 27);
            reconfigured &= fn_submit(x_acp, 5, 0, 0xffffffffu, &voice_index, 0) == S_OK;
            reconfigured &= fn_submit(x_acp, 18, 0, 0xffffffffu, refill_read_args, 0) == S_OK;
            for (int wait = 0; wait < 1000 && (words[2] >> 31) == 0u; ++wait) Sleep(1);
            reconfigured &= (words[2] >> 31) == 1u && ((words[0] >> 27) & 0x1Fu) == (stereo ? 8u : 4u);
#if defined(LEONIDA_HAVE_XMA2_DECODER)
            reconfigured &= has_signal(ring_bytes);
#endif
        }
        check(reconfigured, "Re-enabled voices select mono or stereo PCM at all four sample rates");
        check(fn_submit(x_acp, 8, 0, 0xffffffffu, &voice_bit, 0) == S_OK,
              "DISABLE_XMAS stops the selected voice");

        configure_voice(0, 3, 4, 0, 0);
        write_xma_packet(packets, 1, 1);
        fail_worker_allocation.store(true);
        check(fn_submit(x_acp, 5, 0, 0xffffffffu, &voice_index, 0) == S_OK,
              "Enable a voice before an injected worker allocation failure");
        for (int wait = 0; wait < 1000 && (words[2] >> 31) == 0u; ++wait) Sleep(1);
        check(!fail_worker_allocation.exchange(false) && (words[2] >> 31) != 0u,
              "Worker allocation failure surfaces an XMA error without terminating the process");
        check(fn_submit(x_acp, 5, 0, 0xffffffffu, &voice_index, 0) == S_OK && wait_for_ring(),
              "A voice can run again after a worker allocation failure");

        check(release_ctx(x_acp) == S_OK, "Release SHAPE contexts through the opaque AcpHal handle");
        check(fn_release(x_acp) == S_OK, "Release packet loop AcpHal succeeds");
        free_mem(in_va);
        free_mem(out_va);
        check(release_ctx(&xma_ctx) == S_OK, "Release packet loop SHAPE contexts");
    }

    // 9. ENABLE_XMA racing SHAPE allocation and release, including concurrent releases.
    {
        using RaceConnectFn = HRESULT(__stdcall*)(void*, uint32_t, uint32_t);
        using RaceSubmitFn = HRESULT(__stdcall*)(void*, uint32_t, uint64_t, uint32_t, const void*, uint32_t);
        using RaceReleaseFn = HRESULT(__stdcall*)(void*);

        void* race_acp = nullptr;
        check(create(&race_acp) == S_OK && race_acp != nullptr, "AcpHalCreate for XMA race test");
        auto race_vt = *static_cast<void* const* const*>(race_acp);
        const auto race_connect = reinterpret_cast<RaceConnectFn>(race_vt[0]);
        const auto race_submit = reinterpret_cast<RaceSubmitFn>(race_vt[2]);
        const auto race_release = reinterpret_cast<RaceReleaseFn>(race_vt[5]);
        check(race_connect(race_acp, 0x1000, 0x1000) == S_OK, "Connect for XMA race test");

        constexpr int kRaceIterations = 20000;
        std::atomic<int> bind_failures{0};
        std::thread enabler([&]() {
            const uint32_t enable_arg = 1u;
            for (int i = 0; i < kRaceIterations; ++i) {
                race_submit(race_acp, 5, 0xe000u + static_cast<uint64_t>(i), 0xffffffffu,
                            &enable_arg, 0);
            }
        });
        std::thread releaser([&]() {
            for (int i = 0; i < kRaceIterations; ++i) {
                if (release_ctx(nullptr) != S_OK) ++bind_failures;
            }
        });
        for (int i = 0; i < kRaceIterations; ++i) {
            TestShapeContexts race_ctx{};
            race_ctx.numXmaContexts = 4;
            if (alloc_ctx(&race_ctx) != S_OK) ++bind_failures;
            if (release_ctx(&race_ctx) != S_OK) ++bind_failures;
        }
        enabler.join();
        releaser.join();
        check(bind_failures == 0, "20000 bind/release cycles race ENABLE_XMA and independent releases");
        check(race_release(race_acp) == S_OK, "Release race AcpHal succeeds");
    }

    std::printf("acphal_test complete: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
