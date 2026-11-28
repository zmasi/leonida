// Xbox-ABI queue and fence proxies (d3d12_xs_device.h) over synthetic COM objects, with no D3D12 runtime and no GPU:
//   - devices that stop at ID3D12Device, Device1, Device3 and Device9: the slots of a missing interface (45/46,
//     50, 75) are refused without reaching the device, the others get the real objects behind the proxies;
//   - fences with and without ID3D12Fence1: 11 or 12 public slots, QueryInterface and slot 11 to match;
//   - fence slot 12: the address of a completed value that follows the real fence, from a CPU Signal through the
//     proxy and from a signal the proxy never sees, up to device removal, with the descriptor's code pointer unused, and
//     the completion event left open when another reference keeps the real fence alive past the proxy;
//   - creation that fails, that returns S_OK with no object, a null IID or output, and an interface the real object
//     has but the proxy does not model;
//   - OpenSharedHandle of a fence (as ID3D12Fence, IUnknown, Fence1), of a non-fence object, of an invalid handle,
//     and with a null output or IID, with the references of every object it touches;
//   - allocation failures in the proxy (operator new, the arrays it unwraps), injected in this executable only;
//   - device slot 26 called the console's way (the struct's buffer in RCX, the device in RDX) and the PC way;
//   - --fail-virtualalloc / --fail-virtualprotect / --fail-veh: the guard region's setup failing, injected by
//     patching this executable's own import table before the first proxy table is built (one process each).
// Every device, queue, fence and resource here is a mock that records the calls it gets. No game data, no Xbox DLLs.
#include "../shims/d3d12_xs_device.h"
#include "../shims/xmem.h"
#include "../src/title_page_watch.h"
#include "../src/d3d12_xs_frame_events.h"
#include "../src/native_pso.h"

#include <d3d12.h>
#include <windows.h>

#include <algorithm>

#include <crtdbg.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <new>
#include <utility>

namespace {

const char* g_label = "xs_proxy_mock";

#define EXPECT(condition) do { \
    if (!(condition)) { std::fprintf(stderr, "%s line %d: %s\n", g_label, __LINE__, #condition); return 1; } \
} while (0)

// ---- Allocation failures: this executable's global operator new fails the next g_fail_news calls ----

int g_fail_news = 0;     // single-threaded: armed right before the call it is meant for
int g_failed_news = 0;   // allocations refused so far

}  // namespace

// The annotations are vcruntime_new.h's, so /analyze sees the same contract as for the CRT's own operator new.
_NODISCARD _Ret_notnull_ _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size) {
    if (g_fail_news > 0) {
        --g_fail_news;
        ++g_failed_news;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
_NODISCARD _Ret_maybenull_ _Success_(return != NULL) _Post_writable_byte_size_(size) _VCRT_ALLOCATOR
void* __CRTDECL operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
void __CRTDECL operator delete(void* p) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, std::size_t) noexcept { std::free(p); }
void __CRTDECL operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

namespace xs = rt::d3d12_xs;

// ---- Mocks ----

enum class Kind { device, queue, fence, resource, allocator, command_list, list, heap };

constexpr unsigned kMockSlots = 128;
constexpr std::uint64_t kFenceHandle = 0x1000, kResourceHandle = 0x2000;  // the handles the mock device opens
constexpr std::uint64_t kCompletedValue = 7;                            // what a mock fence's slot 8 returns
constexpr UINT kHeapMarker = 0x26262626;  // the VisibleNodeMask a mock device's slot 26 answers
constexpr SIZE_T kMockHeapCpuStart = 0x55550000ull;
constexpr UINT64 kMockHeapGpuStart = 0x66660000ull;
constexpr UINT kMockDescriptorInc = 32;
// {5d1e0c4a-2b7f-4e61-9a3d-8c4b2f1e6a70}: an interface the mock queue implements that the proxy does not model.
constexpr GUID kExtraIid{0x5d1e0c4a, 0x2b7f, 0x4e61, {0x9a, 0x3d, 0x8c, 0x4b, 0x2f, 0x1e, 0x6a, 0x70}};

struct Mock {
    void** vtable;
    Kind kind;
    long refs;
    int device_level;        // device: highest ID3D12DeviceN it answers (0 = ID3D12Device only)
    int list_level;          // command_list: 0=base (60 slots), 7=GCL7 (81 slots), 8=GCL8 (82 slots)
    bool fence1;             // fence: answers ID3D12Fence1; device: the fences it makes do
    bool allocator_tag;      // allocator: answers kLeonidaCommandAllocatorTag; device: the allocators it makes do
    bool full_chain_lists;   // device: CreateCommandList makes a full-chain list mock (Kind::list), else command_list
    bool create_null;        // device: creation returns create_result with no object
    bool zero_fences;        // device: the fences it makes start at 0, not kCompletedValue
    HRESULT create_result;   // device: what creation returns (S_OK makes the object)
    bool list_refuse;        // device: CreateCommandList refuses every riid with E_NOINTERFACE
    IID create_riid;         // device: copy of the riid its last CreateCommandList got (the caller's may be a local)
    Mock* last_child;        // device: the object its last creation or open made
    Mock* device10_alias;    // device: when set, QueryInterface(ID3D12Device10) answers this object, not itself
    SRWLOCK lock;            // fence: guards completed, armed_value and armed_event (zeroed = SRWLOCK_INIT)
    std::uint64_t completed; // fence: what GetCompletedValue (slot 8) returns
    std::uint64_t armed_value;  // fence: the value SetEventOnCompletion (slot 9) waits for
    HANDLE armed_event;      // fence: the event it sets then, or null
    Mock* completes_with;    // fence: once its completed value reaches completes_at, a GetCompletedValue first
    std::uint64_t completes_at, completes_value;  // signals that fence to completes_value (GPU order across queues)
    bool signals_complete;   // queue: a Signal (slot 14) completes the fence at once, as an idle GPU would
    const void* observed_queue;  // optional proxy pair to sample during a forwarded fence call
    const void* observed_fence;
    unsigned long observed_queue_refs, observed_fence_refs;
    unsigned calls[kMockSlots];
    std::uint64_t args[kMockSlots][6];
    std::uint64_t array[kMockSlots][4];  // device: the first entries of the array argument, as received
    D3D12_COMMAND_QUEUE_DESC queue_desc;  // device: the description of its last CreateCommandQueue (slot 8)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;  // device: the description CreateShaderResourceView (slot 18) last received
    IUnknown* held;                     // ID3D12Object::SetPrivateDataInterface (slot 5): one reference, until destroyed
    std::uint64_t tail[kMockSlots][3];   // device slots 27/53: the words after the fifth (a6..a8), as received
    bool words_only;                     // device slot 27: record the words and return S_OK, creating nothing
    UINT om_rtv_count;                   // list slot 46 (OMSetRenderTargets): captured RTV/DSV handles
    BOOL om_single_range;
    SIZE_T om_rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT];
    bool om_has_dsv;
    SIZE_T om_dsv;
    SIZE_T clear_dsv_handle;             // list slot 47 (ClearDepthStencilView): typed float-ABI capture
    D3D12_CLEAR_FLAGS clear_dsv_flags;
    FLOAT clear_dsv_depth;
    UINT8 clear_dsv_stencil;
    UINT clear_dsv_num_rects;
    const D3D12_RECT* clear_dsv_rects;
    D3D_FEATURE_LEVEL max_level;  // device: the highest level FEATURE_LEVELS reports (0: the query is refused)
    // list: set by a call the Vulkan layer's list takes as invalid (a texture barrier with no resource at slot 80, a
    // clear at slot 48 of a view made with no resource); Close (slot 9) then fails, as that list's does.
    bool list_error;
    // list slot 80 (Barrier): what the last call carried, copied (the proxy's converted groups live for the call).
    UINT barrier_group_count;
    D3D12_BARRIER_TYPE barrier_group_types[4];
    UINT barrier_group_sizes[4];
    UINT barrier_global_count;
    D3D12_GLOBAL_BARRIER barrier_globals[8];
    UINT barrier_texture_count;
    D3D12_TEXTURE_BARRIER barrier_textures[8];
};

Mock g_pool[512];
unsigned g_pool_used = 0;

// The real handles the mock device last wrote an RTV with no resource at (device slot 20). A mock list's clear of one
// sets list_error, as the Vulkan layer's empty view record does.
SIZE_T g_mock_null_rtvs[16];
unsigned g_mock_null_rtv_count = 0;

void mock_note_rtv(SIZE_T handle, bool no_resource) {
    for (unsigned i = 0; i < g_mock_null_rtv_count; ++i)
        if (g_mock_null_rtvs[i] == handle) g_mock_null_rtvs[i--] = g_mock_null_rtvs[--g_mock_null_rtv_count];
    if (no_resource && g_mock_null_rtv_count < std::size(g_mock_null_rtvs))
        g_mock_null_rtvs[g_mock_null_rtv_count++] = handle;
}

bool mock_is_null_rtv(SIZE_T handle) {
    for (unsigned i = 0; i < g_mock_null_rtv_count; ++i)
        if (g_mock_null_rtvs[i] == handle) return true;
    return false;
}

void mock_barrier(Mock* m, std::uint64_t count, std::uint64_t groups_word) {
    const auto* groups = reinterpret_cast<const D3D12_BARRIER_GROUP*>(groups_word);
    m->barrier_group_count = static_cast<UINT>(count);
    m->barrier_global_count = m->barrier_texture_count = 0;
    for (UINT g = 0; groups && g < static_cast<UINT>(count); ++g) {
        const D3D12_BARRIER_GROUP& group = groups[g];
        if (g < std::size(m->barrier_group_types)) {
            m->barrier_group_types[g] = group.Type;
            m->barrier_group_sizes[g] = group.NumBarriers;
        }
        for (UINT i = 0; i < group.NumBarriers; ++i) {
            if (group.Type == D3D12_BARRIER_TYPE_GLOBAL && m->barrier_global_count < std::size(m->barrier_globals)) {
                m->barrier_globals[m->barrier_global_count++] = group.pGlobalBarriers[i];
            } else if (group.Type == D3D12_BARRIER_TYPE_TEXTURE) {
                if (!group.pTextureBarriers[i].pResource) m->list_error = true;
                if (m->barrier_texture_count < std::size(m->barrier_textures))
                    m->barrier_textures[m->barrier_texture_count++] = group.pTextureBarriers[i];
            }
        }
    }
}

void** mock_vtable();
void** mock_list_vtable();

Mock* new_mock(Kind kind) {
    if (g_pool_used == std::size(g_pool)) {
        std::fprintf(stderr, "%s: mock pool exhausted\n", g_label);
        std::exit(1);
    }
    Mock* m = &g_pool[g_pool_used++];
    std::memset(m, 0, sizeof(*m));
    m->vtable = (kind == Kind::list || kind == Kind::command_list) ? mock_list_vtable() : mock_vtable();
    m->kind = kind;
    if (kind == Kind::fence) m->completed = kCompletedValue;
    return m;
}

// A mock fence's completed value, read and written under its lock: the proxy's wait thread calls slots 8 and 9 while
// the test signals. Signal sets the one armed event once the value reaches it, as the PC fence does.
std::uint64_t fence_completed(Mock* m) {
    AcquireSRWLockExclusive(&m->lock);
    const std::uint64_t value = m->completed;
    ReleaseSRWLockExclusive(&m->lock);
    return value;
}

void fence_signal(Mock* m, std::uint64_t value) {
    AcquireSRWLockExclusive(&m->lock);
    m->completed = value;
    if (m->armed_event && value >= m->armed_value) {
        SetEvent(m->armed_event);
        m->armed_event = nullptr;
    }
    ReleaseSRWLockExclusive(&m->lock);
}

HRESULT fence_set_event(Mock* m, std::uint64_t value, HANDLE event) {
    if (!event) return S_OK;  // the PC fence would block until `value`; no caller here does that
    AcquireSRWLockExclusive(&m->lock);
    if (m->completed >= value) {
        SetEvent(event);
    } else {
        m->armed_value = value;
        m->armed_event = event;
    }
    ReleaseSRWLockExclusive(&m->lock);
    return S_OK;
}

bool mock_has(const Mock* m, REFIID iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object))) return true;
    if (m->kind == Kind::device) {
        if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
        const IID levels[] = {__uuidof(ID3D12Device1), __uuidof(ID3D12Device2), __uuidof(ID3D12Device3),
                              __uuidof(ID3D12Device4), __uuidof(ID3D12Device5), __uuidof(ID3D12Device6),
                              __uuidof(ID3D12Device7), __uuidof(ID3D12Device8), __uuidof(ID3D12Device9),
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device10),
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device11),
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device12),
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device13),
#endif
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
                              __uuidof(ID3D12Device14),
#endif
        };
        for (int i = 0; i < m->device_level && i < static_cast<int>(std::size(levels)); ++i)
            if (IsEqualIID(iid, levels[i])) return true;
        return false;
    }
    if (IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable))) return true;
    switch (m->kind) {
        case Kind::queue: return IsEqualIID(iid, __uuidof(ID3D12CommandQueue)) || IsEqualIID(iid, kExtraIid);
        case Kind::fence:
            return IsEqualIID(iid, __uuidof(ID3D12Fence)) || (m->fence1 && IsEqualIID(iid, __uuidof(ID3D12Fence1)));
        case Kind::resource: return IsEqualIID(iid, __uuidof(ID3D12Resource));
        case Kind::heap:  // a descriptor heap, or a query heap (device slot 39)
            return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12QueryHeap));
        case Kind::command_list:
            if (IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
                IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList))) return true;
#ifdef __ID3D12GraphicsCommandList7_INTERFACE_DEFINED__
            if (m->list_level >= 7 && (IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
                                       IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1))))
                return true;
#endif
            return false;
        case Kind::allocator:
            return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator)) ||
                   (m->allocator_tag && IsEqualIID(iid, xs::kLeonidaCommandAllocatorTag));
        case Kind::list: {
            // The mock answers every graphics-command-list interface so the proxy sees the full 82-slot chain.
            const IID lists[] = {__uuidof(ID3D12CommandList),         __uuidof(ID3D12GraphicsCommandList),
                                 __uuidof(ID3D12GraphicsCommandList1), __uuidof(ID3D12GraphicsCommandList2),
                                 __uuidof(ID3D12GraphicsCommandList3), __uuidof(ID3D12GraphicsCommandList4),
                                 __uuidof(ID3D12GraphicsCommandList5), __uuidof(ID3D12GraphicsCommandList6),
                                 __uuidof(ID3D12GraphicsCommandList7), __uuidof(ID3D12GraphicsCommandList8)};
            for (const IID& list_iid : lists) if (IsEqualIID(iid, list_iid)) return true;
            return false;
        }
        default: return false;
    }
}

HRESULT mock_query(Mock* m, const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid || !mock_has(m, *riid)) return E_NOINTERFACE;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (m->device10_alias && IsEqualIID(*riid, __uuidof(ID3D12Device10))) m = m->device10_alias;
#endif
    *out = m;
    ++m->refs;
    return S_OK;
}

// A device creating (or opening) a `kind` object as `riid`: S_FALSE without an output, as the PC runtime answers a
// capability check; the device's create_result and create_null otherwise.
HRESULT mock_create(Mock* device, Kind kind, std::uint64_t riid, std::uint64_t out_word) {
    auto** out = reinterpret_cast<void**>(out_word);
    if (!out) return S_FALSE;
    *out = nullptr;
    if (device->create_null || FAILED(device->create_result)) return device->create_result;
    Mock* child = new_mock(kind);
    child->fence1 = device->fence1;
    child->allocator_tag = device->allocator_tag;
    if (kind == Kind::fence && device->zero_fences) child->completed = 0;
    device->last_child = child;
    return mock_query(child, reinterpret_cast<const IID*>(riid), out);
}

void copy_array(Mock* m, unsigned slot, std::uint64_t objects, std::uint64_t count) {
    const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
    for (std::uint32_t i = 0; in && i < static_cast<std::uint32_t>(count) && i < 4; ++i) m->array[slot][i] = in[i];
}

// The mock GPU clock pairs kMockGpu0 with a fixed QPC origin. Its timestamp frequency is 1 MHz. A resource's Map (slot 8) hands out
// g_mock_mapped: the timestamp readback of list values, which the test fills as the GPU would.
constexpr std::uint64_t kMockGpu0 = 1000000;
// All mock queues use the same fixed clock pair, even when the pump recalibrates one queue.
std::uint64_t mock_cpu_origin() {
    static const auto origin = [] {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return static_cast<std::uint64_t>(now.QuadPart);
    }();
    return origin;
}
alignas(8) std::uint64_t g_mock_readback[4096];
void* g_mock_mapped = g_mock_readback;

// The queue calls that order work (ExecuteCommandLists 10, Signal 14, Wait 15), in the order the mock queues got
// them: for an ExecuteCommandLists, a1 is the count and a2 the first list; else the fence and the value.
struct QueueCall {
    const Mock* queue;
    unsigned slot;
    std::uint64_t a1, a2;
};
QueueCall g_queue_calls[64];
unsigned g_queue_call_count = 0;

void log_queue_call(const Mock* queue, unsigned slot, std::uint64_t a1, std::uint64_t a2) {
    if (g_queue_call_count < std::size(g_queue_calls)) g_queue_calls[g_queue_call_count++] = {queue, slot, a1, a2};
}

// GetResourceAllocationInfo as a PC driver without console tile padding answers it. A buffer is its width, a texture
// width x height x slices x element bytes (2 for B5G5R5A1, 4 for RGBA8, 8 for RGBA16F, 16 for RGBA32F; mips are not
// counted, and one resource is at most 4 GiB, the way a driver caps an allocation), each rounded up to the
// resource's alignment: 64 KiB, or 4 MiB with more than one sample. A texture with no width is refused. Resources are
// laid out one after another, each at its own alignment, and the total is rounded up to the largest alignment, as the
// Vulkan layer's place() does. The struct is written to the hidden return pointer (RDX of a C++ method call), which
// comes back in RAX. `each` (slot 56) gets every resource's offset, alignment and size.
std::uint64_t mock_align(std::uint64_t value, std::uint64_t to) {
    return value > UINT64_MAX - (to - 1) ? UINT64_MAX / to * to : (value + to - 1) / to * to;
}

D3D12_RESOURCE_ALLOCATION_INFO mock_one_allocation(const D3D12_RESOURCE_DESC& d) {
    if (d.Width == 0) return {UINT64_MAX, 65536};
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return {mock_align(d.Width, 65536), 65536};
    std::uint64_t element = 4;
    if (d.Format == DXGI_FORMAT_B5G5R5A1_UNORM) element = 2;
    if (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) element = 8;
    if (d.Format == DXGI_FORMAT_R32G32B32A32_FLOAT) element = 16;
    const std::uint64_t samples = d.SampleDesc.Count ? d.SampleDesc.Count : 1;
    const std::uint64_t alignment = samples > 1 ? 4194304 : 65536;
    const std::uint64_t bytes = std::min<std::uint64_t>(
        d.Width * d.Height * d.DepthOrArraySize * element * samples, std::uint64_t{4} << 30);
    return {mock_align(bytes, alignment), alignment};
}

D3D12_RESOURCE_DESC g_seen_descs[4];  // the first descriptions the mock device's slot 25 was given
std::uint32_t g_seen_desc_count = 0;
std::uint32_t g_seen_calls = 0;  // slot 25 calls since the test cleared it; only the first is recorded

std::uint64_t g_texture_size_bias = 0;  // added to each texture's answer: a driver that pads more than the console

std::uint64_t mock_allocation_info(std::uint64_t ret, std::uint64_t count, std::uint64_t descs_word,
                                   D3D12_RESOURCE_ALLOCATION_INFO1* each = nullptr) {
    const auto* descs = reinterpret_cast<const D3D12_RESOURCE_DESC*>(descs_word);
    if (g_seen_calls++ == 0) {
        g_seen_desc_count = static_cast<std::uint32_t>(count);
        for (std::uint32_t i = 0; descs && i < g_seen_desc_count && i < std::size(g_seen_descs); ++i)
            g_seen_descs[i] = descs[i];
    }
    D3D12_RESOURCE_ALLOCATION_INFO total{0, 1};
    bool refused = false;
    for (std::uint32_t i = 0; descs && i < static_cast<std::uint32_t>(count); ++i) {
        D3D12_RESOURCE_ALLOCATION_INFO one = mock_one_allocation(descs[i]);
        if (descs[i].Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && one.SizeInBytes != UINT64_MAX)
            one.SizeInBytes += g_texture_size_bias;
        refused = refused || one.SizeInBytes == UINT64_MAX;
        const std::uint64_t offset = mock_align(total.SizeInBytes, one.Alignment);
        if (each) each[i] = {offset, one.Alignment, one.SizeInBytes};
        total.SizeInBytes = one.SizeInBytes > UINT64_MAX - offset ? UINT64_MAX : offset + one.SizeInBytes;
        total.Alignment = std::max(total.Alignment, one.Alignment);
    }
    auto* out = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(ret);
    *out = {refused ? UINT64_MAX : mock_align(total.SizeInBytes, total.Alignment), total.Alignment};
    return ret;
}

// `words` are the call's first six words (a1..a6), copied into the thunk's own frame.
std::uint64_t mock_call(Mock* m, unsigned slot, const std::uint64_t* words) {
    const std::uint64_t a1 = words[0], a2 = words[1], a3 = words[2], a4 = words[3], a5 = words[4], a6 = words[5];
    if (m->kind == Kind::device) {
        // The description lives on the caller's stack only for the call, so it is copied while the call is active.
        if (slot == 18 && a2 >= 0x10000) std::memcpy(&m->srv, reinterpret_cast<const void*>(a2), sizeof(m->srv));
        switch (slot) {
            case 8:                                                                           // CreateCommandQueue
                if (a1) std::memcpy(&m->queue_desc, reinterpret_cast<const void*>(a1), sizeof(m->queue_desc));
                return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a2, a3));
            case 9: return static_cast<std::uint32_t>(mock_create(m, Kind::allocator, a2, a3));  // CreateCmdAllocator
            case 20: mock_note_rtv(static_cast<SIZE_T>(a3), a1 == 0); break;  // CreateRenderTargetView(res, desc, dest)
            // Slot 12 (CreateCommandList) never reaches this switch: the device vtable's slot is the dedicated
            // mock_create_command_list, which needs the sixth word the generic thunk does not declare on some
            // builds and which picks the list mock's chain breadth from full_chain_lists.
            case 14: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));      // CreateDescriptorHeap
            case 15: return kMockDescriptorInc;                                                  // GetDescriptorHandleIncrementSize
            case 23:                                                                             // CopyDescriptors
                copy_array(m, 23, a2, a1);                                                       // dest starts
                copy_array(m, 24, words[4], a4);                                                 // src starts (recorded in slot 24's array)
                break;
            case 75: return static_cast<std::uint32_t>(mock_create(m, Kind::queue, a3, a4));   // CreateCommandQueue1
            case 36: return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a3, a4));   // CreateFence
            case 39: return static_cast<std::uint32_t>(mock_create(m, Kind::heap, a2, a3));    // CreateQueryHeap
            case 32:                                                                          // OpenSharedHandle
                if (a1 == kFenceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::fence, a2, a3));
                if (a1 == kResourceHandle) return static_cast<std::uint32_t>(mock_create(m, Kind::resource, a2, a3));
                return static_cast<std::uint32_t>(E_INVALIDARG);
            case 25: return mock_allocation_info(a1, a3, a4);  // GetResourceAllocationInfo (mask a2 is unused)
            case 56:  // GetResourceAllocationInfo1: (return buffer, mask, count, descs, per-description array)
                return mock_allocation_info(a1, a3, a4, reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(words[4]));
            case 34: case 35: case 46: copy_array(m, slot, a2, a1); break;  // (count, objects, ...)
            case 45: copy_array(m, slot, a1, a3); break;                    // (fences, values, count, ...)
            case 26: {  // GetCustomHeapProperties, the PC method: (return buffer, node mask, heap type)
                auto* out = reinterpret_cast<D3D12_HEAP_PROPERTIES*>(a1);
                if (!out) break;
                *out = {};
                out->Type = static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3));
                out->CreationNodeMask = static_cast<UINT>(a2);
                out->VisibleNodeMask = kHeapMarker;
                return a1;  // a struct-returning method hands the buffer's address back in RAX
            }
            case 41:                                                        // CreateCommandSignature(desc, ...)
                if (const auto* desc = reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(a1)) {
                    m->array[41][0] = desc->ByteStride | std::uint64_t{desc->NumArgumentDescs} << 32;
                    for (UINT i = 0; desc->pArgumentDescs && i < desc->NumArgumentDescs && i < 3; ++i) {
                        std::uint32_t type = 0;  // read as its 32 bits: a console type is outside the enum's range
                        std::memcpy(&type, &desc->pArgumentDescs[i].Type, sizeof(type));
                        m->array[41][i + 1] = type;
                    }
                }
                break;
            case 50: copy_array(m, slot, a3, a2); break;                    // (flags, count, objects, fence, value)
            case 68: {                                                      // GetResourceAllocationInfo2
                auto* ret = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(a1);
                const auto count = static_cast<std::uint32_t>(a3);
                const auto* descs = reinterpret_cast<const D3D12_RESOURCE_DESC1*>(a4);
                auto* info1 = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(words[4]);
                if (descs && count > 0) {
                    m->array[68][0] = static_cast<std::uint64_t>(descs[0].Layout);
                    m->array[68][1] = static_cast<std::uint64_t>(descs[0].Flags);
                    m->array[68][2] = descs[0].SamplerFeedbackMipRegion.Width |
                                      descs[0].SamplerFeedbackMipRegion.Height |
                                      descs[0].SamplerFeedbackMipRegion.Depth;
                    if (count > 1) {
                        const auto& last = descs[count - 1];
                        m->array[68][3] = (static_cast<std::uint64_t>(last.Layout) << 32) |
                                          static_cast<std::uint32_t>(last.Flags);
                    }
                }
                if (ret) *ret = {0x40000, 0x10000};
                if (info1 && count > 0) info1[0] = {0, 0x10000, 0x40000};
                return a1;
            }
            case 13: {  // CheckFeatureSupport(feature, data, size): FEATURE_LEVELS only, as the host answers it
                auto* levels = reinterpret_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(a2);
                if (static_cast<std::uint32_t>(a1) != D3D12_FEATURE_FEATURE_LEVELS || !levels || !m->max_level ||
                    static_cast<std::uint32_t>(a3) != sizeof(*levels) || !levels->pFeatureLevelsRequested)
                    return static_cast<std::uint32_t>(E_INVALIDARG);
                D3D_FEATURE_LEVEL best = D3D_FEATURE_LEVEL(0);
                for (UINT i = 0; i < levels->NumFeatureLevels; ++i)
                    if (levels->pFeatureLevelsRequested[i] <= m->max_level)
                        best = std::max(best, levels->pFeatureLevelsRequested[i]);
                levels->MaxSupportedFeatureLevel = best;
                return static_cast<std::uint32_t>(best ? S_OK : DXGI_ERROR_UNSUPPORTED);
            }
            default: break;
        }
        return S_OK;
    }
    if (m->kind == Kind::fence) {
        switch (slot) {
            case 8:  // GetCompletedValue
                if (m->completes_with && fence_completed(m) >= m->completes_at) {
                    fence_signal(m->completes_with, m->completes_value);
                    m->completes_with = nullptr;
                }
                return fence_completed(m);
            case 9: return static_cast<std::uint32_t>(fence_set_event(m, a1, reinterpret_cast<HANDLE>(a2)));
            case 10: fence_signal(m, a1); return S_OK;  // Signal
            default: break;
        }
    }
    if (m->kind == Kind::queue && slot == 10) {  // ExecuteCommandLists(count, lists)
        copy_array(m, 10, a2, a1);
        log_queue_call(m, 10, static_cast<std::uint32_t>(a1), a2 ? *reinterpret_cast<const std::uint64_t*>(a2) : 0);
    }
    if (m->kind == Kind::queue && (slot == 14 || slot == 15)) log_queue_call(m, slot, a1, a2);  // Signal, Wait
    if (m->kind == Kind::queue && slot == 14 && m->signals_complete && a1)
        fence_signal(reinterpret_cast<Mock*>(a1), a2);
    if ((m->kind == Kind::command_list || m->kind == Kind::list) && slot == 28)
        copy_array(m, slot, a2, a1);  // SetDescriptorHeaps(count, heaps)
    if (m->kind == Kind::queue && slot == 16 && a1) {
        // GetTimestampFrequency writes a fixed value — unless a1 is a mock object, which on this slot is a
        // newer-layout Signal's fence, not an out pointer.
        const auto at = static_cast<std::uintptr_t>(a1);
        const auto base = reinterpret_cast<std::uintptr_t>(g_pool);
        const bool is_mock = at >= base && at < base + sizeof(g_pool) && (at - base) % sizeof(Mock) == 0;
        if (!is_mock) *reinterpret_cast<UINT64*>(a1) = 1000000;
    }
    if (m->kind == Kind::queue && slot == 17 && a1 && a2) {
        *reinterpret_cast<UINT64*>(a1) = kMockGpu0;
        *reinterpret_cast<UINT64*>(a2) = mock_cpu_origin();
    }
    if (m->kind == Kind::resource && slot == 8 && a3) {  // Map(subresource, range, data): the test's readback
        *reinterpret_cast<void**>(a3) = g_mock_mapped;
    }
    if (m->kind == Kind::heap && slot == 9) {
        if (a1) reinterpret_cast<D3D12_CPU_DESCRIPTOR_HANDLE*>(a1)->ptr = kMockHeapCpuStart;
        return kMockHeapCpuStart;
    }
    if (m->kind == Kind::heap && slot == 10) {
        if (a1) reinterpret_cast<D3D12_GPU_DESCRIPTOR_HANDLE*>(a1)->ptr = kMockHeapGpuStart;
        return kMockHeapGpuStart;
    }
    if ((m->kind == Kind::list || m->kind == Kind::command_list) && slot == 46) {  // OMSetRenderTargets
        m->om_rtv_count = static_cast<UINT>(a1);
        m->om_single_range = static_cast<BOOL>(a3);
        std::memset(m->om_rtvs, 0, sizeof(m->om_rtvs));
        const auto* rtvs = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2);
        if (rtvs && m->om_rtv_count > 0) {
            const UINT n = m->om_single_range
                               ? 1u
                               : (m->om_rtv_count < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT
                                      ? m->om_rtv_count
                                      : D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT);
            for (UINT i = 0; i < n; ++i) m->om_rtvs[i] = rtvs[i].ptr;
        }
        const auto* dsv = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a4);
        m->om_has_dsv = dsv != nullptr;
        m->om_dsv = dsv ? dsv->ptr : 0;
    }
    if (m->kind == Kind::list || m->kind == Kind::command_list) {
        if (slot == 9) return static_cast<std::uint32_t>(m->list_error ? E_INVALIDARG : S_OK);  // Close
        if (slot == 48 && mock_is_null_rtv(static_cast<SIZE_T>(a1))) m->list_error = true;  // ClearRenderTargetView
        if (slot == 80) mock_barrier(m, a1, a2);                                             // Barrier
    }
    if (m->kind == Kind::queue && (slot == 14 || slot == 15) && m->observed_queue) {
        m->observed_queue_refs = xs::command_object_proxy_ref_count(m->observed_queue);
        m->observed_fence_refs = xs::command_object_proxy_ref_count(m->observed_fence);
    }
    return S_OK;
}

template <unsigned Slot>
std::uint64_t WINAPI mock_slot(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                               std::uint64_t a5, std::uint64_t a6) {
    if constexpr (Slot == 0) {
        return static_cast<std::uint32_t>(mock_query(self, reinterpret_cast<const IID*>(a1),
                                                     reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return static_cast<std::uint64_t>(++self->refs);
    } else if constexpr (Slot == 2) {
        const long left = --self->refs;
        if (left == 0 && self->held) {  // destroyed: private data goes with it
            IUnknown* const held = self->held;
            self->held = nullptr;
            held->Release();
        }
        return static_cast<std::uint64_t>(left);
    } else if constexpr (Slot == 5) {  // SetPrivateDataInterface(guid, object): keeps one reference
        ++self->calls[5];
        auto* const object = reinterpret_cast<IUnknown*>(a2);
        if (object) object->AddRef();
        if (self->held) self->held->Release();
        self->held = object;
        return S_OK;
    } else {
        // A caller may pass fewer words than this thunk declares (OpenSharedHandle passes three), so its frame
        // has no room past the four register homes. The words are copied into this frame and passed by pointer:
        // forwarding them as stack arguments let MSVC tail-call mock_call and write into the caller's saved
        // registers (an optimized build corrupted the test's own locals that way).
        ++self->calls[Slot];
        const std::uint64_t words[] = {a1, a2, a3, a4, a5, a6};
        std::memcpy(self->args[Slot], words, sizeof(words));
        return mock_call(self, Slot, words);
    }
}

void WINAPI mock_clear_depth_stencil_view(Mock* self, D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                          FLOAT depth, UINT8 stencil, UINT num_rects, const D3D12_RECT* rects) {
    ++self->calls[47];
    self->clear_dsv_handle = dsv.ptr;
    self->clear_dsv_flags = flags;
    self->clear_dsv_depth = depth;
    self->clear_dsv_stencil = stencil;
    self->clear_dsv_num_rects = num_rects;
    self->clear_dsv_rects = rects;
}

// PC ID3D12Device::CreateCommandList takes six words after `this`, one more than the generic mock thunk declares, so
// it gets its own entry in the table: the out pointer is a6 (first stack argument on x64) and has to be written.
HRESULT WINAPI mock_create_command_list(Mock* self, std::uint64_t node_mask, std::uint64_t type,
                                        std::uint64_t allocator, std::uint64_t initial_state, const IID* riid,
                                        void** out) {
    ++self->calls[12];
    self->args[12][0] = node_mask;
    self->args[12][1] = type;
    self->args[12][2] = allocator;
    self->args[12][3] = initial_state;
    self->args[12][4] = reinterpret_cast<std::uint64_t>(riid);
    if (riid) self->create_riid = *riid;
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->list_refuse) return E_NOINTERFACE;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(self->full_chain_lists ? Kind::list : Kind::command_list);
    child->list_level = self->list_level;
    self->last_child = child;
    return mock_query(child, riid, out);
}

// PC ID3D12Device::CreateCommittedResource (slot 27) takes seven words after `this`, so it has its own entry too. It
// records what it was given (the description by value) and makes a resource mock.
struct CommittedCall {
    D3D12_HEAP_PROPERTIES heap;
    D3D12_HEAP_FLAGS heap_flags;
    D3D12_RESOURCE_DESC desc;
    D3D12_RESOURCE_STATES state;
    const D3D12_CLEAR_VALUE* clear;
    const IID* riid;
};
CommittedCall g_committed{};

HRESULT WINAPI mock_create_committed_resource(Mock* self, const D3D12_HEAP_PROPERTIES* heap,
                                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* desc,
                                              D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                              const IID* riid, void** out) {
    ++self->calls[27];
    // The first words are kept as every other slot's are (the lease checks read them), and the table is shared by
    // every mock kind: only a device creates a resource here.
    const std::uint64_t words[6] = {reinterpret_cast<std::uint64_t>(heap), static_cast<std::uint64_t>(heap_flags),
                                    reinterpret_cast<std::uint64_t>(desc), static_cast<std::uint64_t>(state),
                                    reinterpret_cast<std::uint64_t>(clear), reinterpret_cast<std::uint64_t>(riid)};
    std::memcpy(self->args[27], words, 5 * sizeof(words[0]));
    const std::uint64_t rest[3] = {reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0};
    std::memcpy(self->tail[27], rest, sizeof(rest));
    if (self->kind != Kind::device) return static_cast<HRESULT>(mock_call(self, 27, words));
    // The initial-state checks pass made-up words for the clear value, riid and output: nothing is read or created.
    if (self->words_only) return S_OK;
    g_committed = CommittedCall{};
    if (heap) g_committed.heap = *heap;
    g_committed.heap_flags = heap_flags;
    if (desc) g_committed.desc = *desc;
    g_committed.state = state;
    g_committed.clear = clear;
    g_committed.riid = riid;
    if (!out) return S_FALSE;
    *out = nullptr;
    if (self->create_null || FAILED(self->create_result)) return self->create_result;
    Mock* child = new_mock(Kind::resource);
    self->last_child = child;
    return mock_query(child, riid, out);
}

#ifdef __ID3D12Device10_INTERFACE_DEFINED__
// PC ID3D12Device10::CreateCommittedResource3 takes ten words after `this`, so it has its own entry (PC slot 76) that
// keeps all of them: the resource description is copied because the caller's copy is a local of the proxy.
struct CommittedResource3Call {
    unsigned calls;
    const Mock* device;  // the PC object the call arrived on
    const D3D12_HEAP_PROPERTIES* heap;
    D3D12_HEAP_FLAGS heap_flags;
    D3D12_RESOURCE_DESC1 desc;
    std::uint32_t layout;
    const D3D12_CLEAR_VALUE* clear;
    const void* session;
    std::uint32_t castable_count;
    const DXGI_FORMAT* castable_formats;
    const IID* riid;
    void** out;
};
CommittedResource3Call g_committed3;

HRESULT WINAPI mock_create_committed_resource3(Mock* self, const D3D12_HEAP_PROPERTIES* heap, D3D12_HEAP_FLAGS flags,
                                               const D3D12_RESOURCE_DESC1* desc, std::uint32_t layout,
                                               const D3D12_CLEAR_VALUE* clear, const void* session,
                                               std::uint32_t castable_count, const DXGI_FORMAT* castable_formats,
                                               const IID* riid, void** out) {
    g_committed3 = {g_committed3.calls + 1, self, heap, flags, *desc, layout, clear, session, castable_count,
                    castable_formats, riid, out};
    return mock_create(self, Kind::resource, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out));
}
#endif

// PC CreateCommittedResource (27) and CreateCommittedResource1 (53) take seven and eight words after `this`; the generic
// thunk keeps five, so these two record the rest as well (a6..a8 in tail).
template <unsigned Slot>
std::uint64_t WINAPI mock_committed_resource(Mock* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                             std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                             std::uint64_t a8) {
    ++self->calls[Slot];
    const std::uint64_t head[] = {a1, a2, a3, a4, a5};
    const std::uint64_t rest[] = {a6, a7, a8};
    std::memcpy(self->args[Slot], head, sizeof(head));
    std::memcpy(self->tail[Slot], rest, sizeof(rest));
    return S_OK;
}

template <std::size_t... I>
void fill_mock_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&mock_slot<static_cast<unsigned>(I)>)), ...);
}

void** mock_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[12] = reinterpret_cast<void*>(&mock_create_command_list);
        table[27] = reinterpret_cast<void*>(&mock_create_committed_resource);
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
        table[76] = reinterpret_cast<void*>(&mock_create_committed_resource3);
#endif
        table[53] = reinterpret_cast<void*>(&mock_committed_resource<53>);
        return true;
    }();
    (void)filled;
    return table;
}

void** mock_list_vtable() {
    static void* table[kMockSlots];
    static const bool filled = [] {
        fill_mock_vtable(table, std::make_index_sequence<kMockSlots>{});
        table[47] = reinterpret_cast<void*>(&mock_clear_depth_stencil_view);
        return true;
    }();
    (void)filled;
    return table;
}

// ---- Calls by slot, the way a title compiled against the console headers makes them ----

using SlotFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);

std::uint64_t raw(const void* object, unsigned slot, std::uint64_t a1 = 0, std::uint64_t a2 = 0,
                  std::uint64_t a3 = 0, std::uint64_t a4 = 0, std::uint64_t a5 = 0, std::uint64_t a6 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, 0, 0, 0, 0,
                                                  0, 0, 0, 0);
}

// The same with a sixth word, the first stack argument (slot 133's output pointer).
std::uint64_t raw6(const void* object, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                   std::uint64_t a4, std::uint64_t a5, std::uint64_t a6) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, 0, 0, 0, 0, 0,
                                                  0, 0, 0);
}

// A call with the ten words of device slot 127 (and any other slot that takes up to ten).
std::uint64_t raw10(const void* object, unsigned slot, const std::uint64_t (&w)[10]) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                                  w[7], w[8], w[9], 0, 0, 0, 0);
}

HRESULT hr_of(std::uint64_t value) {
    return static_cast<HRESULT>(static_cast<std::uint32_t>(value));
}

template <class T>
std::uint64_t word(const T* p) {
    return reinterpret_cast<std::uint64_t>(p);
}

const IID kQueueIid = __uuidof(ID3D12CommandQueue);
const IID kFenceIid = __uuidof(ID3D12Fence);
const IID kFence1Iid = __uuidof(ID3D12Fence1);
const IID kUnknownIid = __uuidof(IUnknown);
const IID kResourceIid = __uuidof(ID3D12Resource);
const IID kDevice1Iid = __uuidof(ID3D12Device1);
const D3D12_COMMAND_QUEUE_DESC kQueueDesc{};
// The console's CreateCommandQueueX description: the PC description's 16 bytes, then four Xbox words (slot 48).
const std::uint32_t kXboxQueueDesc[8] = {};

// A device proxy over a new mock device. The proxy takes the mock's one reference.
void* make_device(Mock*& mock, int level, bool fence1, bool allocator_tag = true, bool full_chain_lists = false) {
    mock = new_mock(Kind::device);
    mock->device_level = level;
    mock->fence1 = fence1;
    mock->allocator_tag = allocator_tag;
    mock->full_chain_lists = full_chain_lists;
    mock->refs = 1;
    void* device = nullptr;
    if (FAILED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(mock), __uuidof(ID3D12Device), &device,
                                       /*console_lists=*/true)))
        return nullptr;
    return device;
}

void* make_queue(void* device) {
    void* queue = nullptr;
    return SUCCEEDED(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&queue)))) ? queue : nullptr;
}

void* make_fence(void* device, const IID& iid = kFenceIid) {
    void* fence = nullptr;
    return SUCCEEDED(hr_of(raw(device, 36, 0, 0, word(&iid), word(&fence)))) ? fence : nullptr;
}

unsigned long release(void* object) {
    return static_cast<unsigned long>(raw(object, 2));
}

// ---- 0. The host device's feature-level floor (BL-2) ----

// A host D3D12CreateDevice whose adapter tops out at g_host_max: a minimum above it is refused with E_INVALIDARG,
// as on a host that tops out at 11_x, and g_host_mode makes it fail outright or succeed with no object.
D3D_FEATURE_LEVEL g_host_max = D3D_FEATURE_LEVEL_12_1;
D3D_FEATURE_LEVEL g_host_minimum = D3D_FEATURE_LEVEL(0);  // the minimum the last call asked for
enum class HostMode { normal, fail, no_object } g_host_mode = HostMode::normal;

HRESULT WINAPI fake_create_device(IUnknown*, D3D_FEATURE_LEVEL minimum, REFIID riid, void** out) {
    g_host_minimum = minimum;
    if (!out) return S_FALSE;
    *out = nullptr;
    if (g_host_mode == HostMode::fail) return DXGI_ERROR_UNSUPPORTED;
    if (g_host_mode == HostMode::no_object) return S_OK;
    if (minimum > g_host_max) return E_INVALIDARG;
    Mock* m = new_mock(Kind::device);
    m->max_level = g_host_max;
    return mock_query(m, &riid, out);
}

int check_host_device_floor() {
    g_label = "xs_proxy_mock host feature level";
    EXPECT(xs::kHostDeviceFloor == D3D_FEATURE_LEVEL_11_0);
    const D3D_FEATURE_LEVEL tops[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    for (const D3D_FEATURE_LEVEL top : tops) {
        g_host_max = top;
        // The floor 12_0 the host path used to ask for refuses the adapters that top out at 11_x.
        void* old = nullptr;
        const HRESULT old_hr = fake_create_device(nullptr, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), &old);
        EXPECT(FAILED(old_hr) == (top < D3D_FEATURE_LEVEL_12_0));
        if (old) release(old);

        ID3D12Device* real = nullptr;
        EXPECT(xs::create_host_device(&fake_create_device, nullptr, &real) == S_OK && real);
        EXPECT(g_host_minimum == D3D_FEATURE_LEVEL_11_0);
        EXPECT(xs::max_feature_level(real) == top);   // the device keeps its highest level
        // The title asks the device it gets (the Xbox-ABI proxy) through slot 13, CheckFeatureSupport.
        void* device = nullptr;
        EXPECT(SUCCEEDED(xs::create_device_proxy(real, __uuidof(ID3D12Device), &device)) && device);
        D3D12_FEATURE_DATA_FEATURE_LEVELS levels{UINT(std::size(tops)), tops, D3D_FEATURE_LEVEL(0)};
        EXPECT(hr_of(raw(device, 13, D3D12_FEATURE_FEATURE_LEVELS, word(&levels), sizeof(levels))) == S_OK);
        EXPECT(levels.MaxSupportedFeatureLevel == top);
        release(device);
    }
    g_host_max = D3D_FEATURE_LEVEL_12_1;
    ID3D12Device* real = reinterpret_cast<ID3D12Device*>(&g_host_max);   // must be cleared on every failure
    EXPECT(xs::create_host_device(nullptr, nullptr, &real) == E_NOTIMPL && !real);
    EXPECT(xs::create_host_device(&fake_create_device, nullptr, nullptr) == E_POINTER);
    g_host_mode = HostMode::fail;
    real = reinterpret_cast<ID3D12Device*>(&g_host_max);
    EXPECT(xs::create_host_device(&fake_create_device, nullptr, &real) == DXGI_ERROR_UNSUPPORTED && !real);
    g_host_mode = HostMode::no_object;
    real = reinterpret_cast<ID3D12Device*>(&g_host_max);
    EXPECT(xs::create_host_device(&fake_create_device, nullptr, &real) == E_FAIL && !real);
    g_host_mode = HostMode::normal;
    EXPECT(xs::max_feature_level(nullptr) == D3D_FEATURE_LEVEL(0));
    g_label = "xs_proxy_mock";
    return 0;
}

// ---- 1. Device interface levels ----

// D3D12_RESOURCE_STATES is a 32-bit enum: MSVC stores only the low half of its 8-byte stack slot.
std::uint32_t state_of(std::uint64_t word) { return static_cast<std::uint32_t>(word); }

int check_levels() {
    struct Level { int level; unsigned slots; } const levels[] = {{0, 44}, {1, 47}, {3, 51}, {9, 76}};
    for (const Level& l : levels) {
        Mock* mock = nullptr;
        void* device = make_device(mock, l.level, true);
        EXPECT(device && xs::device_proxy_slot_count(device) == l.slots);
        void* queue = make_queue(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_queue_proxy(queue) && xs::is_fence_proxy(fence));
        const auto real_queue = word(xs::queue_proxy_real_queue(queue));
        const auto real_fence = word(xs::fence_proxy_real_fence(fence));
        EXPECT(real_queue && real_fence && real_queue != word(queue) && real_fence != word(fence));
        // What every residency call gets, with the proxies in it replaced by their real objects.
        const std::uint64_t objects[4] = {word(queue), word(fence), real_queue, 0};
        const std::uint64_t unwrapped[4] = {real_queue, real_fence, real_queue, 0};
        const std::uint64_t fences[2] = {word(fence), word(fence)};
        const std::uint64_t values[2] = {1, 2};
        const D3D12_RESIDENCY_PRIORITY priorities[4] = {};
        const auto same = [](const std::uint64_t* got, const std::uint64_t* want, int n) {
            return std::memcmp(got, want, n * sizeof(std::uint64_t)) == 0;
        };

        // 34 MakeResident, with a count whose high dword is not zero: the PC prototype takes a UINT.
        EXPECT(hr_of(raw(device, 34, 0xdeadbeef00000002ull, word(objects))) == S_OK);
        EXPECT(mock->calls[34] == 1 && static_cast<std::uint32_t>(mock->args[34][0]) == 2);
        EXPECT(same(mock->array[34], unwrapped, 2) && mock->array[34][2] == 0);
        // 31 CreateSharedHandle
        EXPECT(hr_of(raw(device, 31, word(fence), 0, GENERIC_ALL, 0, 0)) == S_OK);
        EXPECT(mock->calls[31] == 1 && mock->args[31][0] == real_fence);

        // 45/46: ID3D12Device1
        const HRESULT r45 = hr_of(raw(device, 45, word(fences), word(values), 2, 0, 0));
        const HRESULT r46 = hr_of(raw(device, 46, 4, word(objects), word(priorities)));
        if (l.level >= 1) {
            EXPECT(r45 == S_OK && mock->calls[45] == 1 && mock->array[45][0] == real_fence &&
                   mock->array[45][1] == real_fence);
            EXPECT(r46 == S_OK && mock->calls[46] == 1 && same(mock->array[46], unwrapped, 4));
        } else {
            EXPECT(r45 == E_NOTIMPL && r46 == E_NOTIMPL && mock->calls[45] == 0 && mock->calls[46] == 0);
            void* device1 = nullptr;
            EXPECT(FAILED(hr_of(raw(device, 0, word(&kDevice1Iid), word(&device1)))) && !device1);
        }
        // 50: ID3D12Device3, with the fence argument unwrapped too
        const HRESULT r50 = hr_of(raw(device, 50, 0, 4, word(objects), word(fence), 1));
        if (l.level >= 3) {
            EXPECT(r50 == S_OK && mock->calls[50] == 1 && same(mock->array[50], unwrapped, 4) &&
                   mock->args[50][3] == real_fence);
        } else {
            EXPECT(r50 == E_NOTIMPL && mock->calls[50] == 0);
        }
        // 75: ID3D12Device9::CreateCommandQueue1
        void* queue1 = reinterpret_cast<void*>(1);  // cleared on every failure (bc59ef46's convention)
        const HRESULT r75 = hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&queue1)));
        if (l.level >= 9) {
            EXPECT(r75 == S_OK && xs::is_queue_proxy(queue1) && mock->calls[75] == 1);
            EXPECT(release(queue1) == 0 && mock->last_child->refs == 0);
        } else {
            EXPECT(r75 == E_NOTIMPL && !queue1 && mock->calls[75] == 0);
        }

        // 134: past every PC device vtable and not handled by the proxy (133 is, see check_placed_resource_x).
        // Refused, not a jump through a missing entry.
        EXPECT(hr_of(raw(device, 134)) == E_NOTIMPL && mock->calls[127] == 0);

        // 133 with a handler (CreatePlacedResourceX): the arguments in the title's positions (va, desc, state,
        // clear, riid, out) reach the handler with the real device, the Xbox flag bits masked to the PC ones the host
        // takes (0x20004 -> 0x4, and 0x200 dropped) and the buffer's Xbox layout word made ROW_MAJOR.
        {
            static void* seen_real = nullptr;
            static void* seen_va = nullptr;
            static D3D12_RESOURCE_DESC seen_desc{};
            static GUID seen_iid{};
            static std::uint32_t seen_layout = 0;
            const xs::PlacedAtVaHandler handler = [](ID3D12Device* real_device, void* va,
                                                     const D3D12_RESOURCE_DESC& desc, std::uint32_t layout,
                                                     REFIID iid, void** out, const D3D12_CLEAR_VALUE*) -> HRESULT {
                seen_layout = layout;
                seen_real = real_device;
                seen_va = va;
                seen_desc = desc;
                seen_iid = iid;
                *out = reinterpret_cast<void*>(0x1330);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0x10000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);  // the Xbox row-major word
            xbox_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x20204);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000000000ull);
            const HRESULT r133 = hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0,
                                           word(&resource_iid), word(&placed)));
            xs::set_placed_at_va_handler(nullptr);
            EXPECT(r133 == S_OK && placed == reinterpret_cast<void*>(0x1330));
            EXPECT(seen_real == xs::device_proxy_real_device(device) && seen_va == va);
            EXPECT(static_cast<std::uint32_t>(seen_desc.Flags) == 0x4);
            EXPECT(seen_desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && seen_desc.Width == 0x10000);
            EXPECT(std::memcmp(&seen_iid, &resource_iid, sizeof(GUID)) == 0);
            EXPECT(seen_layout == 0xFFFFFFFFu);  // the title's third word, passed through as the barrier layout
            EXPECT(xs::device_slot_overridden(133));
        }

        // 133 with a pages hook (set_placed_at_va_pages_hook): a buffer's range [va, va + width) is handed to it
        // before the handler runs; a texture is not; a hook that fails still leaves the call to the handler.
        {
            static int order = 0;
            static int hook_at = 0;
            static int handler_at = 0;
            static const void* hook_va = nullptr;
            static std::size_t hook_bytes = 0;
            static bool hook_result = true;
            order = hook_at = handler_at = 0;
            const xs::PlacedAtVaPagesHook hook = [](const void* va, std::size_t bytes) -> bool {
                hook_at = ++order;
                hook_va = va;
                hook_bytes = bytes;
                return hook_result;
            };
            const xs::PlacedAtVaHandler handler = [](ID3D12Device*, void*, const D3D12_RESOURCE_DESC&, std::uint32_t,
                                                     REFIID, void** out, const D3D12_CLEAR_VALUE*) -> HRESULT {
                handler_at = ++order;
                *out = reinterpret_cast<void*>(0x1331);
                return S_OK;
            };
            xs::set_placed_at_va_handler(handler);
            xs::set_placed_at_va_pages_hook(hook);
            D3D12_RESOURCE_DESC xbox_desc{};
            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            xbox_desc.Width = 0xadf0000;
            xbox_desc.Height = xbox_desc.DepthOrArraySize = xbox_desc.MipLevels = 1;
            xbox_desc.SampleDesc.Count = 1;
            xbox_desc.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x1);
            const GUID resource_iid = __uuidof(ID3D12Resource);
            void* placed = nullptr;
            void* const va = reinterpret_cast<void*>(0x7f0000010000ull);
            auto place = [&]() {
                placed = nullptr;
                return hr_of(raw(device, 133, word(va), word(&xbox_desc), 0xFFFFFFFFull, 0, word(&resource_iid),
                                 word(&placed)));
            };
            const HRESULT buffer_hr = place();
            EXPECT(buffer_hr == S_OK && placed == reinterpret_cast<void*>(0x1331));
            EXPECT(hook_at == 1 && handler_at == 2 && hook_va == va && hook_bytes == 0xadf0000);

            hook_result = false;
            order = hook_at = handler_at = 0;
            const HRESULT refused_hr = place();
            EXPECT(refused_hr == S_OK && hook_at == 1 && handler_at == 2);
            hook_result = true;

            xbox_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            xbox_desc.Width = 64;
            xbox_desc.Height = 64;
            order = hook_at = handler_at = 0;
            const HRESULT texture_hr = place();
            EXPECT(texture_hr == S_OK && hook_at == 0 && handler_at == 1);

            xs::set_placed_at_va_pages_hook(nullptr);
            xs::set_placed_at_va_handler(nullptr);
        }

        // 86: Xbox CreatePipelineState -> PC slot 47 (ID3D12Device2)
        void* pso_out = reinterpret_cast<void*>(1);
        const HRESULT r86 = hr_of(raw(device, 86, 0, word(&kUnknownIid), word(&pso_out)));
        if (l.level >= 2) {
            EXPECT(r86 == S_OK && mock->calls[47] == 1 && mock->calls[86] == 0);
        } else {
            EXPECT(r86 == E_NOTIMPL && !pso_out && mock->calls[47] == 0 && mock->calls[86] == 0);
        }

        // 101: Xbox CreateStateObject -> PC slot 62 (ID3D12Device5)
        void* so_out = reinterpret_cast<void*>(1);
        const HRESULT r101 = hr_of(raw(device, 101, 0, word(&kUnknownIid), word(&so_out)));
        if (l.level >= 5) {
            EXPECT(r101 == S_OK && mock->calls[62] == 1 && mock->calls[101] == 0);
        } else {
            EXPECT(r101 == E_NOTIMPL && !so_out && mock->calls[62] == 0 && mock->calls[101] == 0);
        }

        // 108: Xbox GetResourceAllocationInfo2 -> PC slot 68 (ID3D12Device8)
        D3D12_RESOURCE_ALLOCATION_INFO alloc_info{1, 1};
        const std::uint64_t r108 = raw(device, 108, word(&alloc_info), 1, 0, 0, 0);
        if (l.level >= 8) {
            EXPECT(r108 == word(&alloc_info) && alloc_info.SizeInBytes == 0x40000 && alloc_info.Alignment == 0x10000 &&
                   mock->calls[68] == 1 && mock->calls[108] == 0);
        } else {
            EXPECT(r108 == word(&alloc_info) && alloc_info.SizeInBytes == UINT64_MAX && alloc_info.Alignment == 0 &&
                   mock->calls[68] == 0 && mock->calls[108] == 0);
        }

        Mock* const queue_mock = reinterpret_cast<Mock*>(real_queue);
        Mock* const fence_mock = reinterpret_cast<Mock*>(real_fence);
        EXPECT(release(queue) == 0 && release(fence) == 0);
        EXPECT(queue_mock->refs == 0 && fence_mock->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: devices up to ID3D12Device, Device1, Device3, Device9: 44/47/51/76 slots, 45/46/50/75 refused "
                "without the interface, residency arrays and fences unwrapped otherwise\n", g_label);
    return 0;
}

// ---- 1b. Slot 48: the title's CreateCommandQueue with a 32-byte description ----

int check_slot48() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 3, true);
    EXPECT(device);
    // 16-byte PC prefix, then four Xbox words; the flags carry a bit the PC does not know besides the timeout bit.
    const std::uint32_t desc[8] = {D3D12_COMMAND_LIST_TYPE_COMPUTE, 100,
                                   0x8000u | D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT, 1, 0x11, 0x22, 0x33, 0x44};
    void* queue = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, word(desc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[8] == 1);
    EXPECT(mock->queue_desc.Type == D3D12_COMMAND_LIST_TYPE_COMPUTE && mock->queue_desc.Priority == 100 &&
           mock->queue_desc.Flags == D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT &&
           mock->queue_desc.NodeMask == 1);
    EXPECT(release(queue) == 0);
    void* none = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 48, 0, word(&kQueueIid), word(&none))) == E_INVALIDARG && !none);
    EXPECT(mock->calls[8] == 1);
    EXPECT(release(device) == 0);
    std::printf("%s: slot 48 makes a queue proxy from a 32-byte description, PC prefix and flags only\n", g_label);
    return 0;
}

// ---- 2. Fences with and without ID3D12Fence1 ----

int check_fence1() {
    for (const bool fence1 : {false, true}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* fence = make_fence(device);
        EXPECT(xs::is_fence_proxy(fence) && xs::fence_proxy_slot_count(fence) == (fence1 ? 12u : 11u));
        Mock* const fence_mock = mock->last_child;
        void* as_fence1 = nullptr;
        const HRESULT qi = hr_of(raw(fence, 0, word(&kFence1Iid), word(&as_fence1)));
        if (fence1) {
            EXPECT(qi == S_OK && as_fence1 == fence && release(as_fence1) == 1);
        } else {
            EXPECT(qi == E_NOINTERFACE && !as_fence1);
        }
        EXPECT(raw(fence, 8) == kCompletedValue);  // GetCompletedValue forwarded
        const HRESULT slot11 = hr_of(raw(fence, 11));
        if (fence1) {
            EXPECT(slot11 == S_OK && fence_mock->calls[11] == 1);
        } else {
            EXPECT(slot11 == E_NOTIMPL && fence_mock->calls[11] == 0);  // not forwarded past an 11-slot vtable
        }
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);

        // Asked for as ID3D12Fence1: made as ID3D12Fence, then refused when the fence lacks Fence1, and released.
        void* created1 = nullptr;
        const HRESULT made1 = hr_of(raw(device, 36, 0, 0, word(&kFence1Iid), word(&created1)));
        if (fence1) {
            EXPECT(made1 == S_OK && xs::is_fence_proxy(created1) && release(created1) == 0);
        } else {
            EXPECT(made1 == E_NOINTERFACE && !created1);
        }
        EXPECT(mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fences without and with ID3D12Fence1: 11/12 slots, QueryInterface, slot 11 and creation as "
                "Fence1 refused or forwarded to match\n", g_label);
    return 0;
}

// ---- Fence slot 12: the completed value at an address ----

std::atomic<int> g_descriptor_calls{0};
void WINAPI descriptor_callback() { g_descriptor_calls.fetch_add(1); }

// True once *completed reads `value`, within five seconds (the proxy's wait thread writes it).
bool reads_within(const volatile std::uint64_t* completed, std::uint64_t value) {
    for (int i = 0; i < 5000; ++i) {
        if (*completed == value) return true;
        Sleep(1);
    }
    return *completed == value;
}

bool fence_armed_for(Mock* fence, std::uint64_t value) {
    AcquireSRWLockExclusive(&fence->lock);
    const bool armed = fence->armed_event && fence->armed_value == value;
    ReleaseSRWLockExclusive(&fence->lock);
    return armed;
}

int check_fence_completed_value() {
    for (const bool fence1 : {false, true}) {  // past ID3D12Fence1 either way
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, fence1);
        EXPECT(device);
        void* fence = make_fence(device);
        Mock* const fence_mock = mock->last_child;
        void* other = make_fence(device);
        Mock* const other_mock = mock->last_child;
        EXPECT(fence && other && fence_mock != other_mock);

        // The title's shape: (value, descriptor with a code pointer in its second word, flags).
        const std::uint64_t descriptor[4] = {0, reinterpret_cast<std::uint64_t>(&descriptor_callback), 0, 0};
        const auto* completed =
            reinterpret_cast<const volatile std::uint64_t*>(raw(fence, 12, 9, word(descriptor), 1));
        EXPECT(completed && *completed == kCompletedValue);
        EXPECT(xs::fence_slot_first_call_seen(12));
        EXPECT(raw(fence, 12, 9, word(descriptor), 1) == word(completed));  // one address per fence
        const auto* other_completed = reinterpret_cast<const volatile std::uint64_t*>(raw(other, 12, 0, 0, 0));
        EXPECT(other_completed && other_completed != completed);

        // A CPU Signal through the proxy shows at once.
        EXPECT(hr_of(raw(fence, 10, 9)) == S_OK && fence_mock->completed == 9);
        EXPECT(*completed == 9 && *other_completed == kCompletedValue);
        // A signal the proxy never sees (a queue's, on the GPU) shows without another call, and again after it.
        fence_signal(fence_mock, 12);
        EXPECT(reads_within(completed, 12));
        fence_signal(fence_mock, 40);
        EXPECT(reads_within(completed, 40));
        fence_signal(other_mock, 8);
        EXPECT(reads_within(other_completed, 8) && *completed == 40);
        // Wait for the wait thread to arm its next threshold before rewinding.
        const ULONGLONG until = GetTickCount64() + 1000;
        while (!fence_armed_for(fence_mock, 41) && GetTickCount64() < until) Sleep(1);
        EXPECT(fence_armed_for(fence_mock, 41));
        EXPECT(hr_of(raw(fence, 10, 3)) == S_OK && *completed == 3);
        fence_signal(fence_mock, 15);
        EXPECT(fence_completed(fence_mock) == 15);
        if (!reads_within(completed, 15)) {
            std::fprintf(stderr, "rewind: underlying fence %llu, slot 12 mirror %llu\n",
                         static_cast<unsigned long long>(fence_completed(fence_mock)),
                         static_cast<unsigned long long>(*completed));
            return 1;
        }
        // A rewind by a producer outside the proxy can go straight below its outstanding event.
        fence_signal(fence_mock, 2);
        EXPECT(reads_within(completed, 2));
        fence_signal(fence_mock, 7);
        EXPECT(reads_within(completed, 7));
        // The fallback must leave the pending completion event alone.
        EXPECT(fence_armed_for(fence_mock, 41));
        fence_signal(fence_mock, 45);
        EXPECT(reads_within(completed, 45));
        // Device removal: the real fence reports ~0, which the title tests for, and nothing is armed after it.
        fence_signal(fence_mock, ~std::uint64_t{0});
        EXPECT(reads_within(completed, ~std::uint64_t{0}));
        Sleep(50);
        AcquireSRWLockExclusive(&fence_mock->lock);
        const bool rearmed = fence_mock->armed_event != nullptr;
        ReleaseSRWLockExclusive(&fence_mock->lock);
        EXPECT(!rearmed);
        EXPECT(g_descriptor_calls.load() == 0);  // the descriptor's code pointer is not called

        // Release waits out the wait thread; both fences and the device go back to zero references.
        EXPECT(release(fence) == 0 && fence_mock->refs == 0);
        EXPECT(release(other) == 0 && other_mock->refs == 0);

        // A fence whose slot 12 is never called arms nothing on the real fence.
        void* quiet = make_fence(device);
        Mock* const quiet_mock = mock->last_child;
        EXPECT(quiet && hr_of(raw(quiet, 10, 3)) == S_OK && raw(quiet, 8) == 3 && quiet_mock->calls[9] == 0);
        EXPECT(release(quiet) == 0 && quiet_mock->refs == 0);

        // Another reference keeps the real fence alive past the proxy. Its pending completion still names the
        // mirror's event and cannot be cancelled, so the event stays open and a later signal sets it.
        void* shared = make_fence(device);
        Mock* const shared_mock = mock->last_child;
        EXPECT(shared && raw(shared, 12, 0, 0, 0) != 0);
        const ULONGLONG armed_by = GetTickCount64() + 1000;
        while (!fence_armed_for(shared_mock, kCompletedValue + 1) && GetTickCount64() < armed_by) Sleep(1);
        EXPECT(fence_armed_for(shared_mock, kCompletedValue + 1));
        ++shared_mock->refs;  // the other holder
        EXPECT(release(shared) == 0 && shared_mock->refs == 1);
        AcquireSRWLockExclusive(&shared_mock->lock);
        const HANDLE pending = shared_mock->armed_event;
        ReleaseSRWLockExclusive(&shared_mock->lock);
        DWORD handle_flags = 0;
        EXPECT(pending && GetHandleInformation(pending, &handle_flags));
        fence_signal(shared_mock, kCompletedValue + 1);
        EXPECT(WaitForSingleObject(pending, 0) == WAIT_OBJECT_0);
        --shared_mock->refs;

        // Out of memory for the mirror: slot 12 returns null, and the next call works.
        void* starved = make_fence(device);
        EXPECT(starved);
        g_fail_news = 1;
        const std::uint64_t none = raw(starved, 12, 0, 0, 0);
        g_fail_news = 0;
        EXPECT(none == 0);
        const auto* later = reinterpret_cast<const volatile std::uint64_t*>(raw(starved, 12, 0, 0, 0));
        EXPECT(later && *later == kCompletedValue);
        EXPECT(release(starved) == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: fence slot 12 returns one address per fence whose completed value follows CPU and unseen "
                "signals up to device removal; the descriptor is not called\n", g_label);
    return 0;
}

// ---- 3. Creation failures and refused interfaces ----

int check_create_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* out = reinterpret_cast<void*>(1);

    mock->create_null = true;  // S_OK with no object
    mock->create_result = S_OK;
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_FAIL && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))) == E_FAIL && !out);
    mock->create_null = false;
    mock->create_result = E_OUTOFMEMORY;  // the real failure, passed on
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, word(&kQueueIid), word(&out))) == E_OUTOFMEMORY && !out);
    mock->create_result = S_OK;

    const unsigned calls8 = mock->calls[8];
    out = reinterpret_cast<void*>(1);  // a null IID: E_POINTER, the output cleared, nothing created
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), 0, word(&out))) == E_POINTER && !out && mock->calls[8] == calls8);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 36, 0, 0, 0, word(&out))) == E_POINTER && !out);
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 75, word(&kQueueDesc), 0, 0, word(&out))) == E_POINTER && !out);
    // No output: the device's own answer (S_FALSE here), as the PC runtime gives it.
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), 0)) == S_FALSE && mock->calls[8] == calls8 + 1);
    EXPECT(hr_of(raw(device, 36, 0, 0, word(&kFenceIid), 0)) == S_FALSE);

    // An interface the real queue implements but the proxy does not model: created, refused, released.
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 8, word(&kQueueDesc), word(&kExtraIid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = mock->last_child;
    void* extra = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(queue, 0, word(&kExtraIid), word(&extra))) == E_NOINTERFACE && !extra);
    EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && queue_mock->refs == 1);
    EXPECT(release(queue) == 0 && queue_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: creation with no object (E_FAIL), a failing device, null IID and output, and an interface the "
                "proxy does not model leave no reference\n", g_label);
    return 0;
}

// ---- 4. OpenSharedHandle ----

int check_open_shared() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, false);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);

    for (const IID* iid : {&kFenceIid, &kUnknownIid}) {  // a fence, whatever it is asked for as, comes back wrapped
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kFenceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const fence_mock = mock->last_child;
        EXPECT(xs::is_fence_proxy(opened) && word(xs::fence_proxy_real_fence(opened)) == word(fence_mock));
        EXPECT(xs::command_object_proxy_ref_count(opened) == 1 && fence_mock->refs == 1);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs + 1);
        EXPECT(release(opened) == 0 && fence_mock->refs == 0);
    }
    void* out = reinterpret_cast<void*>(1);  // as Fence1, which this fence lacks: the real open fails, passed on
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFence1Iid), word(&out))) == E_NOINTERFACE && !out);
    EXPECT(mock->last_child->refs == 0);

    for (const IID* iid : {&kResourceIid, &kUnknownIid}) {  // not a fence: the real object, unchanged
        void* opened = nullptr;
        EXPECT(hr_of(raw(device, 32, kResourceHandle, word(iid), word(&opened))) == S_OK);
        Mock* const resource_mock = mock->last_child;
        EXPECT(opened == resource_mock && !xs::is_fence_proxy(opened) && !xs::is_queue_proxy(opened));
        EXPECT(resource_mock->refs == 1 && release(opened) == 0);
    }
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);

    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, 0xbad, word(&kFenceIid), word(&out))) == E_INVALIDARG && !out);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), 0)) == S_FALSE);  // the device's own answer
    out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 32, kFenceHandle, 0, word(&out))) == E_POINTER && !out);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: OpenSharedHandle wraps a fence (as ID3D12Fence and IUnknown), passes a resource, a failed open, "
                "an invalid handle and a null output through, with balanced references\n", g_label);
    return 0;
}

// ---- 5. Allocation failures in the proxy ----

int check_alloc_failures() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    const std::uint64_t objects[2] = {word(queue), word(fence)};

    const auto failing = [&](auto call) {  // one allocation refused, the result of the call
        const int before = g_failed_news;
        g_fail_news = 1;
        const HRESULT hr = call();
        g_fail_news = 0;
        return g_failed_news == before + 1 ? hr : S_OK;
    };
    void* out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 36, 0, 0, word(&kFenceIid), word(&out))); }) == E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);  // the real fence was made and released
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 8, word(&kQueueDesc), word(&kQueueIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    out = reinterpret_cast<void*>(1);
    EXPECT(failing([&] { return hr_of(raw(device, 32, kFenceHandle, word(&kFenceIid), word(&out))); }) ==
           E_OUTOFMEMORY);
    EXPECT(!out && mock->last_child->refs == 0);
    // Up to 16 entries the unwrapped copy is inline: an armed failure is not even reached, and the call goes on.
    g_fail_news = 1;
    const HRESULT inline_result = hr_of(raw(device, 34, 2, word(objects)));
    const bool untouched = g_fail_news == 1;
    g_fail_news = 0;
    EXPECT(inline_result == S_OK && untouched && mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)));

    // A longer array is allocated; refused, the call returns E_OUTOFMEMORY and reaches no device.
    constexpr unsigned kLong = 32;
    std::uint64_t many[kLong], many_fences[kLong], values[kLong];
    D3D12_RESIDENCY_PRIORITY priorities[kLong] = {};
    for (unsigned i = 0; i < kLong; ++i) {
        many[i] = (i % 2 != 0) ? word(fence) : word(queue);
        many_fences[i] = word(fence);
        values[i] = 1;
    }
    const unsigned calls34 = mock->calls[34], calls45 = mock->calls[45], calls46 = mock->calls[46];
    const unsigned calls50 = mock->calls[50];
    EXPECT(failing([&] { return hr_of(raw(device, 34, kLong, word(many))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 45, word(many_fences), word(values), kLong, 0, 0)); }) ==
           E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 46, kLong, word(many), word(priorities))); }) == E_OUTOFMEMORY);
    EXPECT(failing([&] { return hr_of(raw(device, 50, 0, kLong, word(many), word(fence), 1)); }) == E_OUTOFMEMORY);
    EXPECT(mock->calls[34] == calls34 && mock->calls[45] == calls45 && mock->calls[46] == calls46 &&
           mock->calls[50] == calls50);  // nothing reached the device
    // And without the failure the long array arrives unwrapped.
    EXPECT(hr_of(raw(device, 34, kLong, word(many))) == S_OK && mock->calls[34] == calls34 + 1);
    EXPECT(mock->array[34][0] == word(xs::queue_proxy_real_queue(queue)) &&
           mock->array[34][1] == word(xs::fence_proxy_real_fence(fence)));

    // The device proxy itself: on failure the caller keeps its reference.
    Mock* other = new_mock(Kind::device);
    other->refs = 1;
    void* second = reinterpret_cast<void*>(1);
    EXPECT(failing([&] {
               return xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(other), __uuidof(ID3D12Device),
                                              &second);
           }) == E_OUTOFMEMORY);
    EXPECT(!second && other->refs == 1);

    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    Mock* const fence_mock = reinterpret_cast<Mock*>(xs::fence_proxy_real_fence(fence));
    EXPECT(release(queue) == 0 && release(fence) == 0 && queue_mock->refs == 0 && fence_mock->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a refused allocation in queue, fence, shared-fence and device proxy creation and in the four "
                "unwrapped arrays returns E_OUTOFMEMORY, reaches no device and leaks nothing\n", g_label);
    return 0;
}

// ---- 6. Command allocators (device slot 9) ----

int check_allocators() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    // An allocator that answers Leonida's tag (its lists resolve a proxy) comes back wrapped.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, true);
        EXPECT(device);
        const unsigned long device_refs = xs::device_proxy_ref_count(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
        Mock* const real = mock->last_child;
        EXPECT(xs::is_allocator_proxy(allocator) && word(xs::allocator_proxy_real_allocator(allocator)) == word(real));
        EXPECT(mock->args[9][0] == D3D12_COMMAND_LIST_TYPE_DIRECT && real->refs == 1);
        EXPECT(xs::allocator_proxy_slot_count(allocator) == 9 && xs::device_proxy_ref_count(device) == device_refs + 1);
        // Reset (slot 8) reaches the real allocator; slot 9 (SetAllocatorCallbackX) records callback/context and
        // succeeds without touching the real vtable; every slot past 9 up to the table end is refused.
        EXPECT(hr_of(raw(allocator, 8)) == S_OK && real->calls[8] == 1);
        EXPECT(xs::allocator_proxy_callback(allocator) == 0 && xs::allocator_proxy_callback_context(allocator) == 0);
        EXPECT(xs::allocator_proxy_callback(nullptr) == 0 && xs::allocator_proxy_callback_context(device) == 0);
        constexpr std::uintptr_t fake_cb = 0x12345678u;
        constexpr std::uintptr_t fake_ctx = 0x9abcdef0u;
        const std::uint64_t slot9_hits_before = xs::allocator_slot_hits(9);
        EXPECT(hr_of(raw(allocator, 9, fake_cb, fake_ctx, 0, 0xfeedfaceu)) == S_OK && real->calls[9] == 0);
        EXPECT(xs::allocator_slot_hits(9) == slot9_hits_before + 1 && xs::allocator_slot_first_call_seen(9));
        EXPECT(xs::allocator_proxy_callback(allocator) == fake_cb &&
               xs::allocator_proxy_callback_context(allocator) == fake_ctx);
        EXPECT(hr_of(raw(allocator, 9, 0, 0)) == S_OK && real->calls[9] == 0);
        EXPECT(xs::allocator_proxy_callback(allocator) == 0 && xs::allocator_proxy_callback_context(allocator) == 0);
        for (unsigned slot = 10; slot < xs::allocator_proxy_table_size(); ++slot)
            EXPECT(hr_of(raw(allocator, slot)) == E_NOTIMPL && real->calls[slot] == 0);
        // The tag and the real object stay hidden: QueryInterface answers only the modelled interfaces.
        void* found = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(allocator, 0, word(&xs::kLeonidaCommandAllocatorTag), word(&found))) == E_NOINTERFACE &&
               !found);
        EXPECT(hr_of(raw(allocator, 0, word(&allocator_iid), word(&found))) == S_OK && found == allocator);
        EXPECT(release(found) == 1);
        // An allocator is a pageable: MakeResident gives the real runtime the real allocator.
        const std::uint64_t objects[1] = {word(allocator)};
        EXPECT(hr_of(raw(device, 34, 1, word(objects))) == S_OK && mock->array[34][0] == word(real));
        EXPECT(release(allocator) == 0 && real->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        // Null IID, null output, a failing or empty creation, an IID the allocator lacks.
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, 0, word(&out))) == E_POINTER && !out);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), 0)) == S_FALSE);  // the device's own answer
        mock->create_result = E_OUTOFMEMORY;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_OUTOFMEMORY && !out);
        mock->create_result = S_OK;
        mock->create_null = true;
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out))) == E_FAIL && !out);
        mock->create_null = false;
        const IID fence_iid = __uuidof(ID3D12Fence);
        out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        // A refused allocation for the proxy: E_OUTOFMEMORY, and the real allocator is released.
        g_fail_news = 1;
        out = reinterpret_cast<void*>(1);
        const HRESULT starved = hr_of(raw(device, 9, 0, word(&allocator_iid), word(&out)));
        g_fail_news = 0;
        EXPECT(starved == E_OUTOFMEMORY && !out && mock->last_child->refs == 0);
        EXPECT(xs::device_proxy_ref_count(device) == device_refs);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    // An allocator without the tag (a PC runtime allocator, whose lists could not resolve a proxy) comes back as it
    // is, as the interface asked for.
    {
        Mock* mock = nullptr;
        void* device = make_device(mock, 9, true, false);
        EXPECT(device);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        EXPECT(allocator == mock->last_child && !xs::is_allocator_proxy(allocator) && mock->last_child->refs == 1);
        Mock* const real = mock->last_child;
        EXPECT(release(allocator) == 0 && real->refs == 0);
        const IID fence_iid = __uuidof(ID3D12Fence);
        void* out = reinterpret_cast<void*>(1);
        EXPECT(hr_of(raw(device, 9, 0, word(&fence_iid), word(&out))) == E_NOINTERFACE && !out);
        EXPECT(mock->last_child->refs == 0);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    std::printf("%s: allocators answering Leonida's tag are wrapped (Reset forwarded, tail refused, residency "
                "unwrapped, references balanced); others come back unwrapped; failures leave no reference\n", g_label);
    return 0;
}

// ---- 6b. Device slot 133: a resource placed at a VA becomes a committed DEFAULT-heap resource ----
//
// The device stops at ID3D12Device9 (76 slots), so 133 is past its table: the proxy must answer it before the
// beyond-table refusal and reach the PC CreateCommittedResource (slot 27) with a cleaned description.
int check_placed_resource_x() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device && xs::device_slot_overridden(133));
    xs::reset_device_proxy_stats();
    constexpr std::uint64_t kVa = 0x7f0000000000ull;  // any address: the stand-in does not touch it
    // A bit above every PC flag; an integer (a D3D12_RESOURCE_FLAGS value past the last enumerator is not a constant
    // expression for clang-cl), cast with the PC flag at run time.
    constexpr std::uint32_t kXboxFlag = 0x20000;

    // A buffer with an Xbox flag bit, a non-PC layout word and the all-ones state: PC flags only, ROW_MAJOR, COMMON,
    // no clear value, DEFAULT heap, and the caller's IID.
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 1 << 20;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    buffer.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | kXboxFlag);
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {{0, 0, 0, 1}}};
    void* resource = nullptr;
    EXPECT(hr_of(raw6(device, 133, kVa, word(&buffer), 0xFFFFFFFFu, word(&clear), word(&kResourceIid),
                      word(&resource))) == S_OK);
    EXPECT(resource && reinterpret_cast<Mock*>(resource)->kind == Kind::resource);
    EXPECT(mock->calls[27] == 1 && mock->calls[127] == 0 && xs::device_slot_hits(133) == 1);
    EXPECT(g_committed.heap.Type == D3D12_HEAP_TYPE_DEFAULT && g_committed.heap_flags == D3D12_HEAP_FLAG_NONE);
    EXPECT(g_committed.desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    EXPECT(g_committed.desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && g_committed.desc.Width == buffer.Width);
    EXPECT(g_committed.state == D3D12_RESOURCE_STATE_COMMON && !g_committed.clear);
    EXPECT(g_committed.riid == &kResourceIid);
    EXPECT(release(resource) == 0);

    // A render-target texture in a PC state keeps the state and its clear value; the layout becomes UNKNOWN.
    D3D12_RESOURCE_DESC target = buffer;
    target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target.Width = 64;
    target.Height = 32;
    target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | kXboxFlag);
    EXPECT(hr_of(raw6(device, 133, kVa, word(&target), D3D12_RESOURCE_STATE_RENDER_TARGET, word(&clear),
                      word(&kResourceIid), word(&resource))) == S_OK);
    EXPECT(mock->calls[27] == 2 && g_committed.desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    EXPECT(g_committed.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    EXPECT(g_committed.state == D3D12_RESOURCE_STATE_RENDER_TARGET && g_committed.clear == &clear);
    EXPECT(release(resource) == 0);
    // The same texture in the all-ones state is created in COMMON.
    EXPECT(hr_of(raw6(device, 133, kVa, word(&target), 0xFFFFFFFFu, 0, word(&kResourceIid), word(&resource))) ==
           S_OK);
    EXPECT(mock->calls[27] == 3 && g_committed.state == D3D12_RESOURCE_STATE_COMMON);
    EXPECT(release(resource) == 0);

    // Refusals before the device is called; the output is cleared.
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw6(device, 133, kVa, 0, 0, 0, word(&kResourceIid), word(&resource))) == E_INVALIDARG);
    EXPECT(!resource);
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw6(device, 133, kVa, word(&buffer), 0, 0, 0, word(&resource))) == E_POINTER && !resource);
    EXPECT(hr_of(raw6(device, 133, kVa, word(&buffer), 0, 0, word(&kResourceIid), 0)) == E_POINTER);
    EXPECT(mock->calls[27] == 3);

    // A failed creation is returned as it is, with no object.
    mock->create_result = E_OUTOFMEMORY;
    resource = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw6(device, 133, kVa, word(&buffer), 0, 0, word(&kResourceIid), word(&resource))) ==
           E_OUTOFMEMORY);
    EXPECT(!resource && mock->calls[27] == 4);
    mock->create_result = S_OK;

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 133 places a resource as a committed DEFAULT one (PC flags, COMMON for all-ones, "
                "clear value for targets only), past a 76-slot device\n", g_label);
    return 0;
}

// ---- 6c. Slots 76 and 133 when the depth provider refuses a depth-stencil texture with unordered access ----
//
// The provider answers E_INVALIDARG or E_NOTIMPL for a texture it has no unordered-access form of: a console D16S8
// (format 118) target that the Vulkan layer makes as D24S8 on a GPU with D24S8 attachments (NVIDIA). The slots then
// make the texture as they did before the provider: slot 76 a committed texture without unordered access, slot 133
// its stand-in (the title-pages handler, else the committed one). The backing the provider refused is released. Any
// other failure (the provider out of memory, the backing itself, a placement the backend refuses) is passed on as it
// is, with no stand-in.
HRESULT g_provider_result = S_OK;
unsigned g_provider_calls = 0;
Mock* g_provider_backing = nullptr;  // the resource the provider was last given
bool g_placement_valid = true;

HRESULT mock_depth_provider(ID3D12Resource* resource, D3D12_HEAP_FLAGS) {
    ++g_provider_calls;
    g_provider_backing = reinterpret_cast<Mock*>(resource);
    return g_provider_result;
}

bool mock_validate_placement(const D3D12_RESOURCE_DESC&, std::uint32_t, const D3D12_CLEAR_VALUE*) {
    return g_placement_valid;
}

int check_depth_provider_refusal() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device && xs::set_depth_stencil_uav_provider(device, mock_depth_provider, mock_validate_placement));
    constexpr std::uint64_t kVa = 0x7f0000000000ull;  // 64 KiB aligned, as the placement is checked to be
    constexpr auto kDepthStencilUav = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    // The description measured where a real boot stopped: 1280x720, one mip, format 118, depth-stencil with UAV.
    D3D12_RESOURCE_DESC d16s8{};
    d16s8.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d16s8.Width = 1280;
    d16s8.Height = 720;
    d16s8.DepthOrArraySize = 1;
    d16s8.MipLevels = 1;
    d16s8.Format = static_cast<DXGI_FORMAT>(118);
    d16s8.SampleDesc.Count = 1;
    d16s8.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    d16s8.Flags = kDepthStencilUav;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    void* resource = nullptr;
    const std::uint64_t words76[10] = {word(&heap), 0, word(&d16s8), D3D12_RESOURCE_STATE_DEPTH_WRITE, 0,
                                       word(&kResourceIid), word(&resource), 0, 0, 0};
    const auto place = [&] {
        return hr_of(raw6(device, 133, kVa, word(&d16s8), 0xFFFFFFFFu, 0, word(&kResourceIid), word(&resource)));
    };
    const auto arm = [&](HRESULT result) {
        g_provider_result = result;
        g_provider_calls = 0;
        g_provider_backing = nullptr;
        resource = reinterpret_cast<void*>(1);
        return mock->calls[27];
    };

    // The provider accepts: one backing, handed out as the texture (slot 76) or behind its placement (slot 133).
    unsigned before = arm(S_OK);
    EXPECT(hr_of(raw10(device, 76, words76)) == S_OK && resource == g_provider_backing && g_provider_calls == 1);
    EXPECT(mock->calls[27] == before + 1 && !(g_committed.desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    EXPECT(release(resource) == 0);
    before = arm(S_OK);
    EXPECT(place() == S_OK && resource && g_provider_calls == 1 && mock->calls[27] == before + 1);
    EXPECT(release(resource) == 0 && g_provider_backing->refs == 0);

    // The provider refuses: the texture is made without it, and the refused backing is not kept.
    for (const HRESULT refusal : {E_INVALIDARG, E_NOTIMPL}) {
        before = arm(refusal);
        EXPECT(hr_of(raw10(device, 76, words76)) == S_OK && resource && g_provider_calls == 1);
        EXPECT(g_provider_backing && g_provider_backing->refs == 0 && resource != g_provider_backing);
        EXPECT(mock->calls[27] == before + 2 && reinterpret_cast<Mock*>(resource)->kind == Kind::resource);
        EXPECT(g_committed.desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL &&
               g_committed.desc.Format == d16s8.Format && g_committed.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
        EXPECT(g_committed.state == D3D12_RESOURCE_STATE_DEPTH_WRITE);
        EXPECT(release(resource) == 0);

        // Slot 133 reaches its stand-in with the title's description, as before the provider; without the
        // title-pages handler that is the committed DEFAULT one, in COMMON for the all-ones layout.
        before = arm(refusal);
        EXPECT(place() == S_OK && resource && g_provider_calls == 1);
        EXPECT(g_provider_backing && g_provider_backing->refs == 0 && resource != g_provider_backing);
        EXPECT(mock->calls[27] == before + 2 && reinterpret_cast<Mock*>(resource)->kind == Kind::resource);
        EXPECT(g_committed.heap.Type == D3D12_HEAP_TYPE_DEFAULT && g_committed.state == D3D12_RESOURCE_STATE_COMMON);
        EXPECT(g_committed.desc.Format == d16s8.Format && g_committed.desc.Width == d16s8.Width &&
               g_committed.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
        EXPECT(release(resource) == 0);
    }

    // Any other failure of the provider is passed on, with no texture and no stand-in.
    before = arm(E_OUTOFMEMORY);
    EXPECT(hr_of(raw10(device, 76, words76)) == E_OUTOFMEMORY && !resource && g_provider_calls == 1);
    EXPECT(g_provider_backing->refs == 0 && mock->calls[27] == before + 1);
    before = arm(E_OUTOFMEMORY);
    EXPECT(place() == E_OUTOFMEMORY && !resource && g_provider_calls == 1);
    EXPECT(g_provider_backing->refs == 0 && mock->calls[27] == before + 1);

    // So is a failure of the backing itself, which the provider never sees.
    mock->create_result = E_INVALIDARG;
    before = arm(S_OK);
    EXPECT(hr_of(raw10(device, 76, words76)) == E_INVALIDARG && !resource && g_provider_calls == 0);
    EXPECT(mock->calls[27] == before + 1);
    before = arm(S_OK);
    EXPECT(place() == E_INVALIDARG && !resource && g_provider_calls == 0 && mock->calls[27] == before + 1);
    mock->create_result = S_OK;

    // And a placement the backend refuses (E_INVALIDARG too), before any backing is made.
    g_placement_valid = false;
    before = arm(E_INVALIDARG);
    EXPECT(place() == E_INVALIDARG && !resource && g_provider_calls == 0 && mock->calls[27] == before);
    g_placement_valid = true;

    EXPECT(xs::set_depth_stencil_uav_provider(device, nullptr));
    g_provider_result = S_OK;
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: a format 118 depth-stencil texture the depth provider refuses (E_INVALIDARG, E_NOTIMPL) is made "
                "without it by slots 76 and 133; its other failures, the backing's and the placement's are passed "
                "on\n", g_label);
    return 0;
}

// The same stand-in with LEONIDA_SLOT133_LAYOUT_WORD=1 (its own process: the switch is read once): the fourth word is
// a D3D12_BARRIER_LAYOUT. A device without ID3D12Device10 gets the legacy state of that layout through
// CreateCommittedResource; one with it gets CreateCommittedResource3 and the layout as given.
int run_slot133_layout_word() {
    g_label = "xs_proxy_mock --slot133-layout";
    EXPECT(_putenv_s("LEONIDA_SLOT133_LAYOUT_WORD", "1") == 0);
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    constexpr std::uint64_t kVa = 0x7f0000000000ull;
    constexpr std::uint32_t kXboxFlag = 0x20000;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 1 << 20;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    buffer.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | kXboxFlag);
    D3D12_RESOURCE_DESC target = buffer;
    target.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target.Width = 64;
    target.Height = 32;
    target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | kXboxFlag);
    const D3D12_CLEAR_VALUE clear{DXGI_FORMAT_R8G8B8A8_UNORM, {{0, 0, 0, 1}}};
    void* resource = nullptr;

    // A buffer is COMMON whatever word it came with.
    EXPECT(hr_of(raw6(device, 133, kVa, word(&buffer), 2, word(&clear), word(&kResourceIid), word(&resource))) == S_OK);
    EXPECT(resource && mock->calls[27] == 1 && g_committed.state == D3D12_RESOURCE_STATE_COMMON && !g_committed.clear);
    EXPECT(g_committed.desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    EXPECT(release(resource) == 0);
    // RENDER_TARGET (2) is that state, with the clear value.
    EXPECT(hr_of(raw6(device, 133, kVa, word(&target), 2, word(&clear), word(&kResourceIid), word(&resource))) == S_OK);
    EXPECT(mock->calls[27] == 2 && g_committed.state == D3D12_RESOURCE_STATE_RENDER_TARGET && g_committed.clear == &clear);
    EXPECT(g_committed.desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET &&
           g_committed.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    EXPECT(release(resource) == 0);
    // SHADER_RESOURCE (6) is the shader-resource states, not INDEX_BUFFER | RENDER_TARGET.
    EXPECT(hr_of(raw6(device, 133, kVa, word(&target), 6, 0, word(&kResourceIid), word(&resource))) == S_OK);
    EXPECT(mock->calls[27] == 3 && g_committed.state == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
    EXPECT(release(resource) == 0);
    // UNORDERED_ACCESS (3), and the all-ones layout (UNDEFINED) as COMMON.
    EXPECT(hr_of(raw6(device, 133, kVa, word(&target), 3, 0, word(&kResourceIid), word(&resource))) == S_OK);
    EXPECT(mock->calls[27] == 4 && g_committed.state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    EXPECT(release(resource) == 0);
    EXPECT(hr_of(raw6(device, 133, kVa, word(&target), 0xFFFFFFFFu, 0, word(&kResourceIid), word(&resource))) ==
           S_OK);
    EXPECT(mock->calls[27] == 5 && g_committed.state == D3D12_RESOURCE_STATE_COMMON);
    EXPECT(release(resource) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);

#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    // A PC device with ID3D12Device10: CreateCommittedResource3 with the layout as given, never CreateCommittedResource.
    Mock* mock10 = nullptr;
    void* device10 = make_device(mock10, 10, true);
    EXPECT(device10);
    g_committed3 = {};
    EXPECT(hr_of(raw6(device10, 133, kVa, word(&target), 6, word(&clear), word(&kResourceIid), word(&resource))) ==
           S_OK);
    EXPECT(resource && g_committed3.calls == 1 && mock10->calls[27] == 0);
    EXPECT(g_committed3.layout == 6 && g_committed3.clear == &clear && !g_committed3.session &&
           g_committed3.castable_count == 0 && g_committed3.riid == &kResourceIid);
    EXPECT(g_committed3.desc.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET &&
           g_committed3.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && g_committed3.desc.Width == 64);
    EXPECT(release(resource) == 0);
    // A buffer goes in UNDEFINED whatever word it came with, and without a clear value.
    EXPECT(hr_of(raw6(device10, 133, kVa, word(&buffer), 2, word(&clear), word(&kResourceIid), word(&resource))) ==
           S_OK);
    EXPECT(g_committed3.calls == 2 && g_committed3.layout == 0xFFFFFFFFu && !g_committed3.clear);
    EXPECT(release(resource) == 0);
    EXPECT(release(device10) == 0 && mock10->refs == 0);
#endif
    std::printf("%s: with LEONIDA_SLOT133_LAYOUT_WORD=1 the stand-in reads the fourth word as a barrier layout\n",
                g_label);
    return 0;
}

// ---- 7. Xbox device slots from the public HLE (48, 73, 78, 79, 80) and the retail title (41) ----
//
// A device that answers ID3D12Device11 (80 PC slots) is the Vulkan layer's shape: slots 78/79 are inside its table
// (CreateReservedResource2/CreateSampler2 there) and 80 is just past it. None of the five may reach the mock's own
// slot: the mock's call counter proves the route, and the returned token/queue/list proves the handler ran.
int check_xbox_slots() {
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    Mock* mock = nullptr;
    // Full-chain lists: the console IID maps to ID3D12GraphicsCommandList6, which a command_list-kind mock
    // does not answer.
    void* device = make_device(mock, 11, true, true, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 80);
    xs::reset_device_proxy_stats();

    // 78 SetFrameIntervalX(output, interval_us, latency, flags): S_OK, not forwarded to PC slot 78.
    EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK);
    EXPECT(mock->calls[78] == 0 && xs::device_slot_hits(78) == 1);
    // 79 ScheduleFrameEventX(event, offset_us, pEvent, flags): S_OK, not forwarded to PC slot 79.
    EXPECT(hr_of(raw(device, 79, 0, 0, 0x1234, 0)) == S_OK);
    EXPECT(mock->calls[79] == 0 && xs::device_slot_hits(79) == 1);

    // 80 WaitFrameEventX(event, timeout_ms, pEvent, flags, &token): a fresh non-zero token each time; a null token
    // pointer is E_POINTER; the call never blocks (this test would hang otherwise).
    std::uint64_t token = 0, token2 = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0xffffffffu, 0, 0, word(&token))) == S_OK && token != 0);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token2))) == S_OK && token2 != 0 && token2 != token);
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, 0)) == E_POINTER);
    EXPECT(mock->calls[80] == 0 && xs::device_slot_hits(80) == 3);

    // 48 CreateCommandQueueX(desc, riid, out): the queue proxy, created through the PC slot 8 path (filtered desc copy).
    void* queue = nullptr;
    EXPECT(hr_of(raw(device, 48, word(kXboxQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue) && mock->calls[48] == 0 && mock->calls[8] == 1);
    // The PC call gets the description's 16-byte PC prefix, not the console's 32 bytes.
    EXPECT(std::memcmp(&mock->queue_desc, kXboxQueueDesc, sizeof(mock->queue_desc)) == 0);

    // 73 CreateCommandListX(desc, allocator, initialState, riid, out): the type is desc's first word, the allocator
    // proxy is unwrapped, the node mask is 0 and the PC six-argument CreateCommandList (slot 12) is reached.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* const real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));
    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{};
    desc.type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.node_mask = 7;  // ignored by the console call, as the HLE does
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
    // The list is the console list proxy over the PC list the mock made (slot 12's path); the mock's kind depends on
    // full_chain_lists.
    EXPECT(xs::is_list_proxy(list) && word(xs::list_proxy_real_list(list)) == word(mock->last_child));
    EXPECT(mock->last_child->kind == Kind::command_list || mock->last_child->kind == Kind::list);
    EXPECT(mock->calls[73] == 0 && mock->calls[12] == 1 && xs::device_slot_hits(73) == 1);
    EXPECT(mock->args[12][0] == 0 && mock->args[12][1] == D3D12_COMMAND_LIST_TYPE_DIRECT);
    EXPECT(mock->args[12][2] == word(real_allocator) && mock->args[12][3] == 0);
    EXPECT(IsEqualIID(*reinterpret_cast<const IID*>(mock->args[12][4]), list_iid));

    // The measured console list IID (the 1.0.2.1 executable's only GUID of that form) maps to
    // ID3D12GraphicsCommandList6: slot 73 answers it, and a QueryInterface for it on the proxy returns the proxy.
    const IID console_list_iid = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    void* console_list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&console_list_iid),
                     word(&console_list))) == S_OK);
    EXPECT(xs::is_list_proxy(console_list));
    EXPECT(mock->calls[12] == 2);
    void* asked = nullptr;
    EXPECT(hr_of(raw(console_list, 0, word(&console_list_iid), word(&asked))) == S_OK && asked == console_list);
    EXPECT(release(asked) == 1 && release(console_list) == 0);

    // Release the list before the refusal calls below: the COM convention (and the handler) nulls the caller's
    // output on failure, so an invalid call would clear this pointer.
    EXPECT(release(list) == 0);

    // An IID the PC does not know (the console's own list IID; this one is made up for the test) still gets a list:
    // the console list proxy, not a failure from the PC device.
    // {8d2c4f61-3a5e-4b7c-9e10-2f6a8b4c3d51}
    constexpr GUID kConsoleListIid{0x8d2c4f61, 0x3a5e, 0x4b7c, {0x9e, 0x10, 0x2f, 0x6a, 0x8b, 0x4c, 0x3d, 0x51}};
    void* unknown_list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&kConsoleListIid), word(&unknown_list))) ==
           S_OK);
    EXPECT(xs::is_list_proxy(unknown_list) && mock->calls[12] == 3 && mock->calls[73] == 0);
    EXPECT(IsEqualIID(*reinterpret_cast<const IID*>(mock->args[12][4]), list_iid));
    EXPECT(release(unknown_list) == 0);
    // A PC list IID the proxy answers is returned as asked.
    const IID list1_iid = __uuidof(ID3D12GraphicsCommandList1);
    void* list1 = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list1_iid), word(&list1))) == S_OK);
    EXPECT(xs::is_list_proxy(list1) && mock->calls[12] == 4);
    EXPECT(release(list1) == 0);

    // Missing description, allocator or output: E_INVALIDARG/E_POINTER before the real device is called.
    EXPECT(hr_of(raw(device, 73, 0, word(allocator), 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), 0, 0, word(&list_iid), word(&list))) == E_INVALIDARG);
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), 0)) == E_POINTER);

    // 41 CreateCommandSignature: a console argument of type 105 reaches the PC slot 41 as DISPATCH, in a copy of the
    // description (stride, count, the root argument before it and the root signature kept). A PC description is
    // passed as the same pointer.
    D3D12_INDIRECT_ARGUMENT_DESC console_args[2]{};
    console_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    console_args[0].Constant.Num32BitValuesToSet = 1;
    constexpr std::uint32_t kType105 = 105;  // outside the enum's range: written and read as its 32 bits
    std::memcpy(&console_args[1].Type, &kType105, sizeof(kType105));
    const D3D12_COMMAND_SIGNATURE_DESC console_sig{16, 2, console_args, 0};
    const IID sig_iid = __uuidof(ID3D12CommandSignature);
    void* sig = nullptr;
    EXPECT(hr_of(raw(device, 41, word(&console_sig), 0x5150, word(&sig_iid), word(&sig))) == S_OK);
    EXPECT(mock->calls[41] == 1 && xs::device_slot_hits(41) == 1);
    EXPECT(mock->args[41][0] != word(&console_sig) && mock->args[41][1] == 0x5150);
    EXPECT(mock->args[41][2] == word(&sig_iid) && mock->args[41][3] == word(&sig));
    EXPECT(mock->array[41][0] == (16 | std::uint64_t{2} << 32));
    EXPECT(mock->array[41][1] == D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT &&
           mock->array[41][2] == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
    std::uint32_t caller_type = 0;
    std::memcpy(&caller_type, &console_args[1].Type, sizeof(caller_type));
    EXPECT(caller_type == kType105);  // the caller's description is untouched
    D3D12_INDIRECT_ARGUMENT_DESC pc_arg{};
    pc_arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
    const D3D12_COMMAND_SIGNATURE_DESC pc_sig{12, 1, &pc_arg, 0};
    EXPECT(hr_of(raw(device, 41, word(&pc_sig), 0, word(&sig_iid), word(&sig))) == S_OK);
    EXPECT(mock->calls[41] == 2 && mock->args[41][0] == word(&pc_sig));
    EXPECT(mock->array[41][1] == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH);

    // 61 is CreateSamplerX(desc, dest) for the title and the PC CreateMetaCommand for a host caller. With no
    // console-caller check installed (a host program like this one), the call is the PC slot 61.
    D3D12_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    const std::uint64_t sampler_handle = 0x7ff012340040ull;
    raw(device, 61, word(&sampler), sampler_handle);
    EXPECT(mock->calls[61] == 1 && mock->calls[22] == 0 && xs::device_slot_hits(61) == 1);
    // A caller the check calls the title's (here every one): the PC CreateSampler (slot 22) gets the same two words,
    // not the PC slot 61. A null description is skipped without reaching the device.
    xs::set_console_caller([](std::uint64_t) { return true; });
    raw(device, 61, word(&sampler), sampler_handle);
    EXPECT(mock->calls[61] == 1 && mock->calls[22] == 1 && xs::device_slot_hits(61) == 2);
    EXPECT(mock->args[22][0] == word(&sampler) && mock->args[22][1] == sampler_handle);
    raw(device, 61, 0, sampler_handle);
    EXPECT(mock->calls[61] == 1 && mock->calls[22] == 1 && xs::device_slot_hits(61) == 3);
    // A caller outside the title's images is a host one again.
    xs::set_console_caller([](std::uint64_t) { return false; });
    raw(device, 61, word(&sampler), sampler_handle);
    EXPECT(mock->calls[61] == 2 && mock->calls[22] == 1);
    xs::set_console_caller(nullptr);

    EXPECT(xs::device_slot_overridden(41) && xs::device_slot_overridden(48) && xs::device_slot_overridden(61) &&
           xs::device_slot_overridden(73) &&
           xs::device_slot_overridden(78) && xs::device_slot_overridden(79) && xs::device_slot_overridden(80) &&
           xs::device_slot_overridden(86) && xs::device_slot_overridden(101) && xs::device_slot_overridden(108));

    // 86 CreatePipelineState(desc, riid, out) -> PC slot 47 (ID3D12Device2::CreatePipelineState).
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc{};
    const IID pso_iid = __uuidof(ID3D12PipelineState);
    void* pso_stream_out = nullptr;
    EXPECT(hr_of(raw(device, 86, word(&stream_desc), word(&pso_iid), word(&pso_stream_out))) == S_OK);
    EXPECT(mock->calls[86] == 0 && mock->calls[47] == 1 && xs::device_slot_hits(86) == 1);
    EXPECT(mock->args[47][0] == word(&stream_desc) && mock->args[47][1] == word(&pso_iid) &&
           mock->args[47][2] == word(&pso_stream_out));
    pso_stream_out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 86, word(&stream_desc), 0, word(&pso_stream_out))) == E_POINTER && !pso_stream_out);

    // 101 CreateStateObject(desc, riid, out) -> PC slot 62 (ID3D12Device5::CreateStateObject).
    D3D12_STATE_OBJECT_DESC state_obj_desc{};
    const IID so_iid = __uuidof(ID3D12StateObject);
    void* so_out = nullptr;
    EXPECT(hr_of(raw(device, 101, word(&state_obj_desc), word(&so_iid), word(&so_out))) == S_OK);
    EXPECT(mock->calls[101] == 0 && mock->calls[62] == 1 && xs::device_slot_hits(101) == 1);
    EXPECT(mock->args[62][0] == word(&state_obj_desc) && mock->args[62][1] == word(&so_iid) &&
           mock->args[62][2] == word(&so_out));
    so_out = reinterpret_cast<void*>(1);
    EXPECT(hr_of(raw(device, 101, word(&state_obj_desc), 0, word(&so_out))) == E_POINTER && !so_out);

    // 108 GetResourceAllocationInfo2 -> PC slot 68 (ID3D12Device8::GetResourceAllocationInfo2), called with the
    // retail executable's C-ABI struct return (return buffer in RCX, device proxy in RDX), 56-byte D3D12_RESOURCE_DESC
    // entries, and an Xbox layout/flag word, as well as the PC C++ argument order.
    void* const* proxy_vtable = *reinterpret_cast<void* const* const*>(device);
    using CAbiSlot108Fn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(
        D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*,
        D3D12_RESOURCE_ALLOCATION_INFO1*);
    const auto call_slot108_c_abi = reinterpret_cast<CAbiSlot108Fn>(proxy_vtable[108]);
    D3D12_RESOURCE_DESC xbox_descs[2]{};
    xbox_descs[0].Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    xbox_descs[0].Width = 3840;
    xbox_descs[0].Height = 2160;
    xbox_descs[0].DepthOrArraySize = 1;
    xbox_descs[0].MipLevels = 1;
    xbox_descs[0].Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    xbox_descs[0].SampleDesc.Count = 1;
    xbox_descs[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x105);
    xbox_descs[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | 0x300000u);
    xbox_descs[1].Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    xbox_descs[1].Width = 4096;
    xbox_descs[1].Height = 1;
    xbox_descs[1].DepthOrArraySize = 1;
    xbox_descs[1].MipLevels = 1;
    xbox_descs[1].SampleDesc.Count = 1;
    xbox_descs[1].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);
    xbox_descs[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | 0x200000u);
    D3D12_RESOURCE_ALLOCATION_INFO ret_info{};
    D3D12_RESOURCE_ALLOCATION_INFO1 detail_info[2]{};
    EXPECT(call_slot108_c_abi(&ret_info, device, 1, 2, xbox_descs, detail_info) == &ret_info);
    EXPECT(ret_info.SizeInBytes == 0x40000 && ret_info.Alignment == 0x10000);
    EXPECT(detail_info[0].SizeInBytes == 0x40000 && detail_info[0].Alignment == 0x10000);
    EXPECT(mock->calls[108] == 0 && mock->calls[68] == 1 && xs::device_slot_hits(108) == 1);
    EXPECT(mock->array[68][0] == static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_UNKNOWN));
    EXPECT(mock->array[68][1] == static_cast<std::uint64_t>(
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    EXPECT(mock->array[68][2] == 0);
    EXPECT(mock->array[68][3] == ((static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR) << 32) |
                                  static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)));

    D3D12_RESOURCE_DESC xbox_buf_desc = xbox_descs[1];
    xbox_buf_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x200000u);
    D3D12_RESOURCE_ALLOCATION_INFO buf_ret{};
    EXPECT(raw(device, 108, word(&buf_ret), 1, 1, word(&xbox_buf_desc), 0) == word(&buf_ret));
    EXPECT(buf_ret.SizeInBytes == 0x40000 && mock->calls[68] == 2 && xs::device_slot_hits(108) == 2);
    EXPECT(mock->array[68][0] == static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR));
    EXPECT(mock->array[68][1] == static_cast<std::uint64_t>(D3D12_RESOURCE_FLAG_NONE));

    D3D12_RESOURCE_DESC batch1025[1025]{};
    for (int i = 0; i < 1024; ++i) batch1025[i] = xbox_descs[0];
    batch1025[1024] = xbox_descs[1];
    D3D12_RESOURCE_ALLOCATION_INFO batch_ret{};
    EXPECT(call_slot108_c_abi(&batch_ret, device, 1, 1025, batch1025, nullptr) == &batch_ret);
    EXPECT(batch_ret.SizeInBytes == 0x40000 && mock->calls[68] == 3 && xs::device_slot_hits(108) == 3);
    EXPECT(mock->array[68][0] == static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_UNKNOWN));
    EXPECT(mock->array[68][3] == ((static_cast<std::uint64_t>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR) << 32) |
                                  static_cast<std::uint32_t>(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)));

    // A count with a null descriptor array is refused with {UINT64_MAX, 0} and never reaches PC slot 68.
    D3D12_RESOURCE_ALLOCATION_INFO null_ret{};
    EXPECT(call_slot108_c_abi(&null_ret, device, 1, 2, nullptr, nullptr) == &null_ret);
    EXPECT(null_ret.SizeInBytes == UINT64_MAX && null_ret.Alignment == 0);
    EXPECT(mock->calls[68] == 3 && xs::device_slot_hits(108) == 4);

    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slots 48/73/78/79/80 route to a queue, a list and frame-event tokens, and slot 61 to "
                "CreateSampler, without reaching the mock's own PC slots; slot 41 creates argument type 105 as DISPATCH\n",
                g_label);
    return 0;
#else
    std::printf("%s: ID3D12Device11 is not in this SDK; Xbox slot 80's beyond-table case is not built\n", g_label);
    return 0;
#endif
}

// ---- 7b. Console initial state 0x6 on a committed texture (slots 27, 53 and 76) ----
//
// The console creates render-target and depth textures with state 0x6, which the PC runtime refuses for a texture. The
// mock device records the state it was given (slot 27's fourth word, a4 in args[27][3] / args[53][3]).
D3D12_RESOURCE_DESC resource_desc(D3D12_RESOURCE_DIMENSION dimension, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = dimension;
    desc.Width = 64;
    desc.Height = 64;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    return desc;
}

std::uint64_t raw8(const void* object, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                   std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7, std::uint64_t a8 = 0) {
    void* const* vtable = *reinterpret_cast<void* const* const*>(object);
    return reinterpret_cast<SlotFn>(vtable[slot])(const_cast<void*>(object), a1, a2, a3, a4, a5, a6, a7, a8, 0, 0, 0,
                                                  0, 0, 0);
}

int check_committed_resource_states() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 76);
    mock->words_only = true;  // slot 27 has the creating mock entry (slots 76/133 need it); here only the words count
    const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC rt = resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const D3D12_RESOURCE_DESC ds = resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    const D3D12_RESOURCE_DESC plain = resource_desc(D3D12_RESOURCE_DIMENSION_TEXTURE2D, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC buffer = resource_desc(D3D12_RESOURCE_DIMENSION_BUFFER, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    const std::uint64_t kConsole = 0x6;
    const std::uint64_t kRender = D3D12_RESOURCE_STATE_RENDER_TARGET;
    const std::uint64_t kDepth = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    // Slot 27 (the title's CreateCommittedResource) and slot 53 (CreateCommittedResource1): the real device sees
    // the mapped state; the description and the other words arrive as they were.
    // The words after the state (clear value, protected session, riid, out) are distinct, so their positions show.
    for (const unsigned slot : {27u, 53u}) {
        const std::uint64_t before = mock->calls[slot];
        const std::uint64_t clear = 0xC1EA, session = 0x5E55, riid = 0x41D, out = 0x0B7;
        const std::uint64_t first = slot == 27 ? riid : session;
        EXPECT(hr_of(raw8(device, slot, word(&props), 0x40, word(&rt), kConsole, clear, first, slot == 27 ? out : riid,
                          slot == 27 ? 0 : out)) == S_OK);
        EXPECT(mock->args[slot][1] == 0x40 && mock->args[slot][4] == clear);
        EXPECT(mock->tail[slot][0] == first && mock->tail[slot][1] == (slot == 27 ? out : riid));
        EXPECT(slot == 27 || mock->tail[slot][2] == out);
        EXPECT(mock->calls[slot] == before + 1 && state_of(mock->args[slot][3]) == kRender);
        EXPECT(mock->args[slot][0] == word(&props) && mock->args[slot][2] == word(&rt));
        EXPECT(hr_of(raw(device, slot, word(&props), 0, word(&ds), kConsole)) == S_OK);
        EXPECT(state_of(mock->args[slot][3]) == kDepth);
        // Left alone: a buffer with 0x6, a render target with state 0, a texture without a use flag, any other state.
        EXPECT(hr_of(raw(device, slot, word(&props), 0, word(&buffer), kConsole)) == S_OK && state_of(mock->args[slot][3]) == kConsole);
        EXPECT(hr_of(raw(device, slot, word(&props), 0, word(&rt), 0)) == S_OK && state_of(mock->args[slot][3]) == 0);
        EXPECT(hr_of(raw(device, slot, word(&props), 0, word(&plain), kConsole)) == S_OK && state_of(mock->args[slot][3]) == kConsole);
        EXPECT(hr_of(raw(device, slot, word(&props), 0, word(&rt), 0x4)) == S_OK && state_of(mock->args[slot][3]) == 0x4);
    }
    EXPECT(rt.Flags == D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET && ds.Flags == D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

    // Slot 76 (the Xbox resource creation) reaches the PC device's slot 27 with the same mapping.
    const IID resource_iid = __uuidof(ID3D12Resource);
    void* resource = nullptr;
    EXPECT(hr_of(raw8(device, 76, word(&props), 0, word(&rt), kConsole, 0, word(&resource_iid), word(&resource))) == S_OK);
    EXPECT(state_of(mock->args[27][3]) == kRender);
    EXPECT(hr_of(raw8(device, 76, word(&props), 0, word(&ds), kConsole, 0, word(&resource_iid), word(&resource))) == S_OK);
    EXPECT(state_of(mock->args[27][3]) == kDepth);
    EXPECT(hr_of(raw8(device, 76, word(&props), 0, word(&buffer), kConsole, 0, word(&resource_iid), word(&resource))) == S_OK);
    EXPECT(state_of(mock->args[27][3]) == kConsole);

    EXPECT(xs::device_slot_overridden(27) && xs::device_slot_overridden(53));
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: committed textures with state 0x6 reach the PC device as RENDER_TARGET or DEPTH_WRITE; buffers and "
                "other states are unchanged\n", g_label);
    return 0;
}

// ---- 7c. Xbox queue ExecuteCommandLists variant (queue slot 35 -> PC slot 10) ----

int check_xbox_queue_slots() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);  // full-chain list mocks, as check_lists
    EXPECT(device);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    xs::reset_device_proxy_stats();

    EXPECT(xs::queue_slot_overridden(35) && xs::queue_slot_overridden(14) && xs::queue_slot_overridden(15));
    EXPECT(!xs::queue_slot_overridden(10) && !xs::queue_slot_overridden(34) && !xs::queue_slot_overridden(36));

    // Real objects, not numbers: the array is read to replace list proxies by their real lists, as slot 10's is.
    Mock* const list_a = new_mock(Kind::command_list);
    Mock* const list_b = new_mock(Kind::command_list);
    void* lists[2] = {list_a, list_b};
    EXPECT(raw(queue, 35, (0xdeadbeefull << 32) | 2ull, word(lists)) == 0);
    EXPECT(xs::queue_slot_hits(35) == 1 && xs::queue_slot_first_call_seen(35));
    EXPECT(xs::queue_slot_hits(10) == 0);
    EXPECT(queue_mock->calls[35] == 0 && queue_mock->calls[10] == 1);
    EXPECT(queue_mock->args[10][0] == 2);
    EXPECT(queue_mock->array[10][0] == word(list_a) && queue_mock->array[10][1] == word(list_b));
    EXPECT(lists[0] == list_a && lists[1] == list_b);  // the caller's array is not written

    // With the console list proxy (#2490) on this branch, slot 35 hands the real queue the lists the way slot 10 does:
    // a copy of the array with each list proxy replaced by its real list. So the entries are real objects here (the
    // unwrap reads each one's first word), one a list proxy and one a plain object that goes through unchanged, and
    // the mock queue records the array it was given.
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0, word(&gcl_iid),
                     word(&list))) == S_OK);
    Mock* const real_list = mock->last_child;
    EXPECT(xs::is_list_proxy(list) && word(xs::list_proxy_real_list(list)) == word(real_list));
    Mock* const plain = new_mock(Kind::resource);
    void* proxied[2] = {list, plain};
    EXPECT(raw(queue, 35, 2, word(proxied)) == 0);
    EXPECT(xs::queue_slot_hits(35) == 2 && queue_mock->calls[35] == 0 && queue_mock->calls[10] == 2);
    EXPECT(queue_mock->array[10][0] == word(real_list) && queue_mock->array[10][1] == word(plain));
    // PC slot 10 itself is the same path.
    EXPECT(raw(queue, 10, 2, word(proxied)) == 0 && queue_mock->calls[10] == 3);
    EXPECT(queue_mock->array[10][0] == word(real_list) && queue_mock->array[10][1] == word(plain));
    EXPECT(proxied[0] == list && proxied[1] == plain);
    EXPECT(release(list) == 0 && release(allocator) == 0);

    EXPECT(raw(queue, 35, 0, 0) == 0);
    EXPECT(xs::queue_slot_hits(35) == 3 && queue_mock->calls[35] == 0 && queue_mock->calls[10] == 4);
    EXPECT(queue_mock->args[10][0] == 0 && queue_mock->args[10][1] == 0);

    // Queue slot 18. The title's call (one pointer to 8 bytes) is the frequency getter, PC slot 16, and never the
    // PC slot 18 (GetDesc), which would write a 16-byte description through that pointer. A host caller, under a
    // classifier and with none installed, keeps the PC slot 18.
    std::uint64_t frequency[3] = {0xA5A5A5A5A5A5A5A5ull, 0xA5A5A5A5A5A5A5A5ull, 0xA5A5A5A5A5A5A5A5ull};
    xs::set_console_caller([](std::uint64_t) { return true; });
    raw(queue, 18, word(&frequency[1]));
    EXPECT(queue_mock->calls[16] == 1 && queue_mock->calls[18] == 0 && queue_mock->args[16][0] == word(&frequency[1]));
    EXPECT(frequency[0] == 0xA5A5A5A5A5A5A5A5ull && frequency[2] == 0xA5A5A5A5A5A5A5A5ull);
    xs::set_console_caller([](std::uint64_t) { return false; });
    D3D12_COMMAND_QUEUE_DESC host_desc{};
    raw(queue, 18, word(&host_desc));
    EXPECT(queue_mock->calls[16] == 1 && queue_mock->calls[18] == 1 && queue_mock->args[18][0] == word(&host_desc));
    xs::set_console_caller(nullptr);
    raw(queue, 18, word(&host_desc));
    EXPECT(queue_mock->calls[16] == 1 && queue_mock->calls[18] == 2);

    // Queue slots 16 and 17 with one of the title's fences are Signal and Wait, PC slots 14 and 15, with the real
    // fence, and never the PC slots 16 and 17, which write through their first argument: forwarded by number, the
    // frequency went over the fence proxy's first word, its vtable pointer, and the title's next call on that fence
    // went through a null pointer. The fence also marks the newer layout (check_signalx_detection).
    void* fence = make_fence(device);
    EXPECT(fence);
    const auto real_fence = word(xs::fence_proxy_real_fence(fence));
    void* const fence_vtable_before = *reinterpret_cast<void**>(fence);
    const std::uint64_t signal14 = queue_mock->calls[14], wait15 = queue_mock->calls[15];
    const std::uint64_t getter16 = queue_mock->calls[16], getter17 = queue_mock->calls[17];
    EXPECT(hr_of(raw(queue, 16, word(fence), 7)) == S_OK);
    EXPECT(queue_mock->calls[14] == signal14 + 1 && queue_mock->args[14][0] == real_fence &&
           queue_mock->args[14][1] == 7);
    EXPECT(hr_of(raw(queue, 17, word(fence), 9)) == S_OK);
    EXPECT(queue_mock->calls[15] == wait15 + 1 && queue_mock->args[15][0] == real_fence &&
           queue_mock->args[15][1] == 9);
    EXPECT(queue_mock->calls[16] == getter16 && queue_mock->calls[17] == getter17);
    EXPECT(*reinterpret_cast<void**>(fence) == fence_vtable_before);
    EXPECT(release(fence) == 0);

    EXPECT(release(queue) == 0 && release(device) == 0 && mock->refs == 0);
    std::printf("%s: queue slot 35 routes ExecuteCommandLists(count, lists) to PC slot 10 with UINT count masking "
                "and list proxies unwrapped; slots 16 and 17 with a fence are Signal and Wait\n", g_label);
    return 0;
}

// ---- 8. Environment overrides for the Xbox device slots (one process each, read once) ----

struct EnvPresenter {
    int stats = 0, present = 0, suspend = 0, resume = 0;
};

int run_env_slots(bool valid, bool on_slot74 = false, bool on_slot70 = false) {
    const char* frame_interval = on_slot70 ? "70" : on_slot74 ? "74" : valid ? "96" : "2";
    const char* schedule_event = valid ? "97" : "2";
    const char* wait_event = valid ? "98" : "2";
    const char* queue_x = valid ? "99" : "2";
    const char* list_x = valid ? "100" : "2";
    const char* frame_statistics = valid ? "101" : "2";
    const char* suspend_x = valid ? "33" : "2";  // a queue slot beyond the PC table, as SuspendX would be
    const char* resume_x = valid ? "30" : "2";   // equal to PresentX's default: ResumeX is unrouted (logged)
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_INTERVAL_SLOT", frame_interval) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_SCHEDULE_EVENT_SLOT", schedule_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_WAIT_EVENT_SLOT", wait_event) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_FRAME_STATISTICS_SLOT", frame_statistics) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_QUEUE_X_SLOT", queue_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_LIST_X_SLOT", list_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_SUSPEND_X_SLOT", suspend_x) != FALSE);
    EXPECT(SetEnvironmentVariableA("LEONIDA_XS_RESUME_X_SLOT", resume_x) != FALSE);

    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device);
    xs::reset_device_proxy_stats();
    std::uint64_t token = 0;
    if (on_slot70) {
        // SetFrameIntervalX moved onto 70, which also has a built-in handler: the override wins, so (output, interval,
        // latency, flags) is a frame-interval call, never a shader-resource view whose handle is the latency.
        EXPECT(hr_of(raw(device, 70, 0, 16667, 1, 0)) == S_OK && mock->calls[70] == 0 && mock->calls[18] == 0);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);  // the old default is a PC slot now
    } else if (on_slot74) {
        // SetFrameIntervalX moved onto 74, which also has a built-in handler: the override wins, so (output, interval,
        // latency, flags) is a frame-interval call that reaches no PC slot, not the stand-in that ignores its words.
        EXPECT(hr_of(raw(device, 74, 0, 16667, 1, 0)) == S_OK && mock->calls[74] == 0);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);  // the old default is a PC slot now
    } else if (valid) {
        // Every route moved: the new slot is handled, the default slot is forwarded to the mock.
        EXPECT(hr_of(raw(device, 96, 0, 16667, 1, 0)) == S_OK && mock->calls[96] == 0);
        EXPECT(hr_of(raw(device, 97, 0, 0, 0, 0)) == S_OK && mock->calls[97] == 0);
        EXPECT(hr_of(raw(device, 98, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[98] == 0);
        void* queue = nullptr;
        EXPECT(hr_of(raw(device, 99, word(kXboxQueueDesc), word(&kQueueIid), word(&queue))) == S_OK);
        EXPECT(xs::is_queue_proxy(queue) && mock->calls[99] == 0);
        const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
        void* allocator = nullptr;
        EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
        struct {
            std::uint32_t type, node_mask;
        } desc{static_cast<std::uint32_t>(D3D12_COMMAND_LIST_TYPE_DIRECT), 0};
        const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
        void* list = nullptr;
        EXPECT(hr_of(raw(device, 100, word(&desc), word(allocator), 0, word(&list_iid), word(&list))) == S_OK);
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 1);
        EXPECT(release(list) == 0);
        EXPECT(release(allocator) == 0);

        // The presenter answers frame statistics at 101 only (81 is not routed, so the mock refuses it); PresentX
        // stays at 30, SuspendX answers at 33, and ResumeX, asked for 30, is unrouted, so 32 is refused on a device
        // with a presenter attached, without reaching the presenter.
        EnvPresenter env{};
        xs::PresentationHandler h{};
        h.context = &env;
        h.get_frame_statistics = [](void* ctx, std::uint64_t, UINT, UINT*, void*) -> HRESULT {
            ++static_cast<EnvPresenter*>(ctx)->stats;
            return S_FALSE;
        };
        h.present_x = [](void* ctx, UINT, const void*, const void*) -> HRESULT {
            ++static_cast<EnvPresenter*>(ctx)->present;
            return S_OK;
        };
        h.suspend_x = [](void* ctx, UINT) -> HRESULT {
            ++static_cast<EnvPresenter*>(ctx)->suspend;
            return S_OK;
        };
        h.resume_x = [](void* ctx) -> HRESULT {
            ++static_cast<EnvPresenter*>(ctx)->resume;
            return S_OK;
        };
        EXPECT(xs::set_presentation_handler(device, &h));
        UINT count = 0;
        EXPECT(hr_of(raw(device, 101, 77, 0x1a, word(&count), 0)) == S_FALSE && env.stats == 1);
        EXPECT(hr_of(raw(device, 81, 77, 0x1a, word(&count), 0)) == E_NOTIMPL && env.stats == 1);
        EXPECT(hr_of(raw(queue, 30, 1, 0, 0)) == S_OK && env.present == 1);
        EXPECT(hr_of(raw(queue, 33)) == S_OK && env.suspend == 1);
        EXPECT(hr_of(raw(queue, 32)) == E_NOTIMPL && env.resume == 0);
        EXPECT(xs::set_presentation_handler(device, nullptr));
        EXPECT(release(queue) == 0);
    } else {
        // Values outside 3..127 are ignored: the five defaults still answer, and a PC slot that is not a default is
        // forwarded unchanged.
        EXPECT(hr_of(raw(device, 78, 0, 16667, 1, 0)) == S_OK && mock->calls[78] == 0);
        EXPECT(hr_of(raw(device, 79, 0, 0, 0, 0)) == S_OK && mock->calls[79] == 0);
        EXPECT(hr_of(raw(device, 80, 0, 0, 0, 0, word(&token))) == S_OK && token && mock->calls[80] == 0);
        // The default GetFrameStatisticsX slot, unattached: a zero count and S_OK; a null count, or a count with no
        // array, is E_POINTER.
        UINT stats = 0;
        EXPECT(hr_of(raw(device, 81, 0, 0, word(&stats), 0)) == S_OK && stats == 0 && mock->calls[81] == 0);
        EXPECT(hr_of(raw(device, 81, 0, 0, 0, 0)) == E_POINTER);
        stats = 2;
        EXPECT(hr_of(raw(device, 81, 0, 0, word(&stats), 0)) == E_POINTER && stats == 2);
        std::uint8_t stats_out[2 * 0x38] = {};
        EXPECT(hr_of(raw(device, 81, 0, 0, word(&stats), word(stats_out))) == S_OK && stats == 0 &&
               mock->calls[81] == 0);
        // Slot 71 is the Xbox placed-UAV view of the title's own memory (d19339b07), not a PC slot: the proxy
        // handles it before the PC vtable, so the PC slot 71 (CreateSamplerFeedbackUnorderedAccessView) is never
        // reached and the mock records nothing.
        EXPECT(hr_of(raw(device, 71)) == S_OK && mock->calls[71] == 0 && xs::device_slot_hits(71) == 1);
        // A PC slot that is neither an Xbox route nor one of the defaults is still forwarded unchanged.
        EXPECT(hr_of(raw(device, 72)) == S_OK && mock->calls[72] == 1);  // not an Xbox slot: forwarded
    }
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: environment slot overrides %s\n", g_label,
                valid ? "move all five routes" : "outside 3..127 are ignored");
    return 0;
}

// ---- 9. Command lists (device slot 12): the console-shaped proxy and AllocateUploadMemory ----

int check_lists() {
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);
    EXPECT(device);
    const unsigned long device_refs = xs::device_proxy_ref_count(device);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    Mock* const real_allocator = mock->last_child;
    // CreateCommandList(node_mask, type, allocator, pso, iid, out): the allocator proxy is unwrapped on the way
    // in and the list comes back console-shaped.
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0, word(&gcl_iid),
                     word(&list))) == S_OK);
    Mock* const real = mock->last_child;
    EXPECT(real->kind == Kind::list && mock->args[12][2] == word(real_allocator));
    EXPECT(xs::is_list_proxy(list) && word(xs::list_proxy_real_list(list)) == word(real));
    EXPECT(xs::device_proxy_ref_count(device) == device_refs + 2);  // allocator + list
    // The console object body sits at +0x18 with the write cursor pinned at the record limits.
    const auto* body = reinterpret_cast<const std::uint64_t*>(word(list) + 0x18);
    // Object offsets +0x18/+0x28/+0x30 are body[0]/body[2]/body[3]: the cursor is pinned at both record limits.
    EXPECT(body[0] == body[2] && body[0] == body[3]);
    // GetDevice answers with the Xbox-ABI device.
    void* found = nullptr;
    const IID device_iid = __uuidof(ID3D12Device);
    EXPECT(hr_of(raw(list, 7, word(&device_iid), word(&found))) == S_OK && found == device);
    EXPECT(release(found) >= 1);
    // A forwarded console slot: Dispatch is console 14 (PC slot 14 too — order holds through 26).
    EXPECT(hr_of(raw(list, 14, 1, 1, 1)) == S_OK && real->calls[14] == 1);
    // Console slot 139 is the retail title's Reset(allocator, pso) (Pipodi measured it on 1.0.2.1): the allocator
    // proxy unwraps and the call reaches the real list's PC slot 10.
    EXPECT(hr_of(raw(list, 139, word(allocator), 0)) == S_OK && real->calls[10] == 1 &&
           real->args[10][0] == word(real_allocator));
    // Console-only extras (27-30) and the unmeasured tail are recorded and return 0, never forwarded.
    EXPECT(raw(list, 27, 1, 2, 3, 4) == 0 && real->calls[27] == 0);
    EXPECT(raw(list, 148, 0, 16) == 0);  // AllocateUploadMemory(0, ...) refuses
    // AllocateUploadMemory carves from the upload arena; a later root SRV gets a real address for it.
    const std::uint64_t p1 = raw(list, 148, 4096, 64);
    const std::uint64_t p2 = raw(list, 148, 16, 16);
    EXPECT(p1 && p2 > p1 && (p1 & 63) == 0 && p2 >= p1 + 4096);
    // A size that wraps the arena arithmetic is refused and moves nothing: UINT64_MAX used to return a
    // pointer and corrupt the cursor so the next request re-handed the same range (daemosofchaos's
    // report on #2490). A non-power-of-two alignment is unmeasured and refused rather than guessed.
    EXPECT(raw(list, 148, ~0ull, 16) == 0);
    EXPECT(raw(list, 148, 16, 48) == 0);
    const std::uint64_t p3 = raw(list, 148, 16, 16);
    EXPECT(p3 && p3 == p2 + 16);  // fresh memory right after p2 — both refusals left the cursor alone
    // Console 43 -> PC 39 (SetComputeRootShaderResourceView): the arena pointer crosses as a GPU VA (the mock
    // device cannot map a real buffer, so the fallback arena translates to the same address).
    EXPECT(hr_of(raw(list, 43, 0, p1)) == S_OK && real->calls[39] == 1 && real->args[39][1] == p1);
    // The view-struct paths (IASetIndexBuffer console 47 -> PC 43, IASetVertexBuffers console 48 -> PC 44) reach
    // the real list; the arena VAs inside the views are rewritten in the proxy's copies.
    D3D12_INDEX_BUFFER_VIEW ib_view{};
    ib_view.BufferLocation = p1;
    EXPECT(hr_of(raw(list, 47, word(&ib_view))) == S_OK && real->calls[43] == 1);
    D3D12_VERTEX_BUFFER_VIEW vb_views[2]{};
    vb_views[0].BufferLocation = p1;
    vb_views[1].BufferLocation = 0xDEAD0000;  // a VA outside the arena passes through
    EXPECT(hr_of(raw(list, 48, 0, 2, word(vb_views))) == S_OK && real->calls[44] == 1);
    D3D12_STREAM_OUTPUT_BUFFER_VIEW so_view{};
    so_view.BufferLocation = p1;
    so_view.BufferFilledSizeLocation = p2;
    EXPECT(hr_of(raw(list, 49, 0, 1, word(&so_view))) == S_OK && real->calls[45] == 1);
    // The console tail past the PC chain is refused with E_NOTIMPL only when the mapped PC slot is missing.
    // Enhanced Barrier -> GCL7 80; an empty call (no groups) goes through as the PC no-op it is.
    EXPECT(hr_of(raw(list, 146, 0, 0)) == S_OK && real->calls[80] == 1);
    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(xs::device_proxy_ref_count(device) == device_refs);
    EXPECT(release(device) == 0 && mock->refs == 0);

    // A PC caller (console_lists=false — the default, what the GPU harnesses use) gets the real list back:
    // slot 12 returns the mock, not a proxy, so its PC vtable indices stay valid.
    Mock* pc_mock = new_mock(Kind::device);
    pc_mock->device_level = 9;
    pc_mock->refs = 1;
    void* pc_device = nullptr;
    EXPECT(SUCCEEDED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(pc_mock), __uuidof(ID3D12Device),
                                           &pc_device)));
    void* pc_list = nullptr;
    EXPECT(hr_of(raw(pc_device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&gcl_iid),
                     word(&pc_list))) == S_OK);
    EXPECT(pc_list && !xs::is_list_proxy(pc_list) &&
           reinterpret_cast<Mock*>(pc_list)->kind == Kind::command_list);
    EXPECT(release(pc_list) == 0);
    EXPECT(release(pc_device) == 0 && pc_mock->refs == 0);
    std::printf("%s: lists come back console-shaped (cursor pinned, GetDevice, Dispatch forwarded, extras "
                "recorded); AllocateUploadMemory hands out arena pointers a root SRV receives\n", g_label);
    return 0;
}

// ---- 10. SignalX layout detection ------------------------------------------------------------
// A fence at queue slot 25 while the layout is still unknown is itself the newer-layout evidence: a title that
// signals only through SignalX never reaches Signal/Wait at 16/17, so an unanswered SignalX used to return
// S_OK having signalled nothing — a waiter on that fence value then hangs (shdann's report on #2490: the first
// SignalX gave hr=0 calls=0, the value was lost).

int check_signalx_detection() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    const auto real_fence = word(xs::fence_proxy_real_fence(fence));
    // Undetected layout, fence at slot 25: forwarded as Signal and pins the device to the newer layout.
    EXPECT(hr_of(raw(queue, 25, word(fence), 7)) == S_OK && queue_mock->calls[14] == 1 &&
           queue_mock->args[14][0] == real_fence && queue_mock->args[14][1] == 7);
    // Now proven newer: Signal at 16 and SignalX at 25 keep forwarding.
    EXPECT(hr_of(raw(queue, 16, word(fence), 8)) == S_OK && queue_mock->calls[14] == 2 &&
           queue_mock->args[14][1] == 8);
    EXPECT(hr_of(raw(queue, 25, word(fence), 9)) == S_OK && queue_mock->calls[14] == 3 &&
           queue_mock->args[14][1] == 9);
    // A second queue on the same device inherits the shared detection.
    void* queue2 = make_queue(device);
    EXPECT(queue2);
    Mock* const queue2_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue2));
    EXPECT(hr_of(raw(queue2, 25, word(fence), 11)) == S_OK && queue2_mock->calls[14] == 1 &&
           queue2_mock->args[14][1] == 11);
    EXPECT(release(queue2) == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);

    // Proven older stays older: a non-fence at slot 16 pins the older layout (the frequency getter), and a
    // fence at 25 afterwards is an unmodelled console method — S_OK with no forward, not a second detection.
    Mock* mock2 = nullptr;
    void* device2 = make_device(mock2, 9, true);
    void* queue3 = make_queue(device2);
    void* fence3 = make_fence(device2);
    EXPECT(device2 && queue3 && fence3);
    Mock* const queue3_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue3));
    EXPECT(hr_of(raw(queue3, 16, 0, 0)) == S_OK && queue3_mock->calls[16] == 1 &&
           queue3_mock->calls[14] == 0);
    EXPECT(hr_of(raw(queue3, 25, word(fence3), 13)) == S_OK && queue3_mock->calls[14] == 0);
    // The older SignalX at slot 23 still forwards under the pinned-older layout.
    EXPECT(hr_of(raw(queue3, 23, word(fence3), 15)) == S_OK && queue3_mock->calls[14] == 1 &&
           queue3_mock->args[14][1] == 15);
    EXPECT(release(queue3) == 0 && release(fence3) == 0 && release(device2) == 0 && mock2->refs == 0);

    // A fence at the older SignalX slot while undetected forwards but does NOT pin: a newer title could reach
    // an unmodelled slot-23 method with a fence argument, and a premature older pin would drop its real
    // SignalX at 25.
    Mock* mock3 = nullptr;
    void* device3 = make_device(mock3, 9, true);
    void* queue4 = make_queue(device3);
    void* fence4 = make_fence(device3);
    EXPECT(device3 && queue4 && fence4);
    Mock* const queue4_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue4));
    EXPECT(hr_of(raw(queue4, 23, word(fence4), 17)) == S_OK && queue4_mock->calls[14] == 1 &&
           queue4_mock->args[14][1] == 17);
    EXPECT(hr_of(raw(queue4, 25, word(fence4), 19)) == S_OK && queue4_mock->calls[14] == 2 &&
           queue4_mock->args[14][1] == 19);
    EXPECT(release(queue4) == 0 && release(fence4) == 0 && release(device3) == 0 && mock3->refs == 0);

    // Ambiguous slot 18 on an undetected queue: the older GetDesc or the newer frequency getter. The title's
    // call is served as the frequency query — a desc write is far wider than the 8 bytes a frequency caller
    // passes (a title often reads the frequency before its first fence call); once a fence proves the
    // layout the same call keeps reaching the getter through the shifted path.
    Mock* mock4 = nullptr;
    void* device4 = make_device(mock4, 9, true);
    void* queue5 = make_queue(device4);
    void* fence5 = make_fence(device4);
    EXPECT(device4 && queue5 && fence5);
    Mock* const queue5_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue5));
    xs::set_console_caller([](std::uint64_t) { return true; });
    UINT64 hz = 0;
    EXPECT(hr_of(raw(queue5, 18, word(&hz))) == S_OK && hz == 1000000 &&
           queue5_mock->calls[16] == 1 && queue5_mock->calls[18] == 0);
    hz = 0;
    EXPECT(hr_of(raw(queue5, 16, word(fence5), 8)) == S_OK);
    EXPECT(hr_of(raw(queue5, 18, word(&hz))) == S_OK && hz == 1000000 &&
           queue5_mock->calls[16] == 2 && queue5_mock->calls[18] == 0);
    xs::set_console_caller(nullptr);
    EXPECT(release(queue5) == 0 && release(fence5) == 0 && release(device4) == 0 && mock4->refs == 0);
    std::printf("xs_proxy_mock: SignalX on an undetected queue's slot 25 forwards as Signal and marks the "
                "newer layout; proven-older stays older, a fence at 23 does not pin, and an undetected "
                "slot 18 answers the frequency getter\n");
    return 0;
}

// ---- 8b. Device slot 26, GetCustomHeapProperties, the console's way and the PC's ----

int check_custom_heap_properties() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device && xs::device_slot_overridden(26));
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);

    // The retail executable's call: the buffer for the struct in RCX, the device in RDX, node mask and heap type in
    // R8 and R9 (their upper halves are not the callee's to read). The buffer sits inside a pattern that must stay.
    struct Padded {
        std::uint8_t before[16];
        D3D12_HEAP_PROPERTIES value;
        std::uint8_t after[64];
    } padded;
    std::memset(&padded, 0xA5, sizeof(padded));
    using ConsoleFn = std::uint64_t(WINAPI*)(void*, void*, std::uint64_t, std::uint64_t);
    const std::uint64_t returned = reinterpret_cast<ConsoleFn>(vtable[26])(
        &padded.value, device, 0xdeadbeef00000001ull, 0xdeadbeef00000002ull);
    EXPECT(returned == word(&padded.value));
    // The PC method got (buffer, node mask, heap type) once, on the device behind the proxy.
    EXPECT(mock->calls[26] == 1);
    EXPECT(static_cast<std::uint32_t>(mock->args[26][1]) == 1 && static_cast<std::uint32_t>(mock->args[26][2]) == 2);
    EXPECT(padded.value.Type == D3D12_HEAP_TYPE_UPLOAD && padded.value.CreationNodeMask == 1 &&
           padded.value.VisibleNodeMask == kHeapMarker);
    for (const std::uint8_t byte : padded.before) EXPECT(byte == 0xA5);
    for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
    // The proxy is what it was: main's forwarder took the buffer for the proxy, and a thunk that took the proxy
    // for the buffer would write the struct over it.
    EXPECT(xs::is_device_proxy(device) && word(xs::device_proxy_real_device(device)) == word(mock) &&
           xs::device_proxy_slot_count(device) == 44 && xs::device_proxy_ref_count(device) == 1);

    // A PC C++ caller: the proxy in RCX and its buffer in RDX. That call is forwarded as it is.
    D3D12_HEAP_PROPERTIES pc{};
    EXPECT(raw(device, 26, word(&pc), 1, 3) == word(&pc));
    EXPECT(mock->calls[26] == 2 && mock->args[26][0] == word(&pc));
    EXPECT(pc.Type == D3D12_HEAP_TYPE_READBACK && pc.CreationNodeMask == 1 && pc.VisibleNodeMask == kHeapMarker);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 26 with the buffer in RCX and the device in RDX reaches the PC method and writes "
                "only the struct; the PC form is forwarded unchanged\n", g_label);
    return 0;
}

// ---- 8c. Device slot 25, GetResourceAllocationInfo, in the console's form and the PC's ----

int check_resource_allocation_info() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device && xs::device_slot_overridden(25));
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    D3D12_RESOURCE_DESC descs[2] = {};  // two one-byte buffers: the mock sizes each as one 64 KiB unit
    for (D3D12_RESOURCE_DESC& d : descs) {
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 1;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    }

    // The console form: the buffer for the struct in RCX, the device in RDX, then the mask, the count and the
    // descriptions. The buffer sits inside a pattern that must stay.
    struct Padded {
        std::uint8_t before[16];
        D3D12_RESOURCE_ALLOCATION_INFO value;
        std::uint8_t after[64];
    } padded;
    std::memset(&padded, 0xA5, sizeof(padded));
    using ConsoleFn = std::uint64_t(WINAPI*)(void*, void*, std::uint64_t, std::uint64_t, std::uint64_t);
    const std::uint64_t returned =
        reinterpret_cast<ConsoleFn>(vtable[25])(&padded.value, device, 1, 2, word(descs));
    EXPECT(returned == word(&padded.value));
    // The PC method got (a return buffer, mask, count, descriptions) once, on the device behind the proxy.
    EXPECT(mock->calls[25] == 1 && mock->args[25][1] == 1 && mock->args[25][2] == 2);
    EXPECT(padded.value.SizeInBytes == 2 * 65536 && padded.value.Alignment == 65536);
    for (const std::uint8_t byte : padded.before) EXPECT(byte == 0xA5);
    for (const std::uint8_t byte : padded.after) EXPECT(byte == 0xA5);
    EXPECT(xs::is_device_proxy(device) && word(xs::device_proxy_real_device(device)) == word(mock));

    // A PC C++ caller: the proxy in RCX and its buffer in RDX.
    D3D12_RESOURCE_ALLOCATION_INFO pc{};
    EXPECT(raw(device, 25, word(&pc), 1, 1, word(descs)) == word(&pc));
    EXPECT(mock->calls[25] == 2 && mock->args[25][2] == 1);
    EXPECT(pc.SizeInBytes == 65536 && pc.Alignment == 65536);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 25 with the buffer in RCX and the device in RDX reaches the PC method and writes "
                "only the struct; the PC form reaches it too\n", g_label);
    return 0;
}

// ---- 8. Device slot 70: a view from raw words, never the PC CreatePlacedResource1 ----

// What the layer's resource provider knows: one buffer at [kBufferStart, kBufferStart + kBufferSize).
Mock* g_buffer = nullptr;
constexpr std::uint64_t kBufferStart = 0x10000, kBufferSize = 0x400;

std::uint64_t g_asked_bytes = 0;  // the byte count the proxy last asked the provider to cover

ID3D12Resource* find_buffer(ID3D12Device*, std::uint64_t va, std::uint64_t bytes, std::uint64_t* start,
                            std::uint64_t* size) {
    g_asked_bytes = bytes;
    if (!g_buffer || va < kBufferStart || va >= kBufferStart + kBufferSize) return nullptr;
    *start = kBufferStart;
    *size = kBufferSize;
    ++g_buffer->refs;  // the provider hands out a reference; the proxy releases it
    return reinterpret_cast<ID3D12Resource*>(g_buffer);
}

// The measured retail shape: a desktop buffer description, then a raw R32_TYPELESS view description with the VA right
// after it.
struct RawShape {
    D3D12_RESOURCE_DESC resource;
    struct {
        D3D12_SHADER_RESOURCE_VIEW_DESC view;
        D3D12_GPU_VIRTUAL_ADDRESS va;
    } view;
};

RawShape raw_shape(std::uint64_t va, std::uint64_t width) {
    RawShape shape{};
    shape.resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    shape.resource.Width = width;
    shape.resource.Height = 1;
    shape.resource.DepthOrArraySize = 1;
    shape.resource.MipLevels = 1;
    shape.resource.SampleDesc.Count = 1;
    shape.resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    shape.view.view.Format = DXGI_FORMAT_R32_TYPELESS;
    shape.view.view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    shape.view.view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    shape.view.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    shape.view.va = va;
    return shape;
}

bool is_null_view(const Mock* mock) {
    return mock->args[18][0] == 0 && mock->srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
           mock->srv.Format == DXGI_FORMAT_R8G8B8A8_UNORM && mock->srv.Texture2D.MipLevels == 1 &&
           mock->srv.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
}

int check_slot70() {
    // Level 9 has the PC slot 70; level 0 does not (44 PC slots). Neither may reach CreatePlacedResource1.
    for (const int level : {9, 0}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, level, true);
        EXPECT(device && xs::device_slot_overridden(70));
        xs::reset_device_proxy_stats();
        const std::uint32_t words[8] = {0x1000, 0, 0, 0, 0, 0, 0, 0};
        const std::uint32_t view[8] = {};
        const std::uint64_t destination = 0x7000;
        raw(device, 70, word(words), word(view), destination);
        EXPECT(mock->calls[70] == 0 && xs::device_slot_hits(70) == 1);
        // The destination gets the PC null view: CreateShaderResourceView (slot 18) with no resource, a desc, and the
        // destination handle unchanged.
        EXPECT(mock->calls[18] == 1 && mock->args[18][0] == 0 && mock->args[18][1] != 0 &&
               mock->args[18][2] == destination);
        // No destination, no write.
        raw(device, 70, word(words), word(view), 0);
        EXPECT(mock->calls[70] == 0 && mock->calls[18] == 1);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    return 0;
}

// ---- 8b. Device slot 74: (pointer, u32, pointer) succeeds and touches nothing, never the PC ShaderCacheControl ----

int check_slot74() {
    // Level 9 has the PC slot 74 (ShaderCacheControl); level 0 does not (44 PC slots). Neither may reach it.
    for (const int level : {9, 0}) {
        Mock* mock = nullptr;
        void* device = make_device(mock, level, true);
        EXPECT(device && xs::device_slot_overridden(74));
        xs::reset_device_proxy_stats();
        std::uint32_t first[4] = {0x11111111, 0x22222222, 0x33333333, 0x44444444};
        std::uint32_t second[4] = {0x55555555, 0x66666666, 0x77777777, 0x88888888};
        const std::uint32_t first_before[4] = {first[0], first[1], first[2], first[3]};
        const std::uint32_t second_before[4] = {second[0], second[1], second[2], second[3]};
        // The title's call (a caller the classifier takes for a guest image): the stand-in.
        xs::set_console_caller([](std::uint64_t) { return true; });
        EXPECT(hr_of(raw(device, 74, word(first), 7, word(second))) == S_OK);
        EXPECT(mock->calls[74] == 0 && xs::device_slot_hits(74) == 1);
        EXPECT(std::memcmp(first, first_before, sizeof(first)) == 0 && std::memcmp(second, second_before, sizeof(second)) == 0);
        // A host caller, under a classifier and with none installed: the PC slot 74 (ShaderCacheControl) with the
        // same words where the PC device has it, and the refusal past its table where it does not (found on Windows
        // in the review of this change: the stand-in used to take a host's ShaderCacheControl too).
        for (const bool installed : {true, false}) {
            if (installed)
                xs::set_console_caller([](std::uint64_t) { return false; });
            else
                xs::set_console_caller(nullptr);
            const std::uint64_t before = mock->calls[74];
            const HRESULT hr = hr_of(raw(device, 74, 0x1, 0x2));
            if (level >= 9) {
                EXPECT(hr == S_OK && mock->calls[74] == before + 1);
                EXPECT(mock->args[74][0] == 0x1 && mock->args[74][1] == 0x2);
            } else {
                EXPECT(hr == E_NOTIMPL && mock->calls[74] == before);
            }
        }
        xs::set_console_caller(nullptr);
        EXPECT(release(device) == 0 && mock->refs == 0);
    }
    return 0;
}

// A raw buffer view of a registered VA: the view is of that resource, from (va - start) / 4, as wide as the
// description says. Every other shape, and a VA no buffer holds, still gets the null view.
int check_slot70_raw_buffer() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    g_buffer = new_mock(Kind::resource);
    g_buffer->refs = 1;
    EXPECT(xs::set_device_resource_provider(device, &find_buffer));
    const std::uint64_t destination = 0x7000;

    // 0x100 bytes at +0x40 of the buffer.
    RawShape shape = raw_shape(kBufferStart + 0x40, 0x100);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[70] == 0 && mock->calls[18] == 1 && mock->args[18][0] == word(g_buffer) &&
           mock->args[18][2] == destination);
    EXPECT(mock->srv.Format == DXGI_FORMAT_R32_TYPELESS && mock->srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
           (mock->srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) && mock->srv.Buffer.FirstElement == 0x10 &&
           mock->srv.Buffer.NumElements == 0x40 && mock->srv.Buffer.StructureByteStride == 0 &&
           mock->srv.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
    EXPECT(g_buffer->refs == 1);  // the proxy released the reference the provider added
    EXPECT(g_asked_bytes == 0x100);  // it asks for a buffer that holds the whole width, so an alias that does wins

    // A width that runs past the buffer is cut at its end: (0x400 - 0x3f0) / 4 elements.
    shape = raw_shape(kBufferStart + 0x3f0, 0x1000);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 2 && mock->args[18][0] == word(g_buffer) && mock->srv.Buffer.FirstElement == 0xfc &&
           mock->srv.Buffer.NumElements == 4);

    // A VA no buffer holds: the null view, and nothing is left referenced.
    shape = raw_shape(kBufferStart + kBufferSize + 0x40, 0x100);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 3 && is_null_view(mock) && g_buffer->refs == 1);

    // A known VA but not a raw R32_TYPELESS buffer view (a typed view; a texture description): the null view.
    shape = raw_shape(kBufferStart + 0x40, 0x100);
    shape.view.view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 4 && is_null_view(mock));
    shape = raw_shape(kBufferStart + 0x40, 0x100);
    shape.resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 5 && is_null_view(mock) && g_buffer->refs == 1);

    // No provider: the null view as well.
    EXPECT(xs::set_device_resource_provider(device, nullptr));
    shape = raw_shape(kBufferStart + 0x40, 0x100);
    raw(device, 70, word(&shape.resource), word(&shape.view), destination);
    EXPECT(mock->calls[18] == 6 && is_null_view(mock) && mock->calls[70] == 0);

    g_buffer = nullptr;
    EXPECT(release(device) == 0 && mock->refs == 0);
    return 0;
}

// ---- 7. Device slot 25, GetResourceAllocationInfo: a render target takes whole console tiles ----

D3D12_RESOURCE_DESC target_desc(std::uint64_t width, UINT height, DXGI_FORMAT format, UINT slices = 1) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = static_cast<UINT16>(slices);
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x101);  // the Xbox tiled layout word
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}

// ---- Device slot 57, the native graphics pipeline state: the factory first, the PC description as the fallback ----

HRESULT g_factory_result = E_NOTIMPL;
unsigned g_factory_calls = 0;
ID3D12Device* g_factory_device = nullptr;
const void* g_factory_root = nullptr;
void* const kFactoryPipeline = reinterpret_cast<void*>(0x5757);

HRESULT native_factory(ID3D12Device* real, const leonida::native_pso::GraphicsDesc& desc, REFIID, void** out) {
    ++g_factory_calls;
    g_factory_device = real;
    g_factory_root = desc.root_signature;
    if (SUCCEEDED(g_factory_result)) *out = kFactoryPipeline;
    return g_factory_result;
}

// A description with no stage and no state blob: the fallback builds its PC description from defaults and hands it
// to the device's CreateGraphicsPipelineState (PC slot 10), which the mock records.
int check_slot57_native_graphics() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    const IID pipeline_iid = __uuidof(ID3D12PipelineState);
    leonida::native_pso::GraphicsDesc desc{};
    desc.root_signature = reinterpret_cast<const void*>(0x4004);
    void* out = reinterpret_cast<void*>(0x1111);

    // No factory: the PC description path, as before there was one.
    EXPECT(hr_of(raw(device, 57, word(&desc), word(&pipeline_iid), word(&out))) == S_OK);
    EXPECT(mock->calls[10] == 1 && mock->args[10][2] == word(&out) && mock->calls[57] == 0 && out == nullptr);

    // A factory that cannot build the description (no catalogue, a stage it does not know) answers E_NOTIMPL: it was
    // asked once, with the device behind the proxy and the description, and the PC description path then runs.
    xs::set_device_native_graphics_factory(device, &native_factory);
    g_factory_result = E_NOTIMPL;
    out = reinterpret_cast<void*>(0x1111);
    EXPECT(hr_of(raw(device, 57, word(&desc), word(&pipeline_iid), word(&out))) == S_OK);
    EXPECT(g_factory_calls == 1 && g_factory_device == reinterpret_cast<ID3D12Device*>(mock) &&
           g_factory_root == desc.root_signature);
    EXPECT(mock->calls[10] == 2 && mock->args[10][2] == word(&out) && out == nullptr);

    // A factory that builds it: its pipeline is the answer and the PC description path is not asked.
    g_factory_result = S_OK;
    out = nullptr;
    EXPECT(hr_of(raw(device, 57, word(&desc), word(&pipeline_iid), word(&out))) == S_OK);
    EXPECT(g_factory_calls == 2 && out == kFactoryPipeline && mock->calls[10] == 2);

    // A refusal other than E_NOTIMPL is the title's answer; nothing falls back.
    for (const HRESULT refusal : {E_INVALIDARG, E_OUTOFMEMORY, E_FAIL}) {
        const unsigned asked = g_factory_calls;
        g_factory_result = refusal;
        out = reinterpret_cast<void*>(0x1111);
        EXPECT(hr_of(raw(device, 57, word(&desc), word(&pipeline_iid), word(&out))) == refusal);
        EXPECT(g_factory_calls == asked + 1 && out == nullptr && mock->calls[10] == 2);
    }

    // A description the proxy cannot read is refused before the factory sees it, and does not fall back either.
    g_factory_result = S_OK;
    void* reserved = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    EXPECT(reserved);
    const unsigned asked = g_factory_calls;
    out = reinterpret_cast<void*>(0x1111);
    EXPECT(hr_of(raw(device, 57, word(reserved), word(&pipeline_iid), word(&out))) == E_INVALIDARG);
    EXPECT(g_factory_calls == asked && out == nullptr && mock->calls[10] == 2);
    VirtualFree(reserved, 0, MEM_RELEASE);

    // The factory removed again: back to the PC description path.
    xs::set_device_native_graphics_factory(device, nullptr);
    out = reinterpret_cast<void*>(0x1111);
    EXPECT(hr_of(raw(device, 57, word(&desc), word(&pipeline_iid), word(&out))) == S_OK);
    EXPECT(g_factory_calls == asked && mock->calls[10] == 3 && mock->calls[57] == 0);

    EXPECT(release(device) == 0 && mock->refs == 0);
    return 0;
}

int check_render_target_sizes() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    // The retail executable's call: the buffer for the struct in RCX, the device in RDX.
    using ConsoleFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                               const D3D12_RESOURCE_DESC*);
    const auto get_info = reinterpret_cast<ConsoleFn>(vtable[25]);
    auto ask = [&](const D3D12_RESOURCE_DESC* descs, UINT count) {
        D3D12_RESOURCE_ALLOCATION_INFO info{0xDEAD, 0xBEEF};
        const bool same_buffer = get_info(&info, device, 0, count, descs) == &info;
        if (!same_buffer) info.SizeInBytes = 0;
        return info;
    };

    // A 1920 x 1080 RGBA8 target. A 4-byte element tile is 128 x 128 pixels: 15 tiles across, 9 down (1080 / 128 is
    // 8.4), 135 x 64 KiB = 8,847,360 B. The PC device packs it as 8,294,400 B, 8,323,072 B rounded to 64 KiB.
    const D3D12_RESOURCE_DESC hd = target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_ALLOCATION_INFO info = ask(&hd, 1);
    EXPECT(info.SizeInBytes == 8847360 && info.Alignment == 65536);
    // A 3840 x 2160 RGBA16F target: an 8-byte element tile is 128 x 64, 30 across and 34 down (2160 / 64 is 33.75),
    // 1020 tiles = 66,846,720 B, where the PC device says 66,387,968 B.
    const D3D12_RESOURCE_DESC uhd = target_desc(3840, 2160, DXGI_FORMAT_R16G16B16A16_FLOAT);
    EXPECT(ask(&uhd, 1).SizeInBytes == 66846720);
    // Both in one call: the sum of the two console sizes.
    const D3D12_RESOURCE_DESC both[] = {hd, uhd};
    EXPECT(ask(both, 2).SizeInBytes == 8847360 + 66846720);
    // Two slices take two surfaces.
    const D3D12_RESOURCE_DESC two = target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, 2);
    EXPECT(ask(&two, 1).SizeInBytes == 2 * 8847360);

    // What is left alone: a texture that is not a render target, a buffer, a render target of a format the rule does
    // not list, one the PC device refuses, and a surface that is already whole tiles (1024 x 1024 RGBA8).
    D3D12_RESOURCE_DESC sampled = hd;
    sampled.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    EXPECT(ask(&sampled, 1).SizeInBytes == 8323072);
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 100000;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    EXPECT(ask(&buffer, 1).SizeInBytes == 131072);
    D3D12_RESOURCE_DESC odd_format = hd;
    odd_format.Format = DXGI_FORMAT_BC1_UNORM;
    EXPECT(ask(&odd_format, 1).SizeInBytes == 8323072);
    D3D12_RESOURCE_DESC empty = hd;
    empty.Width = 0;
    EXPECT(ask(&empty, 1).SizeInBytes == UINT64_MAX);
    const D3D12_RESOURCE_DESC exact = target_desc(1024, 1024, DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(ask(&exact, 1).SizeInBytes == 4194304);
    // A refused description in a list refuses the list, as before.
    const D3D12_RESOURCE_DESC refused[] = {hd, empty};
    EXPECT(ask(refused, 2).SizeInBytes == UINT64_MAX);
    // A mip chain: 1920 x 1080 with 3 mips is mip 0 in 135 tiles, mip 1 (960 x 540) in 8 x 5 = 40 tiles and mip 2
    // (480 x 270) in 4 x 3 = 12 tiles.
    D3D12_RESOURCE_DESC mips = hd;
    mips.MipLevels = 3;
    EXPECT(ask(&mips, 1).SizeInBytes == (135 + 40 + 12) * 65536ull);

    // A texture the GPU writes through unordered access and the title also clears with the CPU through its lock
    // with a layout word naming its mode: 0x100, linear. Its console layout is 36 slices of 32 rows
    // of 256 bytes, 0x48000 bytes, where the PC device says 0x30000; the larger size wins, rounded to the
    // description's alignment. The title's own layout word decides, though the PC form of the description has lost it.
    D3D12_RESOURCE_DESC grid{};
    grid.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    grid.Width = 32;
    grid.Height = 32;
    grid.DepthOrArraySize = 36;
    grid.MipLevels = 1;
    grid.Format = DXGI_FORMAT_R32_UINT;
    grid.SampleDesc.Count = 1;
    grid.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x100);
    grid.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    EXPECT(ask(&grid, 1).SizeInBytes == 0x50000);
    // In a list it takes its console size at its own offset.
    const D3D12_RESOURCE_DESC grid_and_target[] = {grid, hd};
    EXPECT(ask(grid_and_target, 2).SizeInBytes == 0x50000 + 8847360);
    // The rule only grows: a mode XG lays out smaller (4 KiB thick blocks, 32x32x40 x 4 B = 0x28000) keeps the PC
    // size, and so does a layout word that leaves the mode to XG, or a mode XG's model lacks.
    D3D12_RESOURCE_DESC smaller = grid;
    smaller.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x105);
    EXPECT(ask(&smaller, 1).SizeInBytes == 0x30000);
    D3D12_RESOURCE_DESC no_mode = grid;
    no_mode.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    EXPECT(ask(&no_mode, 1).SizeInBytes == 0x30000);
    D3D12_RESOURCE_DESC unknown_mode = grid;
    unknown_mode.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(0x11b);  // SW_64KB_R_X
    EXPECT(ask(&unknown_mode, 1).SizeInBytes == 0x30000);

    // The PC form (this in RCX, the buffer in RDX) is not the title's call: it gets the PC device's own size, 127
    // units of 64 KiB for the mock, so code that compares the proxy with the PC device still agrees.
    D3D12_RESOURCE_ALLOCATION_INFO pc{};
    EXPECT(raw(device, 25, word(&pc), 0, 1, word(&hd)) == word(&pc));
    EXPECT(pc.SizeInBytes == 8323072);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 25 pads render targets to whole console tiles and leaves the other sizes as the PC "
                "device gives them\n", g_label);
    return 0;
}

// ---- 7b. Device slot 25 hands the PC device clean descriptions and leaves the title's array alone ----

int check_allocation_descriptions_cleaned() {
    constexpr std::uint32_t kXboxTiledLayout = 0x101, kXboxRowMajorLayout = 0x1, kXboxFlagBits = 0x40000 | 0x20000;
    constexpr D3D12_RESOURCE_FLAGS kPcFlags =
        D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Mock* mock = nullptr;
    void* device = make_device(mock, 0, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    using ConsoleFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                               const D3D12_RESOURCE_DESC*);
    const auto get_info = reinterpret_cast<ConsoleFn>(vtable[25]);

    // A texture with the Xbox tiled layout word and Xbox bits above the PC flags, and a buffer with the row-major word.
    D3D12_RESOURCE_DESC titles[2] = {target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM), {}};
    titles[0].Flags = static_cast<D3D12_RESOURCE_FLAGS>(kPcFlags | kXboxFlagBits);
    titles[0].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kXboxTiledLayout);
    titles[1].Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    titles[1].Width = 100000;
    titles[1].Height = 1;
    titles[1].DepthOrArraySize = 1;
    titles[1].MipLevels = 1;
    titles[1].SampleDesc.Count = 1;
    titles[1].Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kXboxRowMajorLayout);
    titles[1].Flags = static_cast<D3D12_RESOURCE_FLAGS>(kXboxFlagBits);
    D3D12_RESOURCE_DESC kept[2];
    std::memcpy(kept, titles, sizeof(kept));

    D3D12_RESOURCE_ALLOCATION_INFO info{};
    g_seen_desc_count = 0;
    g_seen_calls = 0;
    std::memset(g_seen_descs, 0xCC, sizeof(g_seen_descs));
    EXPECT(get_info(&info, device, 0, 2, titles) == &info);
    // The PC device saw both descriptions with PC flags and a PC layout, the rest of each as the title wrote it.
    EXPECT(g_seen_desc_count == 2);
    EXPECT(g_seen_descs[0].Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN && g_seen_descs[0].Flags == kPcFlags);
    EXPECT(g_seen_descs[0].Width == 1920 && g_seen_descs[0].Height == 1080 &&
           g_seen_descs[0].Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    EXPECT(g_seen_descs[1].Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR && g_seen_descs[1].Flags == D3D12_RESOURCE_FLAG_NONE);
    EXPECT(g_seen_descs[1].Width == 100000);
    // The title's array is as it was, and the size still follows the render-target flag it kept (the proxy's later
    // per-target questions to the PC device are not recorded).
    EXPECT(std::memcmp(kept, titles, sizeof(kept)) == 0);
    EXPECT(info.SizeInBytes == 8847360 + 131072);

    const D3D12_TEXTURE_LAYOUT normalized_layouts[] = {
        static_cast<D3D12_TEXTURE_LAYOUT>(kXboxTiledLayout),
        static_cast<D3D12_TEXTURE_LAYOUT>(0x102),
        static_cast<D3D12_TEXTURE_LAYOUT>(0x105),
        D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
    };
    for (const auto layout : normalized_layouts) {
        D3D12_RESOURCE_DESC descs[] = {titles[0], titles[1]};
        descs[0].Layout = descs[1].Layout = layout;
        g_seen_calls = 0;
        EXPECT(get_info(&info, device, 0, 2, descs) == &info);
        EXPECT(g_seen_desc_count == 2);
        EXPECT(g_seen_descs[0].Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
        EXPECT(g_seen_descs[1].Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    }

    const D3D12_TEXTURE_LAYOUT preserved_layouts[] = {
        D3D12_TEXTURE_LAYOUT_UNKNOWN,
        D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
        D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE,
    };
    for (const auto layout : preserved_layouts) {
        D3D12_RESOURCE_DESC descs[] = {titles[0], titles[1]};
        descs[0].Layout = descs[1].Layout = layout;
        g_seen_calls = 0;
        EXPECT(get_info(&info, device, 0, 2, descs) == &info);
        EXPECT(g_seen_desc_count == 2);
        EXPECT(g_seen_descs[0].Layout == layout && g_seen_descs[1].Layout == layout);
    }

    constexpr auto kUav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    constexpr auto kDepthStencil = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    constexpr auto kRenderTarget = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_RESOURCE_DESC formats[] = {
        target_desc(64, 64, DXGI_FORMAT_B5G5R5A1_UNORM),
        target_desc(64, 64, DXGI_FORMAT_R32G8X24_TYPELESS),
        target_desc(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM),
        target_desc(64, 64, DXGI_FORMAT_R32_FLOAT),
    };
    formats[0].Flags = kPcFlags;
    formats[1].Flags = kDepthStencil | kUav;
    formats[2].Flags = kPcFlags;
    formats[3].Flags = kUav;
    g_seen_calls = 0;
    EXPECT(get_info(&info, device, 0, 4, formats) == &info);
    EXPECT(g_seen_desc_count == 4);
    EXPECT(g_seen_descs[0].Flags == kRenderTarget);
    EXPECT(g_seen_descs[1].Flags == kDepthStencil);
    EXPECT(g_seen_descs[2].Flags == kPcFlags);
    EXPECT(g_seen_descs[3].Flags == kUav);

    // PC calls must keep their layout and flags so the host can reject an invalid descriptor.
    using PcFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(void*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT,
                                                          const D3D12_RESOURCE_DESC*);
    const auto get_pc_info = reinterpret_cast<PcFn>(vtable[25]);
    D3D12_RESOURCE_DESC pc_buffer = titles[1];
    pc_buffer.Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE;
    pc_buffer.Flags = static_cast<D3D12_RESOURCE_FLAGS>(0x1000);
    g_seen_calls = 0;
    EXPECT(get_pc_info(device, &info, 0, 1, &pc_buffer) == &info);
    EXPECT(g_seen_desc_count == 1 && g_seen_descs[0].Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE);
    EXPECT(g_seen_descs[0].Flags == pc_buffer.Flags);

    // A null array with a zero count reaches the PC device as it came.
    g_seen_desc_count = 99;
    g_seen_calls = 0;
    EXPECT(get_info(&info, device, 0, 0, nullptr) == &info && g_seen_calls == 1 && g_seen_desc_count == 0);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: device slot 25 gives the PC device descriptions with PC flags and layout, and the title's own "
                "array is not written\n", g_label);
    return 0;
}

// ---- 11. Console render target layout: alignment, per-description offsets, mips, formats, overflow ----

int check_render_target_layout() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* const* vtable = *reinterpret_cast<void* const* const*>(device);
    // The retail executable's calls: the buffer for the struct in RCX, the device in RDX.
    using InfoFn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                            const D3D12_RESOURCE_DESC*);
    using Info1Fn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(D3D12_RESOURCE_ALLOCATION_INFO*, void*, UINT, UINT,
                                                             const D3D12_RESOURCE_DESC*,
                                                             D3D12_RESOURCE_ALLOCATION_INFO1*);
    const auto get_info = reinterpret_cast<InfoFn>(vtable[25]);
    const auto get_info1 = reinterpret_cast<Info1Fn>(vtable[56]);
    auto ask = [&](const D3D12_RESOURCE_DESC* descs, UINT count) {
        D3D12_RESOURCE_ALLOCATION_INFO info{0xDEAD, 0xBEEF};
        if (get_info(&info, device, 0, count, descs) != &info) info = {0, 0};
        return info;
    };
    auto ask_one = [&](const D3D12_RESOURCE_DESC& d) { return ask(&d, 1); };
    auto target = [](std::uint64_t w, UINT h, DXGI_FORMAT f, UINT slices, UINT mips) {
        D3D12_RESOURCE_DESC d = target_desc(w, h, f, slices);
        d.MipLevels = static_cast<UINT16>(mips);
        return d;
    };

    // A four-sample 1 x 1 target is 4 MiB at 4 MiB alignment. Next to a 1080p target that grows to 8,847,360 B, the
    // two need 4 MiB + 8,847,360 B = 13,041,664 B placed, and the total is a whole number of 4 MiB: 16 MiB, in
    // either order. (The PC device says 12 MiB for them; adding the 512 KiB the padding takes would say 12.5 MiB.)
    const D3D12_RESOURCE_DESC hd = target_desc(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM);
    D3D12_RESOURCE_DESC msaa = target_desc(1, 1, DXGI_FORMAT_R8G8B8A8_UNORM);
    msaa.SampleDesc.Count = 4;
    const D3D12_RESOURCE_DESC msaa_first[] = {msaa, hd};
    const D3D12_RESOURCE_DESC msaa_last[] = {hd, msaa};
    D3D12_RESOURCE_ALLOCATION_INFO info = ask(msaa_first, 2);
    EXPECT(info.SizeInBytes == 16777216 && info.Alignment == 4194304);
    info = ask(msaa_last, 2);
    EXPECT(info.SizeInBytes == 16777216 && info.Alignment == 4194304);
    EXPECT(ask(&msaa, 1).SizeInBytes == 4194304);  // multisampled targets are left as the PC device sizes them

    // GetResourceAllocationInfo1: the offsets follow the padded sizes, in the console form.
    const D3D12_RESOURCE_DESC two[] = {hd, hd};
    D3D12_RESOURCE_ALLOCATION_INFO1 each[2]{};
    D3D12_RESOURCE_ALLOCATION_INFO total{};
    EXPECT(get_info1(&total, device, 0, 2, two, each) == &total);
    EXPECT(total.SizeInBytes == 17694720 && total.Alignment == 65536);
    EXPECT(each[0].Offset == 0 && each[0].SizeInBytes == 8847360 && each[0].Alignment == 65536);
    EXPECT(each[1].Offset == 8847360 && each[1].SizeInBytes == 8847360 && each[1].Alignment == 65536);
    // The per-description array is optional: without one the total is the same 17,694,720 B.
    D3D12_RESOURCE_ALLOCATION_INFO no_array{};
    EXPECT(get_info1(&no_array, device, 0, 2, two, nullptr) == &no_array);
    EXPECT(no_array.SizeInBytes == 17694720 && no_array.Alignment == 65536);
    D3D12_RESOURCE_ALLOCATION_INFO1 mixed[2]{};
    D3D12_RESOURCE_ALLOCATION_INFO pc{};
    EXPECT(get_info1(&pc, device, 0, 2, msaa_first, mixed) == &pc);
    EXPECT(pc.SizeInBytes == 16777216 && pc.Alignment == 4194304);
    EXPECT(mixed[0].Offset == 0 && mixed[0].SizeInBytes == 4194304 && mixed[0].Alignment == 4194304);
    EXPECT(mixed[1].Offset == 4194304 && mixed[1].SizeInBytes == 8847360 && mixed[1].Alignment == 65536);
    // Nothing to pad: the PC device's answer stands, per-description array included.
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 100000;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    const D3D12_RESOURCE_DESC buffers[] = {buffer, buffer};
    D3D12_RESOURCE_ALLOCATION_INFO1 plain[2]{};
    EXPECT(get_info1(&total, device, 0, 2, buffers, plain) == &total);
    EXPECT(total.SizeInBytes == 262144 && plain[1].Offset == 131072 && plain[1].SizeInBytes == 131072);

    // Mips: 0 is the full chain. 256 x 128 RGBA8 takes two tiles for mip 0, one for mip 1 (128 x 64 is wider than half a
    // tile) and one tail tile for the rest.
    EXPECT(ask_one(target(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 0)).SizeInBytes ==
           4 * 65536ull);
    // 128 x 32 with two mips and two slices: mip 0 is a whole tile, mip 1 (64 x 16) is the tail tile, per slice.
    EXPECT(ask_one(target(128, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2)).SizeInBytes ==
           4 * 65536ull);
    // 257 x 32 with two mips: mip 0 is 3 tiles across, mip 1 is 129 x 16 (an odd extent rounds up): 2 tiles.
    EXPECT(ask_one(target(257, 32, DXGI_FORMAT_R8G8B8A8_UNORM, 1, 2)).SizeInBytes ==
           5 * 65536ull);
    // 2-byte B5G5R5A1 is a 256 x 128 tile: 257 x 64 takes two.
    EXPECT(ask_one(target(257, 64, DXGI_FORMAT_B5G5R5A1_UNORM, 1, 1)).SizeInBytes ==
           2 * 65536ull);
    // 16-byte RGBA32F is a 64 x 64 tile: 65 x 32 takes two.
    EXPECT(ask_one(target(65, 32, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1)).SizeInBytes ==
           2 * 65536ull);

    // Sums saturate where the PC device's do. A 2^26 x 2^26 RGBA32F target of 200 slices is 200 x 2^56 B (the PC mock
    // caps it at 4 GiB); two of them are past 2^64.
    const D3D12_RESOURCE_DESC huge = target(1ull << 26, 1u << 26, DXGI_FORMAT_R32G32B32A32_FLOAT, 200, 1);
    EXPECT(ask(&huge, 1).SizeInBytes == 200ull << 56);
    const D3D12_RESOURCE_DESC huge_two[] = {huge, huge};
    info = ask(huge_two, 2);
    EXPECT(info.SizeInBytes == 0xFFFFFFFFFFFF0000ull && info.Alignment == 65536);

    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: render targets are laid out at their own alignments, with per-description offsets, mip chains "
                "and saturating sums\n", g_label);
    return 0;
}


// ---- 9. The refused tail and the guard region ----

int check_tail(bool want_guarded) {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    void* fence = make_fence(device);
    EXPECT(queue && fence);
    EXPECT(xs::command_object_tables_guarded() == want_guarded);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    // Both wrappers keep a reference until the native call returns.
    queue_mock->observed_queue = queue;
    queue_mock->observed_fence = fence;
    for (unsigned slot : {14u, 15u}) {
        queue_mock->observed_queue_refs = queue_mock->observed_fence_refs = 0;
        EXPECT(hr_of(raw(queue, slot, word(fence), 5)) == S_OK && queue_mock->calls[slot] == 1 &&
               queue_mock->args[slot][0] == word(xs::fence_proxy_real_fence(fence)) && queue_mock->args[slot][1] == 5);
        EXPECT(queue_mock->observed_queue_refs == 2 && queue_mock->observed_fence_refs == 2);
        EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && xs::command_object_proxy_ref_count(fence) == 1);
    }
    // Console-only queue methods (the tail past the PC chain, SignalX, PresentX) report S_OK instead of
    // refusing: frame pacing flows through them. Nothing is forwarded — the real queue never sees them.
    for (unsigned slot = 19; slot < xs::queue_proxy_table_size(); ++slot) {
        if (xs::queue_slot_overridden(slot)) continue;  // slot 35 is forwarded (check_xbox_queue_slots)
        EXPECT(hr_of(raw(queue, slot)) == S_OK && queue_mock->calls[slot] == 0);
    }
    EXPECT(raw(fence, 12) != 0);  // the completed value's address (check_fence_completed_value)
    for (unsigned slot = 13; slot < xs::fence_proxy_table_size(); ++slot) EXPECT(hr_of(raw(fence, slot)) == E_NOTIMPL);
    for (unsigned slot : {16u, 17u, 25u}) {
        const unsigned native_slot = slot == 17u ? 15u : 14u;
        const unsigned expected_calls = queue_mock->calls[native_slot] + 1;
        queue_mock->observed_queue_refs = queue_mock->observed_fence_refs = 0;
        EXPECT(hr_of(raw(queue, slot, word(fence), 6)) == S_OK && queue_mock->calls[native_slot] == expected_calls &&
               queue_mock->args[native_slot][0] == word(xs::fence_proxy_real_fence(fence)) &&
               queue_mock->args[native_slot][1] == 6);
        EXPECT(queue_mock->observed_queue_refs == 2 && queue_mock->observed_fence_refs == 2);
        EXPECT(xs::command_object_proxy_ref_count(queue) == 1 && xs::command_object_proxy_ref_count(fence) == 1);
    }
    EXPECT(xs::queue_table_overruns() == 0 && xs::fence_table_overruns() == 0);
    EXPECT(release(queue) == 0 && release(fence) == 0 && release(device) == 0 && mock->refs == 0);
    return 0;
}

struct HeapHookState {
    void* next_ptr = nullptr;
    std::size_t last_alloc_size = 0;
    std::uint64_t last_alloc_attr = 0;
    int alloc_calls = 0;
    void* last_free_ptr = nullptr;
    std::uint64_t last_free_attr = 0;
    int free_calls = 0;
} g_heap_hook;

void* mock_heap_alloc(std::size_t byte_size, std::uint64_t attributes) {
    ++g_heap_hook.alloc_calls;
    g_heap_hook.last_alloc_size = byte_size;
    g_heap_hook.last_alloc_attr = attributes;
    return g_heap_hook.next_ptr;
}

void mock_heap_free(void* ptr, std::uint64_t attributes) {
    ++g_heap_hook.free_calls;
    g_heap_hook.last_free_ptr = ptr;
    g_heap_hook.last_free_attr = attributes;
}

int check_descriptor_heap_hooks() {
    g_heap_hook = {};
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);

    Mock* real = new_mock(Kind::device);
    real->refs = 1;
    real->device_level = 9;
    void* device = nullptr;
    EXPECT(SUCCEEDED(xs::create_device_proxy(reinterpret_cast<ID3D12Device*>(real), __uuidof(ID3D12Device), &device)));

    constexpr SIZE_T kCustomBase = 0x77770000ull;
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kCustomBase);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    desc.NumDescriptors = 8;
    desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const IID iid_heap = __uuidof(ID3D12DescriptorHeap);
    void* heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&heap)))));
    EXPECT(heap != nullptr && xs::is_heap_proxy(heap));
    EXPECT(g_heap_hook.alloc_calls == 1);
    EXPECT(g_heap_hook.last_alloc_size == desc.NumDescriptors * kMockDescriptorInc);
    EXPECT(g_heap_hook.last_alloc_attr == xs::kXmemDescriptorHeapAttributes);

    // Slot 9 returns the title-supplied base; slot 10 keeps the real GPU start so (gpu - cpu) delta stays exact.
    EXPECT(raw(heap, 9) == kCustomBase);
    EXPECT(raw(heap, 10) == kMockHeapGpuStart);

    // Public translator and device slots 17-24 remap handles in [kCustomBase, +256) to kMockHeapCpuStart + offset.
    const SIZE_T off3 = 3 * kMockDescriptorInc;
    const SIZE_T off7 = 7 * kMockDescriptorInc;
    const SIZE_T outside = kCustomBase + desc.NumDescriptors * kMockDescriptorInc;
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase + off3}).ptr ==
           kMockHeapCpuStart + off3);
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{outside}).ptr == outside);

    raw(device, 17, 0x11, kCustomBase + off3);                                          // CreateConstantBufferView
    EXPECT(real->args[17][1] == kMockHeapCpuStart + off3);
    raw(device, 18, 0x22, 0x33, kCustomBase + off3);                                    // CreateShaderResourceView
    EXPECT(real->args[18][2] == kMockHeapCpuStart + off3);
    raw(device, 19, 0x22, 0x33, 0x44, kCustomBase + off7);                              // CreateUnorderedAccessView
    EXPECT(real->args[19][3] == kMockHeapCpuStart + off7);
    raw(device, 20, 0x22, 0x33, kCustomBase + off3);                                    // CreateRenderTargetView
    EXPECT(real->args[20][2] == kMockHeapCpuStart + off3);
    raw(device, 21, 0x22, 0x33, kCustomBase + off7);                                    // CreateDepthStencilView
    EXPECT(real->args[21][2] == kMockHeapCpuStart + off7);
    raw(device, 22, 0x55, kCustomBase + off3);                                          // CreateSampler
    EXPECT(real->args[22][1] == kMockHeapCpuStart + off3);
    raw(device, 24, 1, kCustomBase + off7, kCustomBase + off3, 0);                      // CopyDescriptorsSimple
    EXPECT(real->args[24][1] == kMockHeapCpuStart + off7);
    EXPECT(real->args[24][2] == kMockHeapCpuStart + off3);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsts[2] = {{kCustomBase + off3}, {outside}};
        D3D12_CPU_DESCRIPTOR_HANDLE srcs[2] = {{kCustomBase + off7}, {0x99990000ull}};
        UINT sizes[2] = {1, 1};
        static_cast<ID3D12Device*>(device)->CopyDescriptors(2, dsts, sizes, 2, srcs, sizes,
                                                            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        EXPECT(real->array[23][0] == kMockHeapCpuStart + off3 && real->array[23][1] == outside);
        EXPECT(real->array[24][0] == kMockHeapCpuStart + off7 && real->array[24][1] == 0x99990000ull);
    }

    // Releasing the heap unregisters the range and invokes the free callback with the same attribute word.
    EXPECT(raw(heap, 2) == 0);
    EXPECT(g_heap_hook.free_calls == 1);
    EXPECT(g_heap_hook.last_free_ptr == reinterpret_cast<void*>(kCustomBase));
    EXPECT(g_heap_hook.last_free_attr == xs::kXmemDescriptorHeapAttributes);
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase + off3}).ptr ==
           kCustomBase + off3);

    // Null return from the hook falls back cleanly to the PC heap's start and never calls free.
    g_heap_hook.next_ptr = nullptr;
    void* plain_heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&plain_heap)))));
    EXPECT(plain_heap != nullptr && raw(plain_heap, 9) == kMockHeapCpuStart);
    EXPECT(raw(plain_heap, 2) == 0);
    EXPECT(g_heap_hook.free_calls == 1);

    // QI failure on heap proxy creation (unsupported IID) frees the hook backing immediately.
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kCustomBase);
    void* bad_heap = nullptr;
    EXPECT(hr_of(raw(device, 14, word(&desc), word(&kExtraIid), word(&bad_heap))) == E_NOINTERFACE);
    EXPECT(bad_heap == nullptr && g_heap_hook.free_calls == 2);
    EXPECT(g_heap_hook.last_free_ptr == reinterpret_cast<void*>(kCustomBase));

    // OOM on HeapProxy allocation frees the hook backing and leaves no dangling range.
    g_fail_news = 1;
    void* oom_heap = nullptr;
    EXPECT(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&oom_heap))) == E_OUTOFMEMORY);
    EXPECT(oom_heap == nullptr && g_heap_hook.free_calls == 3);
    EXPECT(xs::translate_cpu_descriptor_handle(device, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase}).ptr == kCustomBase);

    EXPECT(raw(device, 2) == 0 && real->refs == 0);
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    return 0;
}

// ---- Committed resources lease their size through the title's XMem hooks (RT-5 stop 17) ----
// Slots 76 and 127 allocate the slot-25 size of the resource through the installed hooks with page-size field 2, the
// resource holds the lease, and the hooks' free runs once, when the resource is destroyed.
int check_committed_resource_leases() {
    g_heap_hook = {};
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);
    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device);
    constexpr std::uint64_t kLeaseAttributes = (0x80ULL << 16) | (2ULL << 14);
    static std::uint8_t lease_memory[64];
    g_heap_hook.next_ptr = lease_memory;

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 1920;
    desc.Height = 1080;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    // Slot 76: the lease is the console-tiled size slot 25 answers (135 tiles), not the PC one.
    void* resource = nullptr;
    std::uint64_t words76[10] = {word(&heap), 0, word(&desc), 0, 0, word(&kResourceIid), word(&resource), 0, 0, 0};
    EXPECT(hr_of(raw10(device, 76, words76)) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 1 && g_heap_hook.last_alloc_size == 8847360);
    EXPECT(g_heap_hook.last_alloc_attr == kLeaseAttributes);
    EXPECT(g_heap_hook.free_calls == 0 && reinterpret_cast<Mock*>(resource)->held);
    EXPECT(release(resource) == 0);
    EXPECT(g_heap_hook.free_calls == 1 && g_heap_hook.last_free_ptr == lease_memory &&
           g_heap_hook.last_free_attr == kLeaseAttributes);

#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    // Slot 127, a buffer: its PC size, leased the same way.
    D3D12_RESOURCE_DESC1 buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 3 * 65536;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource = nullptr;
    std::uint64_t words127[10] = {word(&heap), 0, word(&buffer), 0xFFFFFFFFu, 0, 0, 0, 0, word(&kResourceIid),
                                  word(&resource)};
    EXPECT(hr_of(raw10(device, 127, words127)) == S_OK && resource);
    EXPECT(g_heap_hook.alloc_calls == 2 && g_heap_hook.last_alloc_size == 3 * 65536);
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == 2);
#endif

    // A driver whose PC answer is above the console tiles (AMD's Windows driver: 513 x 64 KiB for a 3840 x 2160 RGBA8
    // target, 510 tiles on the console) still leases the tiles: the title's 64 MiB range holds two of those and no more.
    desc.Width = 3840;
    desc.Height = 2160;
    g_texture_size_bias = 6 * 65536;  // the mock's 507 x 64 KiB becomes 513 x 64 KiB
    resource = nullptr;
    EXPECT(hr_of(raw10(device, 76, words76)) == S_OK && resource);
    EXPECT(g_heap_hook.last_alloc_size == 30ull * 17 * 65536);
    EXPECT(release(resource) == 0);
    g_texture_size_bias = 0;
    desc.Width = 1920;
    desc.Height = 1080;

    // Hooks that return null: the resource is still created, and nothing is freed later.
    g_heap_hook.next_ptr = nullptr;
    resource = nullptr;
    EXPECT(hr_of(raw10(device, 76, words76)) == S_OK && resource && !reinterpret_cast<Mock*>(resource)->held);
    const unsigned frees = g_heap_hook.free_calls;
    EXPECT(release(resource) == 0 && g_heap_hook.free_calls == frees);

    // No hooks: no lease.
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    const unsigned allocs = g_heap_hook.alloc_calls;
    resource = nullptr;
    EXPECT(hr_of(raw10(device, 76, words76)) == S_OK && resource && g_heap_hook.alloc_calls == allocs);
    EXPECT(release(resource) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: committed resources lease their slot-25 size through the title's XMem hooks until destroyed\n",
                g_label);
    return 0;
}

int check_list_cpu_handles() {
    g_heap_hook = {};
    xs::set_descriptor_heap_allocator(&mock_heap_alloc, &mock_heap_free);

    Mock* real = nullptr;
    void* device = make_device(real, 9, true, true, true);
    EXPECT(device != nullptr);

    constexpr SIZE_T kCustomBase = 0x77880000ull;
    g_heap_hook.next_ptr = reinterpret_cast<void*>(kCustomBase);
    D3D12_DESCRIPTOR_HEAP_DESC desc{};
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    desc.NumDescriptors = 8;
    const IID iid_heap = __uuidof(ID3D12DescriptorHeap);
    void* heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&desc), word(&iid_heap), word(&heap)))));
    EXPECT(heap != nullptr && raw(heap, 9) == kCustomBase);

    const IID iid_alloc = __uuidof(ID3D12CommandAllocator);
    const IID iid_gcl = __uuidof(ID3D12GraphicsCommandList);
    void* allocator = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&iid_alloc), word(&allocator)))));
    void* list = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0,
                               word(&iid_gcl), word(&list)))));
    EXPECT(list != nullptr && xs::is_list_proxy(list));
    Mock* const real_list = reinterpret_cast<Mock*>(xs::list_proxy_real_list(list));
    EXPECT(real_list != nullptr);

    const SIZE_T off1 = 1 * kMockDescriptorInc;
    const SIZE_T off4 = 4 * kMockDescriptorInc;
    const SIZE_T off6 = 6 * kMockDescriptorInc;
    const SIZE_T outside = kCustomBase + desc.NumDescriptors * kMockDescriptorInc;

    // Console slot 50 -> PC slot 46 (OMSetRenderTargets): multiple RTV handles + DSV handle, caller arrays untouched.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = {{kCustomBase + off1}, {outside}};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{kCustomBase + off6};
        raw(list, 50, 2, word(rtvs), FALSE, word(&dsv));
        EXPECT(real_list->calls[46] == 1);
        EXPECT(real_list->om_rtv_count == 2 && real_list->om_single_range == FALSE);
        EXPECT(real_list->om_rtvs[0] == kMockHeapCpuStart + off1 && real_list->om_rtvs[1] == outside);
        EXPECT(real_list->om_has_dsv && real_list->om_dsv == kMockHeapCpuStart + off6);
        EXPECT(rtvs[0].ptr == kCustomBase + off1 && rtvs[1].ptr == outside && dsv.ptr == kCustomBase + off6);
    }
    // Single descriptor range: only rtvs[0] is read and translated even when NumRenderTargetDescriptors > 1; null DSV.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE single_rtv{kCustomBase + off4};
        raw(list, 50, 3, word(&single_rtv), TRUE, 0);
        EXPECT(real_list->calls[46] == 2);
        EXPECT(real_list->om_rtv_count == 3 && real_list->om_single_range == TRUE);
        EXPECT(real_list->om_rtvs[0] == kMockHeapCpuStart + off4 && !real_list->om_has_dsv);
        EXPECT(single_rtv.ptr == kCustomBase + off4);
    }
    // Zero RTVs with a non-null DSV.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{kCustomBase + off1};
        raw(list, 50, 0, 0, FALSE, word(&dsv));
        EXPECT(real_list->calls[46] == 3);
        EXPECT(real_list->om_rtv_count == 0 && real_list->om_has_dsv && real_list->om_dsv == kMockHeapCpuStart + off1);
    }
    // Count beyond D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT (8): forwards safely without overrunning stack scratch.
    {
        D3D12_CPU_DESCRIPTOR_HANDLE nine[9]{};
        for (UINT i = 0; i < 9; ++i) nine[i].ptr = outside + i * kMockDescriptorInc;
        raw(list, 50, 9, word(nine), FALSE, 0);
        EXPECT(real_list->calls[46] == 4 && real_list->om_rtv_count == 9);
    }

    // Console slot 51 -> PC slot 47 (ClearDepthStencilView): CPU handle translated AND FLOAT Depth in XMM3 preserved.
    {
        using ClearDsvFn = void(WINAPI*)(void*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT,
                                         const D3D12_RECT*);
        void* const* list_vtbl = *reinterpret_cast<void* const* const*>(list);
        auto clear_dsv = reinterpret_cast<ClearDsvFn>(list_vtbl[51]);
        const D3D12_RECT rect{1, 2, 3, 4};
        clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{kCustomBase + off6},
                  D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.375f, 0x5Au, 1, &rect);
        EXPECT(real_list->calls[47] == 1);
        EXPECT(real_list->clear_dsv_handle == kMockHeapCpuStart + off6);
        EXPECT(real_list->clear_dsv_flags == (D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL));
        EXPECT(real_list->clear_dsv_depth == 0.375f);
        EXPECT(real_list->clear_dsv_stencil == 0x5Au);
        EXPECT(real_list->clear_dsv_num_rects == 1 && real_list->clear_dsv_rects == &rect);

        // Second call (past first-call log): outside-range handle passes through and a different depth float holds.
        clear_dsv(list, D3D12_CPU_DESCRIPTOR_HANDLE{outside}, D3D12_CLEAR_FLAG_DEPTH, 0.625f, 0u, 0, nullptr);
        EXPECT(real_list->calls[47] == 2);
        EXPECT(real_list->clear_dsv_handle == outside);
        EXPECT(real_list->clear_dsv_depth == 0.625f);
        EXPECT(real_list->clear_dsv_num_rects == 0 && real_list->clear_dsv_rects == nullptr);
    }

    // Console slot 52 -> PC slot 48 (ClearRenderTargetView): CPU handle translated, color and rects forwarded.
    {
        const FLOAT color[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        const D3D12_RECT rect{0, 0, 8, 8};
        raw(list, 52, kCustomBase + off1, word(color), 1, word(&rect));
        EXPECT(real_list->calls[48] == 1);
        EXPECT(real_list->args[48][0] == kMockHeapCpuStart + off1);
        EXPECT(real_list->args[48][1] == word(color) && real_list->args[48][2] == 1 &&
               real_list->args[48][3] == word(&rect));

        raw(list, 52, outside, word(color), 0, 0);
        EXPECT(real_list->calls[48] == 2 && real_list->args[48][0] == outside);
    }

    // Console slots 53/54 -> PC slots 49/50 (ClearUnorderedAccessViewUint/Float): GPU handle kept, CPU handle remapped.
    {
        const UINT uvals[4] = {1, 2, 3, 4};
        const FLOAT fvals[4] = {1.0f, 0.5f, 0.25f, 0.0f};
        const D3D12_RECT rect{2, 2, 6, 6};
        raw(list, 53, kMockHeapGpuStart + off1, kCustomBase + off4, 0x3333ull, word(uvals), 1, word(&rect));
        EXPECT(real_list->calls[49] == 1);
        EXPECT(real_list->args[49][0] == kMockHeapGpuStart + off1);
        EXPECT(real_list->args[49][1] == kMockHeapCpuStart + off4);
        EXPECT(real_list->args[49][2] == 0x3333ull && real_list->args[49][3] == word(uvals) &&
               real_list->args[49][4] == 1 && real_list->args[49][5] == word(&rect));

        raw(list, 54, kMockHeapGpuStart + off6, kCustomBase + off6, 0x4444ull, word(fvals), 0, 0);
        EXPECT(real_list->calls[50] == 1);
        EXPECT(real_list->args[50][0] == kMockHeapGpuStart + off6);
        EXPECT(real_list->args[50][1] == kMockHeapCpuStart + off6);
        EXPECT(real_list->args[50][2] == 0x4444ull && real_list->args[50][3] == word(fvals) &&
               real_list->args[50][4] == 0 && real_list->args[50][5] == 0);

        raw(list, 54, kMockHeapGpuStart, outside, 0x4444ull, word(fvals), 0, 0);
        EXPECT(real_list->calls[50] == 2 && real_list->args[50][1] == outside);
    }

    EXPECT(raw(list, 2) == 0);
    EXPECT(raw(allocator, 2) == 0);
    EXPECT(raw(heap, 2) == 0);
    EXPECT(raw(device, 2) == 0 && real->refs == 0);
    xs::set_descriptor_heap_allocator(nullptr, nullptr);
    return 0;
}

// ---- 9b. Queue slot 31: CopyPageMappingsBatchX(count, batches, pool, pool pages, flags) ----
//
// The batches as the retail title fills them (game_scarlett_master.exe+0xf2ca74): a 24-byte batch {destination, range
// count, ranges}, 12-byte ranges {kind, start, count}. Each range reaches xmem's hook once; xmem is not linked here, so
// a recording stand-in takes its place.

struct PageRange31 {
    std::uint32_t kind, start, count;
};
struct PageBatch31 {
    std::uint64_t destination;
    std::uint32_t count;
    const PageRange31* ranges;
};
static_assert(sizeof(PageRange31) == 12 && sizeof(PageBatch31) == 24, "the title's batch layout");

struct CopyCall {
    std::uint64_t destination, source, pages;
    bool repeat;
};
CopyCall g_copy_calls[8];
unsigned g_copy_count = 0;
bool g_copy_result = true;

bool record_copy(void* destination, const void* source, std::uint64_t pages, bool repeat_first) {
    if (g_copy_count < std::size(g_copy_calls))
        g_copy_calls[g_copy_count] = {word(destination), word(source), pages, repeat_first};
    ++g_copy_count;
    return g_copy_result;
}

int check_queue_copy_page_mappings() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true);
    EXPECT(device);
    void* queue = make_queue(device);
    EXPECT(queue);
    Mock* const queue_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));
    constexpr std::uint64_t kPage = 0x10000, kPool = 0x7000000000, kHeap = 0x9000000000;
    const PageRange31 first[3] = {{0, 4, 2}, {3, 0, 5}, {1, 7, 3}};  // a run, left as is, one page repeated
    const PageRange31 second[1] = {{2, 0, 6}};                        // unmapped
    const PageBatch31 batches[3] = {{kHeap, 3, first}, {kHeap + 0x100 * kPage, 1, second}, {kHeap, 0, nullptr}};

    // Without xmem the slot keeps the console-only answer every unmodelled queue slot gets.
    rt::xmem::copy_page_mappings_fn = nullptr;
    EXPECT(hr_of(raw(queue, 31, 3, word(batches), kPool, 16, 0)) == S_OK);

    rt::xmem::copy_page_mappings_fn = &record_copy;
    g_copy_count = 0;
    EXPECT(hr_of(raw(queue, 31, 3, word(batches), kPool, 16, 0)) == S_OK && g_copy_count == 3);
    EXPECT(g_copy_calls[0].destination == kHeap && g_copy_calls[0].source == kPool + 4 * kPage &&
           g_copy_calls[0].pages == 2 && !g_copy_calls[0].repeat);
    EXPECT(g_copy_calls[1].destination == kHeap + 7 * kPage && g_copy_calls[1].source == kPool + 7 * kPage &&
           g_copy_calls[1].pages == 3 && g_copy_calls[1].repeat);
    // Kind 3 maps nothing but still moves the destination on (5 pages, so the repeat lands at page 7); kind 2 unmaps.
    EXPECT(g_copy_calls[2].destination == kHeap + 0x100 * kPage && g_copy_calls[2].source == 0 &&
           g_copy_calls[2].pages == 6);
    EXPECT(queue_mock->calls[31] == 0);  // never forwarded to the PC queue

    // A range past the pool, or of an unknown kind, is refused on its own; the others still map.
    const PageRange31 bad[3] = {{0, 15, 2}, {7, 0, 1}, {1, 15, 4}};  // 15 + 2 > 16 pages; kind 7; page 15 is inside
    const PageBatch31 bad_batch[1] = {{kHeap, 3, bad}};
    g_copy_count = 0;
    EXPECT(hr_of(raw(queue, 31, 1, word(bad_batch), kPool, 16, 0)) == E_INVALIDARG && g_copy_count == 1);
    EXPECT(g_copy_calls[0].destination == kHeap + 3 * kPage && g_copy_calls[0].source == kPool + 15 * kPage &&
           g_copy_calls[0].repeat);

    // xmem refusing a range (no window there) makes the call fail too; no batches at all is fine.
    g_copy_result = false;
    EXPECT(hr_of(raw(queue, 31, 1, word(batches), kPool, 16, 0)) == E_INVALIDARG);
    g_copy_result = true;
    EXPECT(hr_of(raw(queue, 31, 0, 0, kPool, 16, 0)) == S_OK);
    EXPECT(hr_of(raw(queue, 31, 2, 0, kPool, 16, 0)) == E_INVALIDARG);

    rt::xmem::copy_page_mappings_fn = nullptr;
    EXPECT(release(queue) == 0 && release(device) == 0 && mock->refs == 0);
    std::printf("xs_proxy_mock: queue slot 31 (CopyPageMappingsBatchX) sends each range to xmem's hook (a run, one page "
                "repeated, unmapped, left as is), refuses a range past the pool or of an unknown kind, and answers "
                "console-only S_OK without xmem\n");
    return 0;
}

// ---- Guard-region setup failures: this executable's import table, patched before the first proxy table ----

std::atomic<int> g_injected{0};
bool g_armed = false;
void* g_real = nullptr;

LPVOID WINAPI fail_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (g_armed && (type & MEM_RESERVE) && size == 64 * 1024) {
        ++g_injected;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    return reinterpret_cast<decltype(&VirtualAlloc)>(g_real)(address, size, type, protect);
}

BOOL WINAPI fail_virtual_protect(LPVOID address, SIZE_T size, DWORD protect, PDWORD old) {
    if (g_armed && protect == PAGE_READONLY) {
        ++g_injected;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return reinterpret_cast<decltype(&VirtualProtect)>(g_real)(address, size, protect, old);
}

PVOID WINAPI fail_add_veh(ULONG, PVECTORED_EXCEPTION_HANDLER) {
    ++g_injected;
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return nullptr;
}

// Points this executable's import of `function` (by name) at `replacement`; the proxy code is linked into it.
bool patch_import(const char* function, void* replacement) {
    auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress) return false;
    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress); desc->Name; ++desc) {
        const DWORD lookup = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + lookup);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            g_real = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);  // through the replacement, not armed yet
            return true;
        }
    }
    return false;
}

int run_fault_mode(const char* mode) {
    struct Mode { const char* flag; const char* function; void* replacement; bool guarded; } const modes[] = {
        {"--fail-virtualalloc", "VirtualAlloc", reinterpret_cast<void*>(&fail_virtual_alloc), false},
        {"--fail-virtualprotect", "VirtualProtect", reinterpret_cast<void*>(&fail_virtual_protect), true},
        {"--fail-veh", "AddVectoredExceptionHandler", reinterpret_cast<void*>(&fail_add_veh), false},
    };
    for (const Mode& m : modes) {
        if (std::strcmp(mode, m.flag) != 0) continue;
        g_label = m.flag;
        EXPECT(patch_import(m.function, m.replacement));
        g_armed = true;
        const int failed = check_tail(m.guarded);
        // Disarmed once the tables exist: with a static CRT the CRT's own calls go through this import table too
        // (its exit path asks VirtualProtect for read-only pages), and only the proxy's calls are meant here.
        g_armed = false;
        if (failed) return 1;
        EXPECT(g_injected.load() > 0);  // the proxy's own call was the one refused
        std::printf("%s: %s refused %d time(s): the tables %s guarded, the proxies forward and answer their "
                    "console tail as before\n", m.flag, m.function, g_injected.load(),
                    m.guarded ? "stay" : "are not");
        return 0;
    }
    return 2;
}

// ---- 12. List tail slot names (D3D-445) ---------------------------------------------------------------------
// From CopyTextureRegionX on, the newer layout sits two slots later than the older one (d3d12_xs_console_abi.md
// 5.3). A diagnostic name follows the device-wide layout, so slot 85 of the newer layout and slot 83 of the older
// layout are both CopyTextureRegionX, and neither number gets that name under the other layout.

int check_list_tail_names() {
    const unsigned char older = xs::kConsoleLayoutOlder, newer = xs::kConsoleLayoutNewer;
    const auto same = [](const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; };
    EXPECT(same(xs::list_tail_slot_name(83, older), "CopyTextureRegionX"));
    EXPECT(same(xs::list_tail_slot_name(85, newer), "CopyTextureRegionX"));
    // The same numbers under the other layout are different methods.
    EXPECT(!xs::list_tail_slot_name(83, newer));  // not a measured slot of the newer layout
    EXPECT(xs::list_tail_slot_name(85, older) && !same(xs::list_tail_slot_name(85, older), "CopyTextureRegionX"));
    // Every measured newer->older pair has one name, and the eight names differ.
    const unsigned pairs[][2] = {{85, 83}, {86, 84}, {87, 85}, {90, 88}, {91, 89}, {92, 90}, {95, 93}, {96, 94}};
    for (const auto& pair : pairs) {
        EXPECT(same(xs::list_tail_slot_name(pair[0], newer), xs::list_tail_slot_name(pair[1], older)));
        for (const auto& other : pairs)
            EXPECT(pair[1] == other[1] ||
                   !same(xs::list_tail_slot_name(pair[1], older), xs::list_tail_slot_name(other[1], older)));
    }
    // Slots outside the measured set have no tail name in either layout, and no slot has one before the layout is
    // known: the log then prints the slot number only, as before.
    for (unsigned slot = 0; slot < 160; ++slot) {
        bool older_tail = false, newer_tail = false;
        for (const auto& pair : pairs) {
            newer_tail = newer_tail || slot == pair[0];
            older_tail = older_tail || slot == pair[1];
        }
        EXPECT((xs::list_tail_slot_name(slot, older) != nullptr) == older_tail);
        EXPECT((xs::list_tail_slot_name(slot, newer) != nullptr) == newer_tail);
        EXPECT(!xs::list_tail_slot_name(slot, 0));
        EXPECT(!xs::list_tail_slot_name(slot, 3));
    }
    return 0;
}

// Device slot 127 is ID3D12Device10::CreateCommittedResource3 with Xbox descriptors (RT-5 stop 15). Past the PC
// vtable it used to be refused with E_NOTIMPL; the PC device must now get the resource with the Xbox layout word and
// flag bits gone and every other word as the title passed it.
int check_committed_resource3_x() {
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    constexpr std::uint32_t kXboxLayoutTiled = 0x101, kXboxFlagBit = 0x40000, kBarrierLayoutUndefined = 0xFFFFFFFFu;
    constexpr std::uint32_t kTightAlignment = 0x400,   // D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT, a PC bit
        kHeapNotZeroed = 0x1000;                       // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED
    static const int session_sentinel = 0;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT castable[2] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    D3D12_RESOURCE_DESC1 texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = 1920;
    texture.Height = 1080;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    texture.SampleDesc.Count = 1;
    texture.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(kXboxLayoutTiled);
    texture.Flags = static_cast<D3D12_RESOURCE_FLAGS>(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | kXboxFlagBit | kTightAlignment);

    // A PC device with ID3D12Device10: the render target comes back, as the console's call returns it.
    Mock* mock = nullptr;
    void* device = make_device(mock, 10, true);
    EXPECT(device && xs::device_proxy_slot_count(device) == 79);
    xs::reset_device_proxy_stats();
    g_committed3 = {};
    void* resource = nullptr;
    std::uint64_t words[10] = {word(&heap), kHeapNotZeroed, word(&texture), kBarrierLayoutUndefined,
                               word(&clear), word(&session_sentinel), 2, word(castable), word(&kResourceIid), word(&resource)};
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK);
    EXPECT(resource && reinterpret_cast<Mock*>(resource)->kind == Kind::resource);
    EXPECT(g_committed3.calls == 1 && xs::device_slot_hits(127) == 1);
    EXPECT(g_committed3.heap == &heap && g_committed3.clear == &clear);
    EXPECT(g_committed3.heap_flags == static_cast<D3D12_HEAP_FLAGS>(kHeapNotZeroed));  // the title's heap flags
    EXPECT(g_committed3.session == &session_sentinel);                                  // and protected session
    EXPECT(g_committed3.layout == kBarrierLayoutUndefined);
    EXPECT(g_committed3.castable_count == 2 && g_committed3.castable_formats == castable);
    EXPECT(g_committed3.riid == &kResourceIid);
    EXPECT(g_committed3.desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN);
    // The Xbox-only bit goes; every PC bit stays, tight alignment included.
    EXPECT(static_cast<std::uint32_t>(g_committed3.desc.Flags) ==
           (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | kTightAlignment));
    EXPECT(g_committed3.desc.Width == 1920 && g_committed3.desc.Height == 1080 &&
           g_committed3.desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS && g_committed3.desc.SampleDesc.Count == 1);
    EXPECT(texture.Layout == static_cast<D3D12_TEXTURE_LAYOUT>(kXboxLayoutTiled));  // the title's description is read only
    EXPECT(release(resource) == 0);  // the title's one reference is the only one: nothing leaked, nothing extra

    // A buffer takes the row-major layout.
    D3D12_RESOURCE_DESC1 buffer = texture;
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Height = 1;
    buffer.Format = DXGI_FORMAT_UNKNOWN;
    words[2] = word(&buffer);
    words[6] = 0;
    words[7] = 0;
    resource = nullptr;
    EXPECT(hr_of(raw10(device, 127, words)) == S_OK && resource);
    EXPECT(g_committed3.calls == 2 && g_committed3.desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR);
    EXPECT(release(resource) == 0);

    // The PC device's own refusal reaches the title with a null object.
    mock->create_result = E_OUTOFMEMORY;
    resource = reinterpret_cast<void*>(1);
    words[9] = word(&resource);
    EXPECT(hr_of(raw10(device, 127, words)) == E_OUTOFMEMORY && !resource);
    mock->create_result = S_OK;

    // A null heap, description, IID or output is refused before the PC device is called.
    const unsigned before = g_committed3.calls;
    std::uint64_t bad[10];
    std::memcpy(bad, words, sizeof(bad));
    bad[0] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    std::memcpy(bad, words, sizeof(bad));
    bad[2] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    std::memcpy(bad, words, sizeof(bad));
    bad[8] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    std::memcpy(bad, words, sizeof(bad));
    bad[9] = 0;
    EXPECT(hr_of(raw10(device, 127, bad)) == E_POINTER);
    EXPECT(g_committed3.calls == before);
    EXPECT(xs::device_slot_overridden(127));
    EXPECT(release(device) == 0);

    // A PC device whose ID3D12Device10 is another pointer than the device itself: the call goes to that one, and the
    // reference the proxy took on it is given back. The base pointer is a trap that answers E_UNEXPECTED.
    Mock* split_mock = nullptr;
    void* split_device = make_device(split_mock, 10, true);
    EXPECT(split_device);
    Mock* alias = new_mock(Kind::device);
    alias->device_level = 10;
    alias->refs = 1;
    split_mock->device10_alias = alias;
    split_mock->create_result = E_UNEXPECTED;
    resource = nullptr;
    words[9] = word(&resource);
    EXPECT(hr_of(raw10(split_device, 127, words)) == S_OK && resource);
    EXPECT(g_committed3.device == alias);
    EXPECT(alias->refs == 1);
    EXPECT(release(resource) == 0);
    EXPECT(release(split_device) == 0);

    // A PC device that stops at ID3D12Device9 has no CreateCommittedResource3: E_NOTIMPL, a null object, no PC call.
    Mock* old_mock = nullptr;
    void* old_device = make_device(old_mock, 9, true);
    EXPECT(old_device && xs::device_proxy_slot_count(old_device) == 76);
    resource = reinterpret_cast<void*>(1);
    words[9] = word(&resource);
    const unsigned old_before = g_committed3.calls;
    EXPECT(hr_of(raw10(old_device, 127, words)) == E_NOTIMPL && !resource);
    EXPECT(g_committed3.calls == old_before && old_mock->calls[76] == 0);
    EXPECT(release(old_device) == 0);
    std::printf("%s: device slot 127 creates the committed resource with the PC flags and layout\n", g_label);
#endif
    return 0;
}

}  // namespace

// ---- 14. List memory writes and waits (list slots 71-74 on the newer layout, d3d12_xs_list_values.h) ----------
// Synthetic title memory, the mock queues' ordered calls and the mock fences stand in for the GPU: a write lands
// only once the queue's write fence is reached, a wait holds its part behind a gate fence the pump signals, a write
// in flight is relayed as a queue Wait on its fence, and a list is cut at a wait recorded after work.

alignas(64) std::uint64_t g_cells[8];

std::uint64_t cell(int i) { return *reinterpret_cast<volatile std::uint64_t*>(&g_cells[i]); }

bool queue_call_is(unsigned i, const Mock* queue, unsigned slot, std::uint64_t a1, std::uint64_t a2) {
    return i < g_queue_call_count && g_queue_calls[i].queue == queue && g_queue_calls[i].slot == slot &&
           g_queue_calls[i].a1 == a1 && g_queue_calls[i].a2 == a2;
}

void* make_console_list(void* device, void* allocator, Mock* device_mock, Mock** real) {
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    void* list = nullptr;
    if (FAILED(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0, word(&gcl_iid),
                         word(&list)))))
        return nullptr;
    *real = device_mock->last_child;
    return list;
}

std::uint64_t submit(void* queue, void* list) {
    void* lists[1] = {list};
    return raw(queue, 35, 1, word(lists));
}

int check_list_values() {
    // The slot decode: newer layout (and an undetected one) 71-74, older 69-72.
    static_assert(xs::console_value_op(71, 0) == xs::ConsoleValueOp::write32);
    static_assert(xs::console_value_op(72, xs::kConsoleLayoutNewer) == xs::ConsoleValueOp::write64);
    static_assert(xs::console_value_op(73, 0) == xs::ConsoleValueOp::wait32);
    static_assert(xs::console_value_op(74, 0) == xs::ConsoleValueOp::wait64);
    static_assert(xs::console_value_op(70, 0) == xs::ConsoleValueOp::none);  // depth bounds on the newer layout
    static_assert(xs::console_value_op(75, 0) == xs::ConsoleValueOp::none);
    static_assert(xs::console_value_op(69, xs::kConsoleLayoutOlder) == xs::ConsoleValueOp::write32);
    static_assert(xs::console_value_op(72, xs::kConsoleLayoutOlder) == xs::ConsoleValueOp::wait64);
    static_assert(xs::console_value_op(73, xs::kConsoleLayoutOlder) == xs::ConsoleValueOp::none);
    // Comparison codes: memory OP reference, D3D12_COMPARISON_FUNC order.
    EXPECT(xs::console_value_met(2, 4, 5, true) && !xs::console_value_met(2, 5, 5, true));
    EXPECT(xs::console_value_met(3, 5, 5, true) && !xs::console_value_met(3, 6, 5, true));
    EXPECT(xs::console_value_met(4, 5, 5, true) && !xs::console_value_met(4, 6, 5, true));
    EXPECT(xs::console_value_met(5, 6, 5, true) && !xs::console_value_met(5, 5, 5, true));
    EXPECT(xs::console_value_met(6, 6, 5, true) && !xs::console_value_met(6, 5, 5, true));
    EXPECT(xs::console_value_met(7, 5, 5, true) && !xs::console_value_met(7, 4, 5, true));
    // A 32-bit wait compares the low halves only; always, never and unknown codes count as met.
    EXPECT(xs::console_value_met(3, 0xffffffff00000005ull, 5, false) &&
           !xs::console_value_met(3, 0xffffffff00000005ull, 5, true));
    EXPECT(xs::console_value_met(7, 0x100000000ull, 1, true) && !xs::console_value_met(7, 0x100000000ull, 1, false));
    EXPECT(xs::console_value_met(1, 0, 1, true) && xs::console_value_met(8, 0, 1, true) &&
           xs::console_value_met(0, 0, 1, true) && xs::console_value_met(9, 0, 1, true));

    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);  // full-chain list mocks
    EXPECT(device);
    mock->zero_fences = true;  // the gate and write fences start at 0: nothing is reached before the test says so
    xs::reset_list_value_stats();
    xs::set_list_value_wait_timeout_ms(5000);
    void* qa = make_queue(device);
    void* qb = make_queue(device);
    EXPECT(qa && qb);
    Mock* const qa_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(qa));
    Mock* const qb_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(qb));
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    Mock* const real_allocator = mock->last_child;
    Mock* lw_real = nullptr;
    Mock* lv_real = nullptr;
    void* lw = make_console_list(device, allocator, mock, &lw_real);  // writes, on queue A
    EXPECT(lw && xs::list_proxy_part_count(lw) == 1);
    EXPECT(hr_of(raw(lw, 9)) == S_OK);  // closed, so the waiter's list can record into the same allocator
    void* lv = make_console_list(device, allocator, mock, &lv_real);  // waits, on queue B
    EXPECT(lv);
    EXPECT(hr_of(raw(lv, 9)) == S_OK);
    std::memset(g_cells, 0, sizeof(g_cells));

    // 1. Writes land after the work before them: not at submission, once the queue's write fence is reached.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    raw(lw, 14, 1, 1, 1);                                  // Dispatch
    raw(lw, 72, word(&g_cells[0]), 0x1374, 0, 0);         // write64(address, value, flush, flags)
    raw(lw, 71, word(&g_cells[1]), 0x10000abcdull, 0, 0);  // write32: the low 32 bits
    raw(lw, 72, word(&g_cells[2]), 0, 0, 1);              // write64 flag 1: a GPU timestamp
    EXPECT(hr_of(raw(lw, 9)) == S_OK && xs::list_proxy_part_count(lw) == 1);
    g_queue_call_count = 0;
    EXPECT(submit(qa, lw) == 0);
    Mock* const a_written = mock->last_child;  // QueueValues::create made the gate, then the write fence
    Mock* const a_gate = a_written - 1;
    EXPECT(a_written->kind == Kind::fence && a_gate->kind == Kind::fence);
    EXPECT(g_queue_call_count == 2 && queue_call_is(0, qa_mock, 10, 1, word(lw_real)) &&
           queue_call_is(1, qa_mock, 14, word(a_written), 1));
    EXPECT(!xs::list_values_idle(30) && cell(0) == 0 && cell(1) == 0 && cell(2) == 0);
    fence_signal(a_written, 1);
    EXPECT(xs::list_values_idle(2000));
    EXPECT(cell(0) == 0x1374 && cell(1) == 0xabcd && cell(2) != 0);
    xs::ListValueStats stats = xs::list_value_stats();
    EXPECT(stats.writes_recorded == 3 && stats.writes_landed == 3 && stats.writes_failed == 0);

    // 2. A wait holds its part until another queue's write lands: the waiter's queue waits on its gate before the
    // part, and the pump signals the gate only after the writer's fence is reached and the value is in memory.
    EXPECT(hr_of(raw(lv, 139, word(allocator), 0)) == S_OK);
    raw(lv, 74, word(&g_cells[3]), 7, 5, 1);  // wait64(address, >=, 5, ?), before any work: the part's gate
    raw(lv, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lv, 9)) == S_OK && xs::list_proxy_part_count(lv) == 1);
    g_queue_call_count = 0;
    EXPECT(submit(qb, lv) == 0);
    Mock* const b_written = mock->last_child;
    Mock* const b_gate = b_written - 1;
    EXPECT(b_written->kind == Kind::fence && b_gate->kind == Kind::fence && b_gate != a_gate);
    EXPECT(g_queue_call_count == 2 && queue_call_is(0, qb_mock, 15, word(b_gate), 1) &&
           queue_call_is(1, qb_mock, 10, 1, word(lv_real)));
    EXPECT(!xs::list_values_idle(30) && fence_completed(b_gate) == 0);
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    raw(lw, 14, 1, 1, 1);
    raw(lw, 72, word(&g_cells[3]), 5, 0, 0);
    EXPECT(hr_of(raw(lw, 9)) == S_OK);
    EXPECT(submit(qa, lw) == 0);
    EXPECT(!xs::list_values_idle(30) && fence_completed(b_gate) == 0 && cell(3) == 0);  // the writer has not run
    fence_signal(a_written, 2);
    EXPECT(xs::list_values_idle(2000) && cell(3) == 5 && fence_completed(b_gate) == 1);
    stats = xs::list_value_stats();
    EXPECT(stats.waits_recorded == 1 && stats.gates_held == 1 && stats.gates_met == 1 && stats.gates_timed_out == 0);

    // 3. A wait a write in flight will meet is relayed: the waiter's queue waits on the writer's fence value.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    raw(lw, 14, 1, 1, 1);
    raw(lw, 72, word(&g_cells[4]), 9, 0, 0);
    EXPECT(hr_of(raw(lw, 9)) == S_OK);
    EXPECT(submit(qa, lw) == 0);  // a_written 3, not reached
    EXPECT(hr_of(raw(lv, 139, word(allocator), 0)) == S_OK);
    raw(lv, 74, word(&g_cells[4]), 7, 9, 1);
    raw(lv, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lv, 9)) == S_OK);
    g_queue_call_count = 0;
    EXPECT(submit(qb, lv) == 0);
    EXPECT(g_queue_call_count == 2 && queue_call_is(0, qb_mock, 15, word(a_written), 3) &&
           queue_call_is(1, qb_mock, 10, 1, word(lv_real)));
    stats = xs::list_value_stats();
    EXPECT(stats.waits_relayed == 1 && stats.gates_held == 1);
    fence_signal(a_written, 3);
    EXPECT(xs::list_values_idle(2000) && cell(4) == 9);

    // 4. A met wait costs nothing (a 32-bit one ignores the high half of memory); an unmet one that nothing meets
    // is bounded: the gate opens after the timeout.
    g_cells[5] = 0xffffffff00000077ull;
    xs::set_list_value_wait_timeout_ms(30);
    EXPECT(hr_of(raw(lv, 139, word(allocator), 0)) == S_OK);
    raw(lv, 73, word(&g_cells[5]), 3, 0x77, 1);  // wait32(address, ==, 0x77): met now
    raw(lv, 73, word(&g_cells[6]), 3, 0x77, 1);  // never written
    raw(lv, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lv, 9)) == S_OK);
    g_queue_call_count = 0;
    const ULONGLONG started = GetTickCount64();
    EXPECT(submit(qb, lv) == 0);
    EXPECT(g_queue_call_count == 2 && queue_call_is(0, qb_mock, 15, word(b_gate), 2));
    EXPECT(xs::list_values_idle(3000) && fence_completed(b_gate) == 2 && GetTickCount64() - started >= 20);
    stats = xs::list_value_stats();
    EXPECT(stats.gates_timed_out == 1 && stats.waits_met_at_submit >= 1);
    xs::set_list_value_wait_timeout_ms(5000);

    // 5. Memory the title's CPU writes opens a gate too (the pump looks at it while a gate is held).
    EXPECT(hr_of(raw(lv, 139, word(allocator), 0)) == S_OK);
    raw(lv, 74, word(&g_cells[7]), 5, 0, 1);  // wait64(address, >, 0)
    raw(lv, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lv, 9)) == S_OK);
    EXPECT(submit(qb, lv) == 0);
    EXPECT(!xs::list_values_idle(30) && fence_completed(b_gate) == 2);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_cells[7]), 1);
    EXPECT(xs::list_values_idle(2000) && fence_completed(b_gate) == 3);

    // 6. A wait after work cuts the list: the part so far closes, recording goes on in a spare list that starts
    // with the state the title set, and the queue runs the first part, stands at the wait, then runs the second.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    const unsigned primary_closes = lw_real->calls[9], primary_dispatches = lw_real->calls[14];
    raw(lw, 25, 0x1234);         // SetPipelineState -> PC 25
    raw(lw, 33, 0x5678);         // SetComputeRootSignature -> PC 29
    raw(lw, 35, 2, 0x9000);      // SetComputeRootDescriptorTable(2, handle) -> PC 31
    raw(lw, 37, 1, 0x42, 3);     // SetComputeRoot32BitConstant(1, value, offset 3) -> PC 33
    const D3D12_VIEWPORT viewport{0, 0, 640, 480, 0, 1};
    raw(lw, 21, 1, word(&viewport));  // RSSetViewports -> PC 21
    raw(lw, 14, 1, 1, 1);
    const unsigned creates = mock->calls[12];
    g_cells[6] = 0;
    raw(lw, 74, word(&g_cells[6]), 7, 1, 1);  // after work: a cut
    EXPECT(xs::list_proxy_part_count(lw) == 2 && xs::list_value_stats().cuts == 1 && mock->calls[12] == creates + 1);
    Mock* const spare = mock->last_child;
    EXPECT(spare->kind == Kind::list && mock->args[12][2] == word(real_allocator));
    EXPECT(lw_real->calls[9] == primary_closes + 1);  // the first part was closed at the cut
    EXPECT(spare->calls[25] == 1 && spare->args[25][0] == 0x1234);
    EXPECT(spare->calls[29] == 1 && spare->args[29][0] == 0x5678);
    EXPECT(spare->calls[31] == 1 && spare->args[31][0] == 2 && spare->args[31][1] == 0x9000);
    EXPECT(spare->calls[33] == 1 && spare->args[33][0] == 1 && spare->args[33][1] == 0x42 && spare->args[33][2] == 3);
    EXPECT(spare->calls[21] == 1 && spare->args[21][0] == 1);
    raw(lw, 14, 2, 2, 2);  // after the wait: the spare records it
    EXPECT(spare->calls[14] == 1 && lw_real->calls[14] == primary_dispatches + 1);
    EXPECT(hr_of(raw(lw, 9)) == S_OK && spare->calls[9] == 1);
    g_queue_call_count = 0;
    EXPECT(submit(qa, lw) == 0);
    EXPECT(g_queue_call_count == 3 && queue_call_is(0, qa_mock, 10, 1, word(lw_real)) &&
           queue_call_is(1, qa_mock, 15, word(a_gate), 1) && queue_call_is(2, qa_mock, 10, 1, word(spare)));
    EXPECT(!xs::list_values_idle(30) && fence_completed(a_gate) == 0);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_cells[6]), 1);
    EXPECT(xs::list_values_idle(2000) && fence_completed(a_gate) == 1);
    // The next recording starts on the primary list again; a later cut reuses the spare (Reset, no new list), and a
    // met wait between the parts puts both in one submission.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK && xs::list_proxy_part_count(lw) == 1);
    raw(lw, 14, 1, 1, 1);
    raw(lw, 74, word(&g_cells[6]), 7, 1, 1);
    EXPECT(xs::list_proxy_part_count(lw) == 2 && mock->calls[12] == creates + 1 && spare->calls[10] == 1);
    raw(lw, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lw, 9)) == S_OK);
    g_queue_call_count = 0;
    EXPECT(submit(qa, lw) == 0);
    EXPECT(g_queue_call_count == 1 && queue_call_is(0, qa_mock, 10, 2, word(lw_real)) &&
           qa_mock->array[10][1] == word(spare));

    // 7. A query open across a wait defers the cut to its EndQuery: a list must end the queries it begins.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    raw(lw, 56, 0x1111, D3D12_QUERY_TYPE_OCCLUSION, 0);  // BeginQuery -> PC 52
    raw(lw, 12, 3, 1, 0, 0);                              // DrawInstanced
    raw(lw, 74, word(&g_cells[6]), 7, 1, 1);
    EXPECT(xs::list_proxy_part_count(lw) == 1 && xs::list_value_stats().cuts_deferred == 1);
    raw(lw, 57, 0x1111, D3D12_QUERY_TYPE_OCCLUSION, 0);  // EndQuery -> PC 53: the cut happens here
    EXPECT(xs::list_proxy_part_count(lw) == 2);
    EXPECT(hr_of(raw(lw, 9)) == S_OK);
    EXPECT(xs::list_value_stats().cut_failures == 0);

    // 8. Writes before a part's first wait (no work yet) land before the queue stands at it, without a cut: the
    // title's direct lists write job tags as their first calls and then wait for compute results whose lists wait
    // for those tags; a gate ahead of the writes would hold both queues until the timeout.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    raw(lw, 72, word(&g_cells[3]), 0x55, 0, 0);  // write64 first
    raw(lw, 74, word(&g_cells[7]), 7, 2, 1);     // then a wait (memory holds 1)
    raw(lw, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lw, 9)) == S_OK && xs::list_proxy_part_count(lw) == 1);
    g_queue_call_count = 0;
    EXPECT(submit(qa, lw) == 0);
    EXPECT(g_queue_call_count == 3 && queue_call_is(0, qa_mock, 14, word(a_written), 4) &&
           queue_call_is(1, qa_mock, 15, word(a_gate), 2) && queue_call_is(2, qa_mock, 10, 1, word(lw_real)));
    fence_signal(a_written, 4);
    EXPECT(!xs::list_values_idle(30) && cell(3) == 0x55 && fence_completed(a_gate) == 1);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_cells[7]), 2);
    EXPECT(xs::list_values_idle(2000) && fence_completed(a_gate) == 2);
    // A write between two waits cuts the list (it must not land before the first wait is passed); with the first
    // wait met, the first part's pre-write and write land behind it, before the queue stands at the second wait.
    EXPECT(hr_of(raw(lw, 139, word(allocator), 0)) == S_OK);
    raw(lw, 72, word(&g_cells[3]), 0x66, 0, 0);
    raw(lw, 74, word(&g_cells[7]), 7, 2, 1);  // met
    raw(lw, 72, word(&g_cells[4]), 0x77, 0, 0);
    raw(lw, 74, word(&g_cells[7]), 7, 3, 1);  // unmet: a cut
    EXPECT(xs::list_proxy_part_count(lw) == 2 && mock->calls[12] == creates + 1);
    raw(lw, 14, 1, 1, 1);
    EXPECT(hr_of(raw(lw, 9)) == S_OK);
    g_queue_call_count = 0;
    EXPECT(submit(qa, lw) == 0);
    EXPECT(g_queue_call_count == 4 && queue_call_is(0, qa_mock, 10, 1, word(lw_real)) &&
           queue_call_is(1, qa_mock, 14, word(a_written), 5) && queue_call_is(2, qa_mock, 15, word(a_gate), 3) &&
           queue_call_is(3, qa_mock, 10, 1, word(spare)));
    fence_signal(a_written, 5);
    EXPECT(!xs::list_values_idle(30) && cell(3) == 0x66 && cell(4) == 0x77 && fence_completed(a_gate) == 2);
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_cells[7]), 3);
    EXPECT(xs::list_values_idle(2000) && fence_completed(a_gate) == 3);

    // Every real list, spare and fence goes with its proxy.
    EXPECT(xs::list_values_idle(2000));
    EXPECT(release(lw) == 0 && lw_real->refs == 0 && spare->refs == 0);
    EXPECT(release(lv) == 0 && lv_real->refs == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(qa) == 0 && release(qb) == 0 && qa_mock->refs == 0 && qb_mock->refs == 0);
    EXPECT(a_gate->refs == 0 && a_written->refs == 0 && b_gate->refs == 0 && b_written->refs == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: list memory writes land after their part, waits hold their part behind a gate (relayed onto a "
                "write in flight, bounded by a timeout), and a wait after work cuts the list with its state replayed\n",
                g_label);
    return 0;
}

// ---- 15. List values, the rest: LEONIDA_LIST_VALUES=0, bundles, ClearState, root constants, landing order across
// queues, the short timeout for every wait, CopyPageMappingsBatchX behind held work, value stores, the exit ----

alignas(64) std::uint64_t g_cells2[32];

std::uint64_t cell2(int i) { return *reinterpret_cast<volatile std::uint64_t*>(&g_cells2[i]); }

struct Tear {
    alignas(8) std::uint64_t cell = 0;
    std::atomic<bool> stop{false};
    std::atomic<unsigned> torn{0}, reads{0};
};
Tear g_tear;

// A CPU job of the title's: writes the label a GPU wait is waiting for, 100 ms after it was started.
DWORD WINAPI label_job(LPVOID cell) {
    Sleep(100);
    InterlockedExchange64(static_cast<volatile LONG64*>(cell), 1);
    return 0;
}

// The value of the queue's first logged Signal (slot 14) on `fence` since g_queue_call_count was reset, or 0.
std::uint64_t logged_signal(const Mock* queue, const Mock* fence) {
    for (unsigned i = 0; i < g_queue_call_count; ++i)
        if (g_queue_calls[i].queue == queue && g_queue_calls[i].slot == 14 && g_queue_calls[i].a1 == word(fence))
            return g_queue_calls[i].a2;
    return 0;
}

DWORD WINAPI tear_reader(LPVOID) {
    while (!g_tear.stop.load(std::memory_order_relaxed)) {
        const std::uint64_t v = *reinterpret_cast<volatile std::uint64_t*>(&g_tear.cell);
        if (v != 0 && v != ~0ull) g_tear.torn.fetch_add(1, std::memory_order_relaxed);
        g_tear.reads.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

// Queue slot 31 (CopyPageMappingsBatchX) as the list-values tests see it: the hook records the last call and the
// first batch, so a test can tell a remap of the proxy's copy (deferred behind queue work) from the caller's array.
struct PageMappingRecord {
    int count = 0;
    const rt::xmem::PageBatch* batches = nullptr;
    std::uintptr_t pool = 0;
    std::uint32_t pages = 0, flags = 0;
    std::uintptr_t first_destination = 0;
    std::uint32_t first_count = 0;
    rt::xmem::PageRange first_ranges[4]{};
};
static PageMappingRecord g_page_mapping;

static bool record_page_mapping(std::uint32_t count, const rt::xmem::PageBatch* batches, std::uintptr_t pool,
                                std::uint32_t pages, std::uint32_t flags) {
    ++g_page_mapping.count;
    g_page_mapping.batches = batches;
    g_page_mapping.pool = pool;
    g_page_mapping.pages = pages;
    g_page_mapping.flags = flags;
    if (count && batches) {
        g_page_mapping.first_destination = batches[0].destination;
        g_page_mapping.first_count = batches[0].count;
        for (std::uint32_t i = 0; i < batches[0].count && i < 4 && batches[0].ranges; ++i)
            g_page_mapping.first_ranges[i] = batches[0].ranges[i];
    }
    return true;
}

int check_list_values_more() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, true);
    EXPECT(device);
    mock->zero_fences = true;
    xs::reset_list_value_stats();
    xs::set_list_value_wait_timeout_ms(5000);
    void* qa = make_queue(device);
    void* qb = make_queue(device);
    void* qc = make_queue(device);
    EXPECT(qa && qb && qc);
    Mock* const qa_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(qa));
    Mock* const qb_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(qb));
    Mock* const qc_mock = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(qc));
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, 0, word(&allocator_iid), word(&allocator))) == S_OK);
    std::memset(g_cells2, 0, sizeof(g_cells2));

    // 1. LEONIDA_LIST_VALUES=0 (for the lists made meanwhile): writes and waits are the no-ops they were, nothing is
    // cut, and the submission is the plain one.
    xs::set_list_values_enabled(false);
    Mock* off_real = nullptr;
    void* off = make_console_list(device, allocator, mock, &off_real);
    EXPECT(off && hr_of(raw(off, 9)) == S_OK);
    xs::set_list_values_enabled(true);
    EXPECT(hr_of(raw(off, 139, word(allocator), 0)) == S_OK);
    raw(off, 25, 0x1234);
    raw(off, 14, 1, 1, 1);
    raw(off, 74, word(&g_cells2[0]), 7, 1, 1);  // unmet, after work: a modelled list would be cut here
    raw(off, 72, word(&g_cells2[1]), 5, 0, 0);
    EXPECT(hr_of(raw(off, 9)) == S_OK && xs::list_proxy_part_count(off) == 1);
    xs::ListValueStats stats = xs::list_value_stats();
    EXPECT(stats.waits_recorded == 0 && stats.writes_recorded == 0 && stats.cuts == 0);
    g_queue_call_count = 0;
    EXPECT(submit(qa, off) == 0);
    EXPECT(g_queue_call_count == 1 && queue_call_is(0, qa_mock, 10, 1, word(off_real)));
    EXPECT(xs::list_values_idle(2000) && cell2(1) == 0);

    // 2. A bundle's writes and waits are no-ops too (a cut bundle would lose its first parts: ExecuteBundle names
    // one real list), but what it sets stays set in the list that executes it, and a cut there replays it.
    void* bundle = nullptr;
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, word(allocator), 0, word(&gcl_iid),
                     word(&bundle))) == S_OK);
    Mock* const bundle_real = mock->last_child;
    raw(bundle, 25, 0xB0B);       // SetPipelineState
    raw(bundle, 33, 0x5678);      // SetComputeRootSignature: the executing list's, whose arguments stay
    raw(bundle, 37, 1, 0x99, 0);  // SetComputeRoot32BitConstant(1, 0x99, offset 0)
    raw(bundle, 14, 1, 1, 1);
    raw(bundle, 74, word(&g_cells2[0]), 7, 1, 1);
    raw(bundle, 72, word(&g_cells2[1]), 5, 0, 0);
    EXPECT(hr_of(raw(bundle, 9)) == S_OK && xs::list_proxy_part_count(bundle) == 1);
    stats = xs::list_value_stats();
    EXPECT(stats.bundle_ops == 2 && stats.waits_recorded == 0 && stats.writes_recorded == 0 && stats.cuts == 0);
    Mock* direct_real = nullptr;
    void* direct = make_console_list(device, allocator, mock, &direct_real);
    EXPECT(direct && hr_of(raw(direct, 9)) == S_OK);
    EXPECT(hr_of(raw(direct, 139, word(allocator), 0)) == S_OK);
    raw(direct, 33, 0x5678);
    raw(direct, 35, 2, 0x9000);  // a table the bundle leaves alone
    const std::uint32_t constants[3] = {0xc1, 0xc2, 0xc3};
    raw(direct, 39, 3, 3, word(constants), 1);  // SetComputeRoot32BitConstants(3, 3 values, offset 1) -> PC 35
    raw(direct, 37, 3, 0xc4, 4);                 // one more right after them: one run of four
    raw(direct, 31, word(bundle));               // ExecuteBundle -> PC 27, with the bundle's real list
    EXPECT(direct_real->calls[27] == 1 && direct_real->args[27][0] == word(bundle_real));
    const unsigned creates = mock->calls[12];
    raw(direct, 74, word(&g_cells2[0]), 7, 1, 1);  // after the bundle's work: a cut
    EXPECT(xs::list_proxy_part_count(direct) == 2 && mock->calls[12] == creates + 1);
    Mock* const spare = mock->last_child;
    EXPECT(spare->calls[25] == 1 && spare->args[25][0] == 0xB0B);
    EXPECT(spare->calls[29] == 1 && spare->args[29][0] == 0x5678);
    EXPECT(spare->calls[31] == 1 && spare->args[31][0] == 2 && spare->args[31][1] == 0x9000);
    EXPECT(spare->calls[33] == 1 && spare->args[33][0] == 1 && spare->args[33][1] == 0x99 && spare->args[33][2] == 0);
    // (UINT arguments: only the low halves of their registers are defined)
    EXPECT(spare->calls[35] == 1 && std::uint32_t(spare->args[35][0]) == 3 && std::uint32_t(spare->args[35][1]) == 4 &&
           std::uint32_t(spare->args[35][3]) == 1);
    const auto* run = reinterpret_cast<const std::uint32_t*>(spare->args[35][2]);
    EXPECT(run && run[0] == 0xc1 && run[1] == 0xc2 && run[2] == 0xc3 && run[3] == 0xc4);
    EXPECT(hr_of(raw(direct, 9)) == S_OK);

    // 3. ClearState(pipeline) forgets what was set before it and sets its pipeline state.
    EXPECT(hr_of(raw(direct, 139, word(allocator), 0)) == S_OK);
    raw(direct, 25, 0x1234);
    raw(direct, 33, 0x5678);
    raw(direct, 11, 0x999);  // ClearState -> PC 11
    raw(direct, 14, 1, 1, 1);
    const unsigned spare_resets = spare->calls[10], spare_pipelines = spare->calls[25];
    const unsigned spare_signatures = spare->calls[29], spare_constants = spare->calls[35];
    raw(direct, 74, word(&g_cells2[0]), 7, 1, 1);  // a cut into the same spare
    EXPECT(xs::list_proxy_part_count(direct) == 2 && spare->calls[10] == spare_resets + 1);
    EXPECT(spare->calls[25] == spare_pipelines + 1 && spare->args[25][0] == 0x999);
    EXPECT(spare->calls[29] == spare_signatures && spare->calls[35] == spare_constants);
    EXPECT(hr_of(raw(direct, 9)) == S_OK);

    // 4. Writes land in submission order across queues. B's list waits for A's tag (relayed onto A's write fence)
    // and then writes the next tag to the same cell: the GPU runs B's work after A's, so once B's fence is done A's
    // is too. A pump that read A's fence before B's could see B done and A not, and land A's older tag last.
    EXPECT(hr_of(raw(direct, 139, word(allocator), 0)) == S_OK);
    raw(direct, 14, 1, 1, 1);
    raw(direct, 72, word(&g_cells2[2]), 1, 0, 0);    // the tag
    raw(direct, 72, word(&g_cells2[3]), 0xA, 0, 0);  // and a result next to it
    EXPECT(hr_of(raw(direct, 9)) == S_OK);
    EXPECT(submit(qa, direct) == 0);
    Mock* const a_written = mock->last_child;  // qa's value fences are made at this submission
    EXPECT(a_written->kind == Kind::fence && fence_completed(a_written) == 0);
    Mock* lb_real = nullptr;
    void* lb = make_console_list(device, allocator, mock, &lb_real);
    EXPECT(lb && hr_of(raw(lb, 9)) == S_OK);
    EXPECT(hr_of(raw(lb, 139, word(allocator), 0)) == S_OK);
    raw(lb, 74, word(&g_cells2[2]), 7, 1, 1);  // wait64(tag >= 1)
    raw(lb, 14, 1, 1, 1);
    raw(lb, 72, word(&g_cells2[2]), 2, 0, 0);  // the next tag
    EXPECT(hr_of(raw(lb, 9)) == S_OK);
    g_queue_call_count = 0;
    EXPECT(submit(qb, lb) == 0);
    Mock* const b_written = mock->last_child;
    Mock* const b_gate = b_written - 1;
    EXPECT(b_written->kind == Kind::fence && b_gate->kind == Kind::fence);
    EXPECT(queue_call_is(0, qb_mock, 15, word(a_written), 1) && xs::list_value_stats().waits_relayed == 1);
    EXPECT(!xs::list_values_idle(30) && cell2(2) == 0);
    b_written->completes_with = a_written;  // the first look at B's fence once it is done finds A done as well
    b_written->completes_at = 1;
    b_written->completes_value = 1;
    fence_signal(b_written, 1);
    EXPECT(xs::list_values_idle(2000) && fence_completed(a_written) == 1);
    EXPECT(cell2(2) == 2 && cell2(3) == 0xA);

    // 5. Four gates in a row that time out give the short timeout to every wait whose address was never met (a new
    // address each time here, so none times out three times on its own). A wait cut short and met in the rest of the
    // full timeout keeps the full timeout for its address only; a gate cut short and met within its short timeout
    // gives every wait the full timeout again.
    xs::set_list_value_wait_timeout_ms(100);
    xs::reset_list_value_stats();
    const auto hold_on = [&](void* queue, int index, std::uint64_t reference) -> bool {
        if (FAILED(hr_of(raw(lb, 139, word(allocator), 0)))) return false;
        raw(lb, 74, word(&g_cells2[index]), 7, reference, 1);  // wait64(cell >= reference)
        raw(lb, 14, 1, 1, 1);
        return SUCCEEDED(hr_of(raw(lb, 9))) && submit(queue, lb) == 0;
    };
    const auto hold = [&](int index) { return hold_on(qb, index, 1); };  // a cell nothing writes
    using Clock = std::chrono::steady_clock;
    const auto ms_since = [](Clock::time_point since) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
    };
    const auto set_cell = [](int index, std::uint64_t value) {
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_cells2[index]), static_cast<LONG64>(value));
    };
    for (int i = 0; i < 4; ++i) EXPECT(hold(4 + i) && xs::list_values_idle(3000));
    stats = xs::list_value_stats();
    EXPECT(stats.gates_held == 4 && stats.gates_timed_out == 4 && stats.demotions == 1 && stats.gates_short == 0);
    Clock::time_point started = Clock::now();
    EXPECT(hold(8) && xs::list_values_idle(3000) && ms_since(started) < 60);  // a new address: cut short
    stats = xs::list_value_stats();
    EXPECT(stats.gates_short == 1 && stats.gates_timed_out == 5 && stats.late_met == 0);
    set_cell(8, 1);  // late, within the full timeout
    for (int i = 0; i < 2000 && xs::list_value_stats().late_met == 0; ++i) Sleep(1);
    EXPECT(xs::list_value_stats().late_met == 1);
    started = Clock::now();
    EXPECT(hold(9) && xs::list_values_idle(3000) && ms_since(started) < 60);  // still demoted for a new address
    stats = xs::list_value_stats();
    EXPECT(stats.gates_short == 2 && stats.gates_timed_out == 6 && stats.demotions == 1);
    // Cell 8 was met before: it keeps the full timeout (met here after 30 ms, within it), and the demotion stays.
    EXPECT(hold_on(qb, 8, 2) && !xs::list_values_idle(30));
    set_cell(8, 2);
    EXPECT(xs::list_values_idle(3000));
    stats = xs::list_value_stats();
    EXPECT(stats.gates_short == 2 && stats.gates_met == 1 && stats.gates_timed_out == 6);
    started = Clock::now();
    EXPECT(hold(10) && xs::list_values_idle(3000) && ms_since(started) < 60);
    EXPECT(xs::list_value_stats().gates_short == 3);
    // A gate cut short and met within its short timeout lifts the demotion (the write follows the submission at
    // once; a slow machine may need another try).
    bool lifted = false;
    for (int i = 0; i < 3 && !lifted; ++i) {
        const std::uint64_t met_before = xs::list_value_stats().gates_met;
        EXPECT(hold(11 + i));
        set_cell(11 + i, 1);
        EXPECT(xs::list_values_idle(3000));
        lifted = xs::list_value_stats().gates_met == met_before + 1;
    }
    EXPECT(lifted);
    const std::uint64_t short_before = xs::list_value_stats().gates_short;
    started = Clock::now();
    EXPECT(hold(14) && !xs::list_values_idle(40));  // a new address, the full timeout again
    EXPECT(xs::list_values_idle(3000) && ms_since(started) >= 90);
    EXPECT(xs::list_value_stats().gates_short == short_before && xs::list_value_stats().demotions == 1);

    // 6. CopyPageMappingsBatchX behind work the queue has not done: the call returns, and the remap (of a copy of
    // the batches) runs once the queue gets there, before the gate its later work waits on opens. With the earlier
    // work done it remaps at the call.
    xs::set_copy_page_mappings_hook(&record_page_mapping);
    g_page_mapping = {};
    const rt::xmem::PageRange remap_ranges[] = {{0, 4, 2}, {2, 0, 6}};
    const rt::xmem::PageBatch remap[] = {{0x9000000000, 2, remap_ranges}};
    g_queue_call_count = 0;
    started = Clock::now();
    EXPECT(hr_of(raw(qc, 31, 1, word(remap), 0x7000000000, 16, 0)) == S_OK);
    const auto deferred_after = ms_since(started);
    EXPECT(deferred_after >= 15 && deferred_after < 100);  // the 16 ms bound, on a high-resolution timer
    Mock* const c_written = mock->last_child;
    Mock* const c_gate = c_written - 1;
    EXPECT(c_written->kind == Kind::fence && c_gate->kind == Kind::fence && g_page_mapping.count == 0);
    EXPECT(g_queue_call_count == 2 && queue_call_is(0, qc_mock, 14, word(c_written), 1) &&
           queue_call_is(1, qc_mock, 15, word(c_gate), 1));
    EXPECT(!xs::list_values_idle(30) && fence_completed(c_gate) == 0 && g_page_mapping.count == 0);
    // A memory wait behind the remap on the same queue: its time starts when the remap's gate opens, not at its
    // submission (held 150 ms behind the remap, it does not open at once then).
    const std::uint64_t timed_out_before = xs::list_value_stats().gates_timed_out;
    EXPECT(hold_on(qc, 15, 1));
    Sleep(150);
    EXPECT(fence_completed(c_gate) == 0 && xs::list_value_stats().gates_timed_out == timed_out_before);
    fence_signal(c_written, 1);
    EXPECT(!xs::list_values_idle(40) && fence_completed(c_gate) == 1);
    EXPECT(xs::list_values_idle(3000) && fence_completed(c_gate) == 2);
    EXPECT(xs::list_value_stats().gates_timed_out == timed_out_before + 1);
    EXPECT(g_page_mapping.count == 1 && g_page_mapping.batches != remap && g_page_mapping.pool == 0x7000000000 &&
           g_page_mapping.pages == 16 && g_page_mapping.first_destination == 0x9000000000 &&
           g_page_mapping.first_count == 2 && g_page_mapping.first_ranges[0].start == 4 &&
           g_page_mapping.first_ranges[1].kind == 2 && g_page_mapping.first_ranges[1].count == 6);
    stats = xs::list_value_stats();
    EXPECT(stats.remaps_deferred == 1 && stats.remaps_now == 0 && stats.remaps_failed == 0);
    qc_mock->signals_complete = true;  // the queue is idle: its Signal is done at once
    g_page_mapping = {};
    g_queue_call_count = 0;
    EXPECT(hr_of(raw(qc, 31, 1, word(remap), 0x7000000000, 16, 0)) == S_OK);
    EXPECT(g_page_mapping.count == 1 && g_page_mapping.batches == remap && g_queue_call_count == 1);
    stats = xs::list_value_stats();
    EXPECT(stats.remaps_deferred == 1 && stats.remaps_now == 1);
    qc_mock->signals_complete = false;
    xs::set_copy_page_mappings_hook(nullptr);
    xs::set_list_value_wait_timeout_ms(5000);

    // 7. Value stores: an aligned 4- or 8-byte value in one locked access (a reader polling it never sees half of
    // each), an unaligned one byte by byte, memory that cannot be written reported, not raised.
    alignas(8) static std::uint8_t bytes[32] = {};
    EXPECT(leonida::kernel_writes::store(bytes, 0x1122334455667788ull, 8));
    EXPECT(*reinterpret_cast<std::uint64_t*>(bytes) == 0x1122334455667788ull);
    EXPECT(leonida::kernel_writes::store(bytes + 8, 0xaabbccdd11223344ull, 4));
    EXPECT(*reinterpret_cast<std::uint32_t*>(bytes + 8) == 0x11223344u && bytes[12] == 0);
    EXPECT(leonida::kernel_writes::store(bytes + 17, 0x0102030405060708ull, 8));
    EXPECT(bytes[17] == 0x08 && bytes[24] == 0x01 && bytes[16] == 0 && bytes[25] == 0);
    void* read_only = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    EXPECT(read_only && !leonida::kernel_writes::store(read_only, 1, 8) &&
           !leonida::kernel_writes::store(static_cast<std::uint8_t*>(read_only) + 1, 1, 8));
    VirtualFree(read_only, 0, MEM_RELEASE);
    const HANDLE reader = CreateThread(nullptr, 0, &tear_reader, nullptr, 0, nullptr);
    EXPECT(reader);
    for (int i = 0; i < 200000; ++i) leonida::kernel_writes::store(&g_tear.cell, (i & 1) ? ~0ull : 0, 8);
    g_tear.stop = true;
    WaitForSingleObject(reader, INFINITE);
    CloseHandle(reader);
    EXPECT(g_tear.torn == 0 && g_tear.reads > 0);

    // 9. write64 flags 1 and 2 take the GPU's timestamp at the write's place in its list (EndQuery and a resolve of
    // that one slot into the readback), landed once the part's fence passed: flag 1 in the queue's timestamp ticks (a
    // begin/end pair is the GPU work's, not one landing time), flag 2 as the QPC value the queue's clock calibration
    // gives. A pre-write that lands before its part ran (the gate is held) takes the CPU's clock instead.
    xs::reset_list_value_stats();
    xs::set_list_value_wait_timeout_ms(5000);
    EXPECT(hr_of(raw(direct, 139, word(allocator), 0)) == S_OK);
    const unsigned end_queries = direct_real->calls[53], resolves = direct_real->calls[54];
    raw(direct, 72, word(&g_cells2[20]), 0, 0, 1);  // begin
    raw(direct, 14, 1, 1, 1);
    raw(direct, 72, word(&g_cells2[21]), 0, 0, 1);  // end
    raw(direct, 72, word(&g_cells2[22]), 0, 0, 2);  // the system's timestamp
    EXPECT(hr_of(raw(direct, 9)) == S_OK);
    EXPECT(direct_real->calls[53] == end_queries + 3 && direct_real->calls[54] == resolves + 3);
    const auto last = static_cast<std::uint32_t>(direct_real->args[53][2]);  // EndQuery(heap, type, slot)
    EXPECT(std::uint32_t(direct_real->args[53][1]) == D3D12_QUERY_TYPE_TIMESTAMP && last >= 2 && last < 4096);
    EXPECT(direct_real->args[54][0] == direct_real->args[53][0] &&  // ResolveQueryData(heap, type, first, count,
           std::uint32_t(direct_real->args[54][2]) == last &&      //                  readback, offset)
           std::uint32_t(direct_real->args[54][3]) == 1 && direct_real->args[54][5] == std::uint64_t{last} * 8);
    g_queue_call_count = 0;
    EXPECT(submit(qa, direct) == 0);
    const std::uint64_t stamped = logged_signal(qa_mock, a_written);
    EXPECT(stamped && cell2(20) == 0 && cell2(21) == 0);
    g_mock_readback[last - 2] = kMockGpu0 + 1000;  // as the GPU resolves them: 1 MHz ticks
    g_mock_readback[last - 1] = kMockGpu0 + 4000;  // 3 ms of GPU work later
    g_mock_readback[last] = kMockGpu0 - 2000;      // 2 ms before the calibration point
    fence_signal(a_written, stamped);
    EXPECT(xs::list_values_idle(2000));
    EXPECT(cell2(20) == kMockGpu0 + 1000 && cell2(21) == kMockGpu0 + 4000);
    LARGE_INTEGER qpc_frequency{}, qpc_now{};
    QueryPerformanceFrequency(&qpc_frequency);
    QueryPerformanceCounter(&qpc_now);
    EXPECT(cell2(22) == mock_cpu_origin() - 2000 * static_cast<std::uint64_t>(qpc_frequency.QuadPart) / 1000000 &&
           cell2(22) <= static_cast<std::uint64_t>(qpc_now.QuadPart));
    stats = xs::list_value_stats();
    EXPECT(stats.stamps_gpu == 3 && stats.stamps_cpu == 0);
    EXPECT(hr_of(raw(direct, 139, word(allocator), 0)) == S_OK);
    raw(direct, 72, word(&g_cells2[23]), 0, 0, 1);  // a timestamp, then a wait: a pre-write
    raw(direct, 74, word(&g_cells2[24]), 7, 1, 1);  // unmet: the gate holds the part
    raw(direct, 14, 1, 1, 1);
    EXPECT(hr_of(raw(direct, 9)) == S_OK);
    const auto pre_slot = static_cast<std::uint32_t>(direct_real->args[53][2]);
    g_queue_call_count = 0;
    EXPECT(submit(qa, direct) == 0);
    const std::uint64_t pre = logged_signal(qa_mock, a_written);
    // The pre-write's group is signalled, then the queue stands at the gate, then the part.
    EXPECT(pre && g_queue_call_count == 3 && queue_call_is(0, qa_mock, 14, word(a_written), pre) &&
           g_queue_calls[1].slot == 15 && g_queue_calls[1].a1 == word(a_written - 1));
    g_mock_readback[pre_slot] = 77;  // what the part writes once it runs: not there when the pre-write lands
    fence_signal(a_written, pre);
    for (int i = 0; i < 2000 && cell2(23) == 0; ++i) Sleep(1);
    EXPECT(cell2(23) != 0 && cell2(23) != 77 && xs::list_value_stats().stamps_cpu == 1);
    set_cell(24, 1);
    EXPECT(xs::list_values_idle(3000));

    // 10. A GPU wait for a label a CPU job of the title's writes later never blocks the submitting thread: the
    // submission returns at once with the gate held (with a 5 s timeout), a submission behind it on the same queue
    // returns at once too, and the CPU job's write (on another thread, 100 ms later) opens the gate.
    const std::uint64_t met_before = xs::list_value_stats().gates_met;
    started = Clock::now();
    EXPECT(hold(25));
    const HANDLE job = CreateThread(nullptr, 0, &label_job, &g_cells2[25], 0, nullptr);
    EXPECT(job);
    EXPECT(hold_on(qb, 26, 0) && ms_since(started) < 50);  // a met wait: the plain way, behind the held gate
    EXPECT(!xs::list_values_idle(40) && xs::list_value_stats().gates_met == met_before);
    EXPECT(xs::list_values_idle(3000) && xs::list_value_stats().gates_met == met_before + 1 &&
           ms_since(started) >= 90);
    WaitForSingleObject(job, INFINITE);
    CloseHandle(job);

    // 11. Work-aware timeout in Pump (src/list_value_wait_budget.h):
    //   a) a matching in-flight producer pauses a held memory gate past the idle timeout until the producer lands;
    //   b) a busy releaser executing an action gate pauses a concurrent memory gate past the idle timeout;
    //   c) a demoted gate (4 ms budget) scales its pause caps with its short budget so unrelated active work does not
    //      hold it for the full timeout's cap;
    //   d) a matching producer stuck behind an unsignalled fence still hits a finite pause cap instead of hanging.
    const auto write_on_a = [&](int index, std::uint64_t value, std::uint64_t& signal_value) -> bool {
        if (FAILED(hr_of(raw(direct, 139, word(allocator), 0)))) return false;
        raw(direct, 14, 1, 1, 1);  // Dispatch
        raw(direct, 72, word(&g_cells2[index]), value, 0, 0);  // write64
        if (FAILED(hr_of(raw(direct, 9)))) return false;
        g_queue_call_count = 0;
        if (submit(qa, direct) != 0) return false;
        signal_value = logged_signal(qa_mock, a_written);
        return signal_value != 0;
    };
    xs::set_list_value_wait_timeout_ms(40);
    xs::reset_list_value_stats();
    // 11a. Wait on qb before qa submits its matching write (so relay() misses it and Pump holds a gate).
    EXPECT(hold_on(qb, 27, 1));
    std::uint64_t producer_sig = 0;
    EXPECT(write_on_a(27, 1, producer_sig));
    EXPECT(!xs::list_values_idle(150));  // >3x the 40 ms idle timeout: pauses while producer is in flight
    stats = xs::list_value_stats();
    EXPECT(stats.gates_timed_out == 0 && stats.gates_met == 0 && cell2(27) == 0);
    fence_signal(a_written, producer_sig);
    EXPECT(xs::list_values_idle(3000) && cell2(27) == 1);
    stats = xs::list_value_stats();
    EXPECT(stats.gates_met == 1 && stats.gates_timed_out == 0);

    // 11b. Releaser inside a 150 ms action gate pauses a concurrent memory gate past the 40 ms idle timeout.
    xs::set_copy_page_mappings_hook([](std::uint32_t, const rt::xmem::PageBatch*, std::uintptr_t, std::uint32_t,
                                       std::uint32_t) -> bool {
        Sleep(150);
        InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&g_cells2[28]), 1);
        return true;
    });
    g_queue_call_count = 0;
    EXPECT(hr_of(raw(qc, 31, 1, word(remap), 0x7000000000, 16, 0)) == S_OK);
    const std::uint64_t action_sig = logged_signal(qc_mock, c_written);
    EXPECT(action_sig != 0);
    const std::uint64_t met_before_rel = xs::list_value_stats().gates_met;
    const std::uint64_t to_before_rel = xs::list_value_stats().gates_timed_out;
    EXPECT(hold_on(qb, 28, 1));
    fence_signal(c_written, action_sig);
    EXPECT(xs::list_values_idle(3000) && cell2(28) == 1);
    xs::set_copy_page_mappings_hook(nullptr);
    stats = xs::list_value_stats();
    EXPECT(stats.gates_met == met_before_rel + 1 && stats.gates_timed_out == to_before_rel);

    // 11c. Demoted gate (3 timeouts -> 4 ms budget) with unrelated in-flight work on qa times out in a few ms, not
    // the 12 s active-work cap of the 40 ms full timeout.
    for (int i = 0; i < 3; ++i) EXPECT(hold_on(qb, 29, 1) && xs::list_values_idle(3000));
    std::uint64_t unrelated_sig = 0;
    EXPECT(write_on_a(30, 1, unrelated_sig));
    const std::uint64_t short_before_dem = xs::list_value_stats().gates_short;
    const std::uint64_t to_before_dem = xs::list_value_stats().gates_timed_out;
    started = Clock::now();
    EXPECT(hold_on(qb, 29, 1));
    for (int i = 0; i < 1000 && xs::list_value_stats().gates_timed_out == to_before_dem; ++i) Sleep(1);
    const auto demoted_active_ms = ms_since(started);
    EXPECT(xs::list_value_stats().gates_timed_out == to_before_dem + 1 &&
           xs::list_value_stats().gates_short == short_before_dem + 1 && demoted_active_ms < 500);
    fence_signal(a_written, unrelated_sig);
    EXPECT(xs::list_values_idle(3000));

    // 11d. Stuck matching producer (fence never signals) still hits the finite max pause cap instead of hanging.
    const std::uint64_t to_before_stuck = xs::list_value_stats().gates_timed_out;
    started = Clock::now();
    EXPECT(hold_on(qb, 29, 1));
    std::uint64_t stuck_sig = 0;
    EXPECT(write_on_a(29, 1, stuck_sig));
    for (int i = 0; i < 1000 && xs::list_value_stats().gates_timed_out == to_before_stuck; ++i) Sleep(1);
    const auto stuck_producer_ms = ms_since(started);
    EXPECT(xs::list_value_stats().gates_timed_out == to_before_stuck + 1 && stuck_producer_ms < 500);
    fence_signal(a_written, stuck_sig);
    EXPECT(xs::list_values_idle(3000));
    xs::set_list_value_wait_timeout_ms(5000);

    // 8. stop() ends both threads. Afterwards nothing new is landed or opened (the process is ending: a gate
    // signalled from another thread could open one whose remap was skipped, or go below one still to come), and
    // CopyPageMappingsBatchX remaps at the call rather than wait for a queue whose gates stay shut.
    xs::stop_list_values(2000);
    const std::uint64_t gates_before = fence_completed(b_gate);
    EXPECT(hold(16));
    Sleep(20);
    EXPECT(fence_completed(b_gate) == gates_before);
    xs::set_copy_page_mappings_hook(&record_page_mapping);
    g_page_mapping = {};
    started = Clock::now();
    EXPECT(hr_of(raw(qc, 31, 1, word(remap), 0x7000000000, 16, 0)) == S_OK);  // its point is never reached
    EXPECT(g_page_mapping.count == 1 && g_page_mapping.batches == remap && ms_since(started) < 1000);
    xs::set_copy_page_mappings_hook(nullptr);

    EXPECT(release(off) == 0 && off_real->refs == 0);
    EXPECT(release(bundle) == 0 && bundle_real->refs == 0);
    EXPECT(release(direct) == 0 && direct_real->refs == 0 && spare->refs == 0);
    EXPECT(release(lb) == 0 && lb_real->refs == 0);
    EXPECT(release(allocator) == 0);
    EXPECT(release(qa) == 0 && release(qb) == 0 && release(qc) == 0);
    EXPECT(qa_mock->refs == 0 && qb_mock->refs == 0 && qc_mock->refs == 0);
    EXPECT(a_written->refs == 0 && b_written->refs == 0 && b_gate->refs == 0 && c_written->refs == 0 &&
           c_gate->refs == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    std::printf("%s: with LEONIDA_LIST_VALUES=0 and on bundles writes and waits are no-ops, a bundle's and "
                "ClearState's state carry over a cut, writes land in submission order across queues, timeouts in a "
                "row shorten the waits for addresses never met until one cut short is met, a gate's time starts at "
                "its queue's head, a page remap behind held work is deferred, value stores do not tear, and nothing "
                "is opened after stop(); timestamp writes land the GPU's timestamps, and a wait for a label a CPU job "
                "writes later never blocks the submitting thread\n",
                g_label);
    return 0;
}

// A failure ends the process with a message on stderr, never in a dialog a test run would wait on: abort() (an
// exception that escapes, for one), the debug CRT's reports, and Windows' fault and critical-error boxes.
static void no_dialogs() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG  // the report functions exist only in the debug CRT (empty macros otherwise)
    for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(type, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
    }
#endif
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
}

int check_console_command_list() {
    // 1. Slot mapping checks
    EXPECT(xs::console_to_pc_list_slot(14) == 14);   // Dispatch
    EXPECT(xs::console_to_pc_list_slot(26) == 26);   // ResourceBarrier
    EXPECT(xs::console_to_pc_list_slot(31) == 27);   // ExecuteBundle
    EXPECT(xs::console_to_pc_list_slot(32) == 28);   // SetDescriptorHeaps
    EXPECT(xs::console_to_pc_list_slot(33) == 29);   // SetComputeRootSignature
    EXPECT(xs::console_to_pc_list_slot(35) == 31);   // SetComputeRootDescriptorTable
    EXPECT(xs::console_to_pc_list_slot(60) == 58);   // EndEvent
    EXPECT(xs::console_to_pc_list_slot(61) == 59);   // ExecuteIndirect
    EXPECT(xs::console_to_pc_list_slot(85) == 16);   // CopyTextureRegion
    EXPECT(xs::console_to_pc_list_slot(139) == 10);  // Reset, as the retail title calls it
    EXPECT(xs::console_to_pc_list_slot(146) == 80);  // GCL7 Barrier
    EXPECT(xs::console_to_pc_list_slot(148) == xs::kListNotForwarded); // AllocateUploadMemory

    xs::reset_device_proxy_stats();  // the list slot counts below start from this test
    Mock* mock_device = nullptr;
    void* device = make_device(mock_device, 11, true);
    EXPECT(device != nullptr);

    // 2. Slot 12 and Xbox slot 73. On a device proxy made for console callers (console_lists, as make_device does),
    // slot 12 gives the console list proxy too: the merge into main kept the boot stack's slot 12 (main's form
    // returned the unwrapped PC list here). PC callers get PC lists from a proxy made with console_lists=false
    // (check_lists covers both).
    void* pc_list = nullptr;
    const IID list_iid = __uuidof(ID3D12GraphicsCommandList);
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, word(&list_iid), word(&pc_list))) == S_OK);
    EXPECT(pc_list != nullptr);
    EXPECT(xs::is_command_list_proxy(pc_list));
    EXPECT(release(pc_list) == 0);

    // Xbox Slot 73 returns wrapped console command list proxy
    void* allocator = nullptr;
    const IID alloc_iid = __uuidof(ID3D12CommandAllocator);
    EXPECT(hr_of(raw(device, 9, 0, word(&alloc_iid), word(&allocator))) == S_OK);
    EXPECT(xs::is_allocator_proxy(allocator));
    Mock* real_allocator = reinterpret_cast<Mock*>(xs::allocator_proxy_real_allocator(allocator));

    struct XboxCommandListDesc {
        std::uint32_t type;
        std::uint32_t node_mask;
        std::uint64_t reserved[2];
    } desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, {0, 0}};

    void* list_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list_proxy));
    EXPECT(xs::command_list_proxy_table_size() == 160);

    // 3. Inline cursor pinning: +0x18, +0x28, +0x30
    const auto base = reinterpret_cast<std::uintptr_t>(list_proxy);
    std::uint64_t c18 = 0, c28 = 0, c30 = 0;
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    std::memcpy(&c28, reinterpret_cast<const void*>(base + 0x28), sizeof(c28));
    std::memcpy(&c30, reinterpret_cast<const void*>(base + 0x30), sizeof(c30));
    const std::uint64_t expected_limit = static_cast<std::uint64_t>(base + 0x18 + 0x500);
    EXPECT(c18 == expected_limit);
    EXPECT(c28 == expected_limit);
    EXPECT(c30 == expected_limit);

    // 4. Console slot routing on ListProxy
    Mock* real_list = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list_proxy));
    EXPECT(real_list != nullptr && real_list->kind == Kind::command_list);

    // Console slot 32 (SetDescriptorHeaps) -> PC slot 28
    // Pass descriptor heap proxy to ensure UnwrappedArray unwrap_object works
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 16, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* heap_proxy = nullptr;
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    EXPECT(hr_of(raw(device, 14, word(&heap_desc), word(&heap_iid), word(&heap_proxy))) == S_OK);
    EXPECT(heap_proxy != nullptr && xs::is_heap_proxy(heap_proxy));
    ID3D12DescriptorHeap* real_heap = xs::heap_proxy_real_heap(heap_proxy);
    void* heap_ptrs[2] = {heap_proxy, real_heap};
    raw(list_proxy, 32, 2, word(heap_ptrs), 0, 0);
    EXPECT(real_list->calls[28] == 1);
    EXPECT(real_list->args[28][0] == 2);
    // The heap proxy should have been unwrapped in the array passed to PC slot 28:
    EXPECT(real_list->array[28][0] == word(real_heap));
    EXPECT(real_list->array[28][1] == word(real_heap));
    EXPECT(xs::list_slot_hits(32) == 1);
    EXPECT(xs::list_slot_first_call_seen(32));

    // Console slot 33 (SetComputeRootSignature) -> PC slot 29
    raw(list_proxy, 33, 0x5555, 0, 0, 0);
    EXPECT(real_list->calls[29] == 1);
    EXPECT(real_list->args[29][0] == 0x5555);
    EXPECT(xs::list_slot_hits(33) == 1);
    EXPECT(xs::list_slot_first_call_seen(33));

    // Console slot 60 (EndEvent) -> PC slot 58
    raw(list_proxy, 60, 0, 0, 0, 0);
    EXPECT(real_list->calls[58] == 1);

    // Console slot 61 (ExecuteIndirect) -> PC slot 59
    raw(list_proxy, 61, 0x10, 0x20, 0x30, 0x40);
    EXPECT(real_list->calls[59] == 1);
    EXPECT(real_list->args[59][0] == 0x10);

    // Console slot 148 (AllocateUploadMemory) -> an upload-arena pointer (the boot stack's arena, which the merge
    // kept; main's stub returned 0)
    {
        const std::uint64_t upload = raw(list_proxy, 148, 2048, 512, 0, 0);
        EXPECT(upload != 0);
        EXPECT(upload % 512 == 0);
        std::memset(reinterpret_cast<void*>(upload), 0x5a, 2048);
    }
    EXPECT(xs::list_slot_hits(148) == 1);
    EXPECT(xs::list_slot_first_call_seen(148));

    // Console slot 146 (Barrier) on base 60-slot list returns E_NOTIMPL (PC slot 80 >= 60)
    EXPECT(hr_of(raw(list_proxy, 146, 0, 0, 0, 0)) == E_NOTIMPL);

    // Console slot 10 (Reset) unwraps allocator proxy and re-pins inline cursor
    std::uint64_t zero = 0;
    std::memcpy(reinterpret_cast<void*>(base + 0x18), &zero, sizeof(zero));
    EXPECT(hr_of(raw(list_proxy, 10, word(allocator), 0, 0, 0)) == S_OK);
    EXPECT(real_list->calls[10] == 1);
    EXPECT(real_list->args[10][0] == word(real_allocator));
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    EXPECT(c18 == expected_limit);

    // Console slot 139 is Reset as the retail title calls it: the same PC slot 10, allocator unwrapped, cursor
    // re-pinned, and the list's own slot 139 never reached.
    std::memcpy(reinterpret_cast<void*>(base + 0x18), &zero, sizeof(zero));
    EXPECT(hr_of(raw(list_proxy, 139, word(allocator), 0, 0, 0)) == S_OK);
    EXPECT(real_list->calls[10] == 2 && real_list->calls[139] == 0);
    EXPECT(real_list->args[10][0] == word(real_allocator) && real_list->args[10][1] == 0);
    std::memcpy(&c18, reinterpret_cast<const void*>(base + 0x18), sizeof(c18));
    EXPECT(c18 == expected_limit);

    // Console slot 7 (GetDevice) returns device proxy
    void* list_dev = nullptr;
    const IID dev_iid = __uuidof(ID3D12Device);
    const auto dev_refs_before = xs::device_proxy_ref_count(device);
    EXPECT(hr_of(raw(list_proxy, 7, word(&dev_iid), word(&list_dev))) == S_OK);
    EXPECT(list_dev == device);
    EXPECT(xs::device_proxy_ref_count(device) == dev_refs_before + 1);
    EXPECT(release(list_dev) == dev_refs_before);

    // Console slot 0 (QueryInterface)
    void* list_qi = nullptr;
    EXPECT(hr_of(raw(list_proxy, 0, word(&list_iid), word(&list_qi))) == S_OK);
    EXPECT(list_qi == list_proxy);
    EXPECT(release(list_qi) == 1);

    // 5. Queue slot 10 (ExecuteCommandLists) unwraps ListProxy in array
    void* queue = nullptr;
    D3D12_COMMAND_QUEUE_DESC qdesc{};
    const IID queue_iid = __uuidof(ID3D12CommandQueue);
    EXPECT(hr_of(raw(device, 8, word(&qdesc), word(&queue_iid), word(&queue))) == S_OK);
    EXPECT(xs::is_queue_proxy(queue));
    Mock* real_queue = reinterpret_cast<Mock*>(xs::queue_proxy_real_queue(queue));

    Mock* unwrapped_list = new_mock(Kind::command_list);
    void* lists_to_exec[2] = {unwrapped_list, list_proxy};
    raw(queue, 10, 2, word(lists_to_exec), 0, 0);
    EXPECT(real_queue->calls[10] == 1);
    EXPECT(real_queue->args[10][0] == 2);
    EXPECT(real_queue->array[10][0] == word(unwrapped_list));
    EXPECT(real_queue->array[10][1] == word(real_list));

    // 5b. Public list revisions the proxy does not model and unknown private IIDs must not trigger a retry: the PC
    // device is asked once, for ID3D12GraphicsCommandList, and the caller gets the console list proxy.
    // Spell out the Windows SDK's ID3D12GraphicsCommandList9 IID so this also builds with older headers.
    const IID list9_iid = {0x34ed2808, 0xffe6, 0x4c2b, {0xb1, 0x1a, 0xca, 0xbd, 0x2b, 0x0c, 0x59, 0xe1}};
    const IID unknown_iid = {0x12345678, 0x1234, 0x1234, {1, 2, 3, 4, 5, 6, 7, 8}};
    for (const IID* iid : {&list9_iid, &unknown_iid}) {
        const unsigned calls_before = mock_device->calls[12];
        void* unsupported_list = nullptr;
        EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(iid), word(&unsupported_list))) == S_OK);
        EXPECT(xs::is_command_list_proxy(unsupported_list) && mock_device->calls[12] == calls_before + 1);
        EXPECT(IsEqualIID(mock_device->create_riid, __uuidof(ID3D12GraphicsCommandList)));
        EXPECT(release(unsupported_list) == 0);
    }

    // The measured Xbox list IID is created as ID3D12GraphicsCommandList in one call and stays queryable on the proxy.
    const IID console_iid = {0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    const unsigned calls12_before = mock_device->calls[12];
    void* console_list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&console_iid), word(&console_list))) == S_OK);
    EXPECT(xs::is_command_list_proxy(console_list) && mock_device->calls[12] == calls12_before + 1);
    EXPECT(IsEqualIID(mock_device->create_riid, __uuidof(ID3D12GraphicsCommandList)));
    void* console_list_qi = nullptr;
    EXPECT(hr_of(raw(console_list, 0, word(&console_iid), word(&console_list_qi))) == S_OK);
    EXPECT(console_list_qi == console_list && release(console_list_qi) == 1);
    EXPECT(release(console_list) == 0);

    // The PC device refuses even ID3D12GraphicsCommandList for a PC IID: E_NOINTERFACE comes back as is, no retry.
    mock_device->list_refuse = true;
    const unsigned calls12_refused = mock_device->calls[12];
    console_list = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&console_list))) ==
           E_NOINTERFACE);
    EXPECT(!console_list && mock_device->calls[12] == calls12_refused + 1);
    mock_device->list_refuse = false;

    // 6. Test with list_level = 7 (supporting ID3D12GraphicsCommandList7, 81 slots)
    mock_device->list_level = 7;
    void* list7_proxy = nullptr;
    EXPECT(hr_of(raw(device, 73, word(&desc), word(allocator), 0, word(&list_iid), word(&list7_proxy))) == S_OK);
    EXPECT(xs::is_command_list_proxy(list7_proxy));
    Mock* real_list7 = reinterpret_cast<Mock*>(xs::command_list_proxy_real_list(list7_proxy));
    EXPECT(real_list7 != nullptr);
    // Console slot 146 -> PC slot 80 (since 80 < 81 slots): a valid group goes through with the title's pointer.
    {
        const D3D12_GLOBAL_BARRIER global{D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL,
                                          D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS};
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_GLOBAL;
        group.NumBarriers = 1;
        group.pGlobalBarriers = &global;
        raw(list7_proxy, 146, 1, word(&group), 0, 0);
        EXPECT(real_list7->calls[80] == 1);
        EXPECT(real_list7->args[80][0] == 1 && real_list7->args[80][1] == word(&group));
    }
    EXPECT(release(list7_proxy) == 0 && real_list7->refs == 0);

    // Clean up
    EXPECT(release(list_proxy) == 0 && real_list->refs == 0);
    EXPECT(release(heap_proxy) == 0);
    EXPECT(release(allocator) == 0 && real_allocator->refs == 0);
    EXPECT(release(queue) == 0 && real_queue->refs == 0);
    EXPECT(release(device) == 0 && mock_device->refs == 0);

    // 7. Verify stats reset
    EXPECT(xs::list_slot_hits(32) > 0);
    xs::reset_device_proxy_stats();
    EXPECT(xs::list_slot_hits(32) == 0);
    EXPECT(!xs::list_slot_first_call_seen(32));

    std::printf("%s: console command list routing, cursor pinning, queue unwrapping, and slot mapping passed\n", g_label);
    return 0;
}

// ---- Console list slots 146 and 52: what the console takes as a no-op or a global barrier ----
//
// The mock list stands in for the Vulkan layer's: a texture barrier with no resource, or a clear of a view made with
// no resource, puts it in its error state and its Close fails (checked directly on the mock first, so the cases
// below mean something). Through the proxy the barrier becomes a global barrier and the clear is skipped.
int check_console_null_barrier_and_clear() {
    g_mock_null_rtv_count = 0;
    Mock* mock = nullptr;
    void* device = make_device(mock, 9, true, true, /*full_chain_lists=*/true);
    EXPECT(device);
    const IID allocator_iid = __uuidof(ID3D12CommandAllocator);
    const IID gcl_iid = __uuidof(ID3D12GraphicsCommandList);
    void* allocator = nullptr;
    EXPECT(hr_of(raw(device, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&allocator))) == S_OK);
    void* list = nullptr;
    EXPECT(hr_of(raw(device, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(allocator), 0, word(&gcl_iid),
                     word(&list))) == S_OK);
    Mock* const real = reinterpret_cast<Mock*>(xs::list_proxy_real_list(list));
    EXPECT(real && real->kind == Kind::list);
    Mock* const texture_a = new_mock(Kind::resource);  // never dereferenced: only the pointers are compared
    Mock* const texture_b = new_mock(Kind::resource);
    auto* const resource_a = reinterpret_cast<ID3D12Resource*>(texture_a);
    auto* const resource_b = reinterpret_cast<ID3D12Resource*>(texture_b);

    // A texture barrier with no resource, as the title records it: render target to pixel-shader read.
    D3D12_TEXTURE_BARRIER no_resource{};
    no_resource.SyncBefore = D3D12_BARRIER_SYNC_RENDER_TARGET;
    no_resource.SyncAfter = D3D12_BARRIER_SYNC_PIXEL_SHADING;
    no_resource.AccessBefore = D3D12_BARRIER_ACCESS_RENDER_TARGET;
    no_resource.AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
    no_resource.LayoutBefore = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
    no_resource.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    no_resource.Subresources.IndexOrFirstMipLevel = 0xffffffffu;
    D3D12_BARRIER_GROUP texture_group{};
    texture_group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    texture_group.NumBarriers = 1;
    texture_group.pTextureBarriers = &no_resource;

    // The mock models the PC list: the same group sent to it directly fails its Close.
    raw(real, 80, 1, word(&texture_group));
    EXPECT(real->list_error && hr_of(raw(real, 9)) == E_INVALIDARG);
    real->list_error = false;
    real->calls[80] = 0;

    // 1. Through the proxy: one global barrier with the same sync and access, in the call's own storage; the
    //    title's group and barrier are not written; Close succeeds.
    EXPECT(hr_of(raw(list, 146, 1, word(&texture_group))) == S_OK);
    EXPECT(real->calls[80] == 1 && real->args[80][0] == 1 && real->args[80][1] != word(&texture_group));
    EXPECT(real->barrier_group_count == 1 && real->barrier_group_types[0] == D3D12_BARRIER_TYPE_GLOBAL &&
           real->barrier_group_sizes[0] == 1 && real->barrier_global_count == 1 && real->barrier_texture_count == 0);
    EXPECT(real->barrier_globals[0].SyncBefore == D3D12_BARRIER_SYNC_RENDER_TARGET &&
           real->barrier_globals[0].SyncAfter == D3D12_BARRIER_SYNC_PIXEL_SHADING &&
           real->barrier_globals[0].AccessBefore == D3D12_BARRIER_ACCESS_RENDER_TARGET &&
           real->barrier_globals[0].AccessAfter == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    EXPECT(texture_group.Type == D3D12_BARRIER_TYPE_TEXTURE && texture_group.pTextureBarriers == &no_resource &&
           !no_resource.pResource);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // 2. Valid barriers pass unchanged: a texture group with resources goes through with the title's own pointer.
    D3D12_TEXTURE_BARRIER with_resource[2] = {no_resource, no_resource};
    with_resource[0].pResource = resource_a;
    with_resource[1].pResource = resource_b;
    with_resource[1].Subresources = {2, 1, 0, 1, 0, 1};
    D3D12_BARRIER_GROUP valid_group{};
    valid_group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    valid_group.NumBarriers = 2;
    valid_group.pTextureBarriers = with_resource;
    EXPECT(hr_of(raw(list, 146, 1, word(&valid_group))) == S_OK);
    EXPECT(real->calls[80] == 2 && real->args[80][0] == 1 && real->args[80][1] == word(&valid_group));
    EXPECT(real->barrier_texture_count == 2 && real->barrier_global_count == 0 &&
           real->barrier_textures[0].pResource == resource_a && real->barrier_textures[1].pResource == resource_b &&
           real->barrier_textures[1].LayoutAfter == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE &&
           real->barrier_textures[1].Subresources.IndexOrFirstMipLevel == 2);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // 3. Mixed: a buffer group, then a texture group of [resource, none, none, resource]. The buffer group passes as
    //    it is and the texture group splits into texture(1), global(2), texture(1), in order.
    const D3D12_BUFFER_BARRIER buffer{D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COMPUTE_SHADING,
                                      D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                      resource_a, 0, UINT64_MAX};
    D3D12_TEXTURE_BARRIER mixed[4] = {with_resource[0], no_resource, no_resource, with_resource[1]};
    mixed[2].SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    mixed[2].AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    D3D12_BARRIER_GROUP mixed_groups[2]{};
    mixed_groups[0].Type = D3D12_BARRIER_TYPE_BUFFER;
    mixed_groups[0].NumBarriers = 1;
    mixed_groups[0].pBufferBarriers = &buffer;
    mixed_groups[1].Type = D3D12_BARRIER_TYPE_TEXTURE;
    mixed_groups[1].NumBarriers = 4;
    mixed_groups[1].pTextureBarriers = mixed;
    EXPECT(hr_of(raw(list, 146, 2, word(mixed_groups))) == S_OK);
    EXPECT(real->calls[80] == 3 && real->args[80][0] == 4);
    EXPECT(real->barrier_group_types[0] == D3D12_BARRIER_TYPE_BUFFER && real->barrier_group_sizes[0] == 1 &&
           real->barrier_group_types[1] == D3D12_BARRIER_TYPE_TEXTURE && real->barrier_group_sizes[1] == 1 &&
           real->barrier_group_types[2] == D3D12_BARRIER_TYPE_GLOBAL && real->barrier_group_sizes[2] == 2 &&
           real->barrier_group_types[3] == D3D12_BARRIER_TYPE_TEXTURE && real->barrier_group_sizes[3] == 1);
    EXPECT(real->barrier_texture_count == 2 && real->barrier_textures[0].pResource == resource_a &&
           real->barrier_textures[1].pResource == resource_b);
    EXPECT(real->barrier_global_count == 2 &&
           real->barrier_globals[0].SyncBefore == D3D12_BARRIER_SYNC_RENDER_TARGET &&
           real->barrier_globals[1].SyncBefore == D3D12_BARRIER_SYNC_COMPUTE_SHADING &&
           real->barrier_globals[1].AccessBefore == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS &&
           real->barrier_globals[1].AccessAfter == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // 4. Arrays that cannot be read safely are refused whole: nothing reaches the list, which stays valid and keeps
    //    taking calls.
    D3D12_BARRIER_GROUP unknown_type = texture_group;
    reinterpret_cast<std::uint32_t&>(unknown_type.Type) = 7;
    D3D12_BARRIER_GROUP null_barriers{};
    null_barriers.Type = D3D12_BARRIER_TYPE_TEXTURE;
    null_barriers.NumBarriers = 3;
    D3D12_BARRIER_GROUP huge_group = valid_group;
    huge_group.NumBarriers = 0x10000000u;
    EXPECT(hr_of(raw(list, 146, 1, 0)) == S_OK);                       // null array, nonzero count
    EXPECT(hr_of(raw(list, 146, 1, word(&unknown_type))) == S_OK);     // a group type the layer does not know
    EXPECT(hr_of(raw(list, 146, 1, word(&null_barriers))) == S_OK);    // null barrier array, nonzero count
    EXPECT(hr_of(raw(list, 146, 1, word(&huge_group))) == S_OK);       // barrier count over the limit
    EXPECT(hr_of(raw(list, 146, 0x40000000u, word(&valid_group))) == S_OK);  // group count over the limit
    // A malformed group after a convertible one refuses the call as well: nothing of it is recorded.
    D3D12_BARRIER_GROUP half_bad[2] = {texture_group, unknown_type};
    EXPECT(hr_of(raw(list, 146, 2, word(half_bad))) == S_OK);
    EXPECT(real->calls[80] == 3 && !real->list_error);
    EXPECT(hr_of(raw(list, 146, 1, word(&texture_group))) == S_OK && real->calls[80] == 4);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // 5. ClearRenderTargetView (console 52 -> PC 48) of a view made with no resource is skipped; a view made with a
    //    resource is still cleared. No custom heap range here, so the handles reach the device unchanged.
    const SIZE_T rtv_none = kMockHeapCpuStart + 0x1000;
    const SIZE_T rtv_real = rtv_none + kMockDescriptorInc;
    const SIZE_T rtv_copy = rtv_none + 4 * kMockDescriptorInc;
    const FLOAT color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    raw(device, 20, 0, 0, rtv_none);              // CreateRenderTargetView(nullptr, nullptr, handle)
    raw(device, 20, word(resource_a), 0, rtv_real);
    EXPECT(mock->calls[20] == 2 && mock_is_null_rtv(rtv_none) && !mock_is_null_rtv(rtv_real));
    raw(real, 48, rtv_none, word(color), 0, 0);   // the mock's model: the direct clear fails the list
    EXPECT(real->list_error && hr_of(raw(real, 9)) == E_INVALIDARG);
    real->list_error = false;
    real->calls[48] = 0;

    EXPECT(hr_of(raw(list, 52, rtv_none, word(color), 0, 0)) == S_OK && real->calls[48] == 0);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);
    const D3D12_RECT rect{0, 0, 4, 4};
    raw(list, 52, rtv_real, word(color), 1, word(&rect));
    EXPECT(real->calls[48] == 1 && real->args[48][0] == rtv_real && real->args[48][1] == word(color) &&
           real->args[48][2] == 1 && real->args[48][3] == word(&rect));
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // A view made again at the same handle with a resource is cleared from then on.
    raw(device, 20, word(resource_b), 0, rtv_none);
    raw(list, 52, rtv_none, word(color), 0, 0);
    EXPECT(real->calls[48] == 2 && real->args[48][0] == rtv_none);
    raw(device, 20, 0, 0, rtv_none);  // and with no resource again: skipped again
    raw(list, 52, rtv_none, word(color), 0, 0);
    EXPECT(real->calls[48] == 2);

    // Descriptor copies move the mark: CopyDescriptorsSimple of the empty view, then of the real one over it.
    raw(device, 24, 1, rtv_copy, rtv_none, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    raw(list, 52, rtv_copy, word(color), 0, 0);
    EXPECT(real->calls[48] == 2);
    raw(device, 24, 1, rtv_copy, rtv_real, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    raw(list, 52, rtv_copy, word(color), 0, 0);
    EXPECT(real->calls[48] == 3 && real->args[48][0] == rtv_copy);
    // A copy of another heap type leaves the RTV marks alone.
    raw(device, 24, 1, rtv_copy, rtv_none, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    raw(list, 52, rtv_copy, word(color), 0, 0);
    EXPECT(real->calls[48] == 4);
    // CopyDescriptors with ranges: one destination range of two from two source ranges of one (empty, then real).
    const D3D12_CPU_DESCRIPTOR_HANDLE dest_starts[1] = {{rtv_copy}};
    const UINT dest_sizes[1] = {2};
    const D3D12_CPU_DESCRIPTOR_HANDLE src_starts[2] = {{rtv_none}, {rtv_real}};
    const UINT src_sizes[2] = {1, 1};
    raw10(device, 23, {1, word(dest_starts), word(dest_sizes), 2, word(src_starts), word(src_sizes),
                       D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 0, 0, 0});
    EXPECT(mock->calls[23] == 1);
    raw(list, 52, rtv_copy, word(color), 0, 0);
    EXPECT(real->calls[48] == 4);
    raw(list, 52, rtv_copy + kMockDescriptorInc, word(color), 0, 0);
    EXPECT(real->calls[48] == 5 && real->args[48][0] == rtv_copy + kMockDescriptorInc);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // Releasing an RTV heap drops the marks in its real range, so a heap made later at the same address starts
    // unmarked; a mark outside that range stays.
    g_heap_hook.next_ptr = nullptr;  // a plain PC heap even if a descriptor-heap hook is registered: real handles
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 4;
    const IID heap_iid = __uuidof(ID3D12DescriptorHeap);
    void* rtv_heap = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&rtv_heap_desc), word(&heap_iid), word(&rtv_heap)))));
    EXPECT(rtv_heap && xs::is_heap_proxy(rtv_heap) && raw(rtv_heap, 9) == kMockHeapCpuStart);
    const SIZE_T in_heap = kMockHeapCpuStart + 2 * kMockDescriptorInc;
    raw(device, 20, 0, 0, in_heap);
    const unsigned clears = real->calls[48];
    raw(list, 52, in_heap, word(color), 0, 0);
    EXPECT(real->calls[48] == clears);  // marked while the heap lives: skipped
    EXPECT(raw(rtv_heap, 2) == 0);
    raw(list, 52, in_heap, word(color), 0, 0);
    EXPECT(real->calls[48] == clears + 1 && real->args[48][0] == in_heap);  // the mark went with the heap
    real->list_error = false;  // the mock list still takes its own view there as invalid
    raw(list, 52, rtv_none, word(color), 0, 0);
    EXPECT(real->calls[48] == clears + 1);  // outside the released heap: still skipped
    // A second RTV heap at the same address: the view made there with a resource is cleared, and a view made with
    // no resource is marked again for as long as that heap lives.
    void* rtv_heap2 = nullptr;
    EXPECT(SUCCEEDED(hr_of(raw(device, 14, word(&rtv_heap_desc), word(&heap_iid), word(&rtv_heap2)))));
    raw(device, 20, word(resource_a), 0, in_heap);
    raw(list, 52, in_heap, word(color), 0, 0);
    EXPECT(real->calls[48] == clears + 2 && !real->list_error);
    raw(device, 20, 0, 0, in_heap);
    raw(list, 52, in_heap, word(color), 0, 0);
    EXPECT(real->calls[48] == clears + 2);
    EXPECT(raw(rtv_heap2, 2) == 0);
    EXPECT(!real->list_error && hr_of(raw(list, 9)) == S_OK);

    // The marks are the device's own: another device's clear at a handle marked on the first is forwarded (its mock
    // list then takes the view as invalid, which is what that device was told).
    Mock* other_mock = nullptr;
    void* other = make_device(other_mock, 9, true, true, true);
    EXPECT(other);
    void* other_allocator = nullptr;
    EXPECT(hr_of(raw(other, 9, D3D12_COMMAND_LIST_TYPE_DIRECT, word(&allocator_iid), word(&other_allocator))) == S_OK);
    void* other_list = nullptr;
    EXPECT(hr_of(raw(other, 12, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, word(other_allocator), 0, word(&gcl_iid),
                     word(&other_list))) == S_OK);
    Mock* const other_real = reinterpret_cast<Mock*>(xs::list_proxy_real_list(other_list));
    raw(other_list, 52, rtv_none, word(color), 0, 0);
    EXPECT(other_real->calls[48] == 1 && other_real->args[48][0] == rtv_none);

    EXPECT(release(other_list) == 0 && other_real->refs == 0);
    EXPECT(release(other_allocator) == 0);
    EXPECT(release(other) == 0 && other_mock->refs == 0);
    EXPECT(release(list) == 0 && real->refs == 0);
    EXPECT(release(allocator) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);
    g_mock_null_rtv_count = 0;
    std::printf("%s: list slot 146 turns texture barriers with no resource into global barriers (valid groups pass "
                "unchanged, unreadable ones are refused whole) and slot 52 skips the clear of an RTV made with no "
                "resource; the list stays valid\n",
                g_label);
    return 0;
}

// ---- 11. Presentation handler wiring (device slots 78–81 + queue PresentX/SuspendX/ResumeX) ----

struct MockPresenter {
    int interval_calls = 0;
    UINT last_interval = 0;
    int schedule_calls = 0;
    UINT last_event = 0;
    int wait_calls = 0;
    UINT last_flags = 0;
    int stats_calls = 0;
    int present_calls = 0;
    UINT last_planes = 0;
    int suspend_calls = 0;
    int resume_calls = 0;
};

int check_presentation_wiring() {
    Mock* mock = nullptr;
    void* device = make_device(mock, 11, true);
    EXPECT(device != nullptr);
    void* queue = make_queue(device);
    EXPECT(queue != nullptr);

    MockPresenter p{};
    xs::PresentationHandler h{};
    h.context = &p;
    h.set_frame_interval = [](void* ctx, IUnknown*, UINT us, UINT, UINT) -> HRESULT {
        auto* m = static_cast<MockPresenter*>(ctx);
        ++m->interval_calls;
        m->last_interval = us;
        return S_OK;
    };
    h.schedule_frame_event = [](void* ctx, UINT ev, UINT, UINT) -> HRESULT {
        auto* m = static_cast<MockPresenter*>(ctx);
        ++m->schedule_calls;
        m->last_event = ev;
        return S_OK;
    };
    h.wait_frame_event = [](void* ctx, UINT, DWORD, UINT flags, std::uint64_t* tok) -> HRESULT {
        auto* m = static_cast<MockPresenter*>(ctx);
        ++m->wait_calls;
        m->last_flags = flags;
        if (tok) *tok = 77;
        return S_FALSE;
    };
    h.get_frame_statistics = [](void* ctx, std::uint64_t, UINT, UINT* count, void*) -> HRESULT {
        auto* m = static_cast<MockPresenter*>(ctx);
        ++m->stats_calls;
        if (count) *count = 0;
        return S_FALSE;
    };
    h.present_x = [](void* ctx, UINT plane_count, const void*, const void*) -> HRESULT {
        auto* m = static_cast<MockPresenter*>(ctx);
        ++m->present_calls;
        m->last_planes = plane_count;
        return S_OK;
    };
    h.suspend_x = [](void* ctx, UINT) -> HRESULT {
        ++static_cast<MockPresenter*>(ctx)->suspend_calls;
        return S_OK;
    };
    h.resume_x = [](void* ctx) -> HRESULT {
        ++static_cast<MockPresenter*>(ctx)->resume_calls;
        return S_OK;
    };

    EXPECT(xs::set_presentation_handler(device, &h));
    EXPECT(hr_of(raw(device, 78, 0, 33333, 2, 0)) == S_OK && p.interval_calls == 1 && p.last_interval == 33333);
    EXPECT(hr_of(raw(device, 79, 2, 0, 0, 0)) == S_OK && p.schedule_calls == 1 && p.last_event == 2);
    std::uint64_t tok = 0;
    EXPECT(hr_of(raw(device, 80, 0, 0, 0, 2, word(&tok))) == S_FALSE && tok == 77 && p.wait_calls == 1 &&
           p.last_flags == 2);
    UINT count = 3;
    EXPECT(hr_of(raw(device, 81, 77, 0x1a, word(&count), 0)) == S_FALSE && count == 0 && p.stats_calls == 1);

    // Queue slot 30 routes to PresentX while attached. SuspendX and ResumeX have no default route, so 31/32 are refused
    // on a device with a presenter attached and never reach the presenter (the env mode, run_env_slots, routes them).
    EXPECT(hr_of(raw(queue, 30, 1, 0x1000, 0x2000)) == S_OK && p.present_calls == 1 && p.last_planes == 1);
    EXPECT(hr_of(raw(queue, 31, 0)) == E_NOTIMPL && p.suspend_calls == 0);
    EXPECT(hr_of(raw(queue, 32)) == E_NOTIMPL && p.resume_calls == 0);

    // Detached, a device that had a presenter attached does not fall back to its own presenter or the console tail:
    // queue slots 30 to 32 are refused and the presenter is not called again.
    EXPECT(xs::set_presentation_handler(device, nullptr));
    // Unattached GetFrameStatisticsX: no statistics yet (S_OK and a zero count), never the PC slot 81.
    UINT none = 7;
    std::uint8_t none_out[7 * 0x38] = {};
    EXPECT(hr_of(raw(device, 81, 77, 0x1a, word(&none), word(none_out))) == S_OK && none == 0 && p.stats_calls == 1);
    none = 7;
    EXPECT(hr_of(raw(device, 81, 77, 0x1a, word(&none), 0)) == E_POINTER && none == 7 && p.stats_calls == 1);
    EXPECT(hr_of(raw(device, 81, 77, 0x1a, 0, 0)) == E_POINTER && p.stats_calls == 1);
    EXPECT(hr_of(raw(queue, 30, 1, 0, 0)) == E_NOTIMPL && p.present_calls == 1);

    EXPECT(release(queue) == 0);
    EXPECT(release(device) == 0 && mock->refs == 0);

    // PresentX is one slot per layout, never both. A queue is in the older layout (PresentX at 30) until a fence
    // reaches slot 16 or 17, which only the newer layout's Signal and Wait take; from then on PresentX is two slots
    // later, at 32, and slot 30 is another console method there; with a presenter attached the slot of the other
    // layout is refused, and the presenter is not called.
    Mock* newer_mock = nullptr;
    void* newer_device = make_device(newer_mock, 11, true);
    void* newer_queue = make_queue(newer_device);
    void* newer_fence = make_fence(newer_device);
    EXPECT(newer_device && newer_queue && newer_fence);
    MockPresenter np;
    xs::PresentationHandler nh = h;
    nh.context = &np;
    EXPECT(xs::set_presentation_handler(newer_device, &nh));
    EXPECT(hr_of(raw(newer_queue, 30, 1, 0x1000, 0x2000)) == S_OK && np.present_calls == 1);
    EXPECT(hr_of(raw(newer_queue, 32, 1, 0x1000, 0x2000)) == E_NOTIMPL && np.present_calls == 1);
    EXPECT(hr_of(raw(newer_queue, 16, word(newer_fence), 1)) == S_OK);
    EXPECT(hr_of(raw(newer_queue, 32, 2, 0x1000, 0x2000)) == S_OK && np.present_calls == 2 && np.last_planes == 2);
    EXPECT(hr_of(raw(newer_queue, 30, 1, 0x1000, 0x2000)) == E_NOTIMPL && np.present_calls == 2);
    EXPECT(xs::set_presentation_handler(newer_device, nullptr));
    EXPECT(release(newer_queue) == 0 && release(newer_fence) == 0);
    EXPECT(release(newer_device) == 0 && newer_mock->refs == 0);
    std::printf("%s: presentation handler wiring (slots 78-81 and queue PresentX 30, or 32 in the newer layout) "
                "passed\n", g_label);
    return 0;
}

int main(int argc, char** argv) {
    no_dialogs();
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 2) {
        if (std::strcmp(argv[1], "--env-slots") == 0) return run_env_slots(true);
        if (std::strcmp(argv[1], "--env-slots-invalid") == 0) return run_env_slots(false);
        if (std::strcmp(argv[1], "--env-slots-74") == 0) return run_env_slots(true, true);
        if (std::strcmp(argv[1], "--env-slots-70") == 0) return run_env_slots(true, false, true);
        if (std::strcmp(argv[1], "--slot133-layout") == 0) return run_slot133_layout_word();
        return run_fault_mode(argv[1]);
    }
    if (argc != 1) return 2;
    if (check_tail(true) || check_host_device_floor() || check_levels() || check_slot48() || check_fence1() ||
        check_fence_completed_value() || check_create_failures() || check_open_shared() ||
        check_alloc_failures() || check_allocators() || check_xbox_slots() || check_lists() ||
        check_signalx_detection() || check_list_tail_names() || check_console_command_list() ||
        check_custom_heap_properties() ||
        check_resource_allocation_info() || check_slot70() || check_slot70_raw_buffer() ||
        check_slot57_native_graphics() || check_committed_resource3_x() ||
        check_descriptor_heap_hooks() ||
        check_render_target_sizes() ||
        check_allocation_descriptions_cleaned() ||
        check_render_target_layout() ||
        check_committed_resource_leases() ||
        check_committed_resource_states() ||
        check_slot74() ||
        check_xbox_queue_slots() ||
        check_list_cpu_handles() ||
        check_console_null_barrier_and_clear() ||
        check_presentation_wiring() || check_queue_copy_page_mappings())
        return 1;
    if (check_placed_resource_x() || check_depth_provider_refusal()) return 1;
    if (check_list_values()) return 1;       // last: it starts the list-values threads
    if (check_list_values_more()) return 1;  // and this one stops them
    std::printf("xs_proxy_mock_test: PASS (%d allocations refused on purpose)\n", g_failed_news);
    return 0;
}
