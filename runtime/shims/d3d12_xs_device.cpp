// Xbox-ABI proxy around a PC ID3D12Device. See d3d12_xs_device.h for the measured slots and the limits.
//
// How forwarding works: the proxy object's first word is a vtable built once from one thunk per slot. A thunk
// receives the proxy in RCX, replaces it with the PC device pointer from offset 8, and tail-calls the PC method at
// the same index through a 15-argument integer prototype. Every PC D3D12 device method takes integer/pointer
// arguments only, so the x64 registers and stack words line up exactly; methods that return a struct keep their
// hidden return pointer in RDX, which is passed through as the first argument unchanged. The measured exceptions
// are IUnknown (0-2), slot 14 (which wraps the heap), 25 and 26 (the title passes the return buffer in RCX and the
// device in RDX: see "Struct-returning device methods" below), 48 (a queue creation, where the PC has OpenExistingHeapFromAddress), 62 and 76, plus the heap proxy's 9/10
// conversion from the PC hidden-pointer form to the Xbox RAX form. Slots 8/75 (command queues), 36 (fences) and 32 (a shared fence) wrap
// what they create in queue and fence proxies built the same way, and slots 31/34/35/45/46/50 hand the real objects
// behind heap, queue and fence proxies to the real device (see the command queue and fence section).
//
// A thunk declares 14 argument words, but the title passes only as many as the method takes, so the caller's frame
// may end right after the four register homes. The compiler owns a function's incoming stack-argument slots and may
// write them, a tail call included. Every function a thunk hands its words to therefore takes them in the same
// positions (slot 8: desc, riid, out as a1-a3; slot 50: a1-a5; and so on). A tail call then writes back only the
// values it read from those slots. A helper that moved a word to another stack position would overwrite the title's
// frame. xs_proxy_mock_test's mock hit this in an optimized build, and its thunks now pass their words by pointer.
//
// The public HLE Freminet/ScarlettX reports five more Xbox device slots (a third-party observation, not a Microsoft
// ABI); the generic thunk routes them before it forwards anything: 48 CreateCommandQueueX, 73 CreateCommandListX,
// 78 SetFrameIntervalX, 79 ScheduleFrameEventX and 80 WaitFrameEventX. The slots can be moved with LEONIDA_XS_*_SLOT
// for a trace that disagrees. The frame-event calls answer from a per-device token counter: there is no vblank source
// behind the device before a presenter exists, so WaitFrameEventX returns at once with a fresh non-zero token.
#include "../src/title_va_alias.h"
#include "d3d12_xs_device.h"
#include "xmem.h"
#ifdef LEONIDA_VULKAN_DEVICE
#include "d3d12_com.h"  // rt::d3d12::create_console_descriptor_heap for slot 14
#include "../src/vulkan_gpu_labels.h"
#endif
#include "d3d12_xs_placed_depth.h"
#include "d3d12_trace.h"
#include "../src/fsdraw_probe.h"  // LEONIDA_FSDRAW_PROBE gates the fsdraw lines here
#include "../src/native_pso.h"
#include "xmem.h"  // rt::xmem::copy_page_mappings_fn for queue slot 31
#include "d3d12_xs_alloc_size.h"
#include "d3d12_xs_desc_clean.h"
#include "d3d12_xs_list_values.h"
#include "xmem.h"  // PageBatch, PageRange (queue slot 31)
#include "../src/frame_pacer.h"
#include "../src/local_diagnostics.h"
#include "d3d12_xs_pso_stream.h"
#include "../src/shader_library.h"  // leonida::fspx_payload_from_parts for slot 67
#include "../src/title_page_watch.h"  // leonida::kernel_writes for slot 14

#ifdef LEONIDA_VULKAN_DEVICE
// PresentX wiring: the queue's console slots land on the Xbox presentation core, which needs the Vulkan layer's
// WindowPresenter. Other builds of this file (mock and COM tests) keep the refused-slot behaviour.
#include "xbox_present.h"
#include "title_window.h"
#include "title_hud.h"
#include "package_splash.h"
#include "startup_trace.h"
#endif

#include <windows.h>
#include <wrl/client.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>  // _ReturnAddress: slot 61 serves a console caller differently from a host one
#include <iterator>
#include <limits>
#include <chrono>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <set>
#include <new>
#include <vector>
#include <memory>
#include <utility>
#include <vector>

namespace rt::d3d12_xs {
namespace {

// The retail title calls device slots well past ID3D12Device14 (slot 133 at least); every slot past the PC device's
// vtable goes through the generic thunk and is refused with E_NOTIMPL, so the table must outlast the highest one.
constexpr unsigned kMaxSlots = 192;
static_assert(kMaxSlots > 133, "the proxy table must cover the slots the retail title calls (up to 133)");

// Set by the Vulkan device behind the proxy; slot 14 uses it for a console-extended CreateDescriptorHeap.
std::atomic<ConsoleDescriptorHeapHandler> g_console_descriptor_heap{nullptr};
std::atomic<ReferenceSettler> g_reference_settler{nullptr};
// Commits the title's pages before slot 133 imports them or slot 14 checks them (set_placed_at_va_pages_hook; gta6rt
// registers xmem's commit_physical_range, which leaves memory other than mapped physical pages alone).
std::atomic<PlacedAtVaPagesHook> g_placed_at_va_pages{nullptr};
std::atomic<PinnedTexturePages> g_pin_texture_pages{nullptr};
std::atomic<CopyPageMappingsHook> g_copy_page_mappings{nullptr};
// True while this thread forwards a console caller's CheckFeatureSupport (device slot 13).
thread_local bool g_console_feature_query = false;
static_assert(kMaxSlots > 83, "the proxy table must cover ID3D12Device14 (83 slots)");
static_assert(sizeof(void*) == 8, "the Xbox-ABI proxy is x64-only");
static_assert(sizeof(D3D12_CPU_DESCRIPTOR_HANDLE) == 8,
              "the Xbox handle ABI is an 8-byte value in RAX; the heap thunk converts the PC hidden-pointer form");
static_assert(sizeof(D3D12_RESOURCE_DESC) == 56, "the measured Xbox description prefix is 56 bytes");

struct DeviceProxy {
    void** vtable;          // offset 0: the caller's ID3D12Device* points here
    ID3D12Device* real;     // offset 8: the PC device every thunk forwards to
    void** real_vtable;
    unsigned slot_count;    // number of valid entries in real_vtable
    std::atomic<unsigned long> refs;
    // The PresentX state (window, presenter, XboxPresentation) the first queue PresentX builds on this device,
    // under LEONIDA_VULKAN_DEVICE; always null elsewhere. Opaque: PresentState is defined with the queue proxy.
    void* present_state = nullptr;
    std::mutex present_state_mutex;
    // The upload-memory arena console list slot 148 (AllocateUploadMemory) carves from; always null until the
    // first call. Opaque: UploadArena is defined with the list proxy. Atomic: lists record on several threads
    // at once, so the first AllocateUploadMemory installs the arena by compare-exchange.
    std::atomic<void*> upload_state{nullptr};
    // The console runtime layout this title was built against (docs/d3d12_xs_console_abi.md §4): the queue proxy
    // writes it on detection and every object shares it — the queue/list slot tails shift +2 under the newer one.
    std::atomic<unsigned char> console_layout{0};  // 0 unknown, 1 older, 2 newer
    // The real (translated) CPU handles that hold a render-target view the title created with no resource (device
    // slot 20 with a null resource), for console list slot 52: on the console, clearing such a view does nothing.
    // The real device's descriptor cannot tell (the Vulkan layer writes the same empty record for a view it could not
    // make, and the PC runtime's handles are opaque), so the proxy keeps the mark itself. A view made there with a
    // resource, or a descriptor copy over it, removes it.
    std::mutex null_rtv_lock;
    std::set<SIZE_T> null_rtvs;
    std::atomic<std::size_t> null_rtv_count{0};  // null_rtvs.size(), read without the lock on the clear path
    // Set by create_device_proxy: false hands real (PC-shaped) lists back from CreateCommandList for callers
    // compiled against the PC headers.
    bool console_lists = true;
    // The device's IDXGIDevice1 (see "IDXGIDevice1 on the device" below): a title's IDXGIDevice1* points here.
    void** dxgi_vtable;
    DxgiAdapterProvider adapter_provider;
    DescriptorMappingProvider descriptor_mapping = nullptr;
    NativeGraphicsFactory native_graphics = nullptr;
    NativeStreamFactory prepare_stream = nullptr;
    TextureLeaseProvider texture_lease_provider = nullptr;
    RawTextureViewProvider raw_texture_view = nullptr;
    ConsoleConstantView constant_view = nullptr;
    ResourceByVaProvider resource_provider = nullptr;  // slot 70: a buffer by console VA
    DepthStencilUavProvider depth_stencil_uav_provider = nullptr;
    ValidateDepthPlacement validate_depth_placement = nullptr;
    PlacedDepthTextures placed_depth_textures;
    std::atomic<int> gpu_thread_priority;
    std::atomic<unsigned> max_frame_latency;
    // The first-frame subset of SetFrameIntervalX/ScheduleFrameEventX/WaitFrameEventX. Tokens were never measured;
    // they start at 1 and only have to be fresh and non-zero for the boot sequence. Waiting returns at once: there is
    // no vblank source until a presenter exists, and a bounded zero wait cannot stall the first frame.
    std::atomic<std::uint32_t> frame_interval_us{0};
    std::atomic<std::uint32_t> frame_latency{1};
    std::atomic<bool> frame_event_scheduled{false};
    std::atomic<std::uint32_t> frame_event_offset_us{0};
    // The timestamp query heap and readback of list write64 flags 1 and 2 (d3d12_xs_list_values.h Timestamps), made
    // at the first such write; null when the real device cannot make them.
    std::mutex timestamps_mutex;
    std::shared_ptr<list_values::Timestamps> timestamps;
    bool timestamps_tried = false;
    std::shared_ptr<leonida::FrameTokenSource> frame_tokens = std::make_shared<leonida::FrameTokenSource>();
    // Optional presentation wiring attached via set_presentation_handler (guarded by presentation_lock).
    SRWLOCK presentation_lock = SRWLOCK_INIT;
    PresentationHandler presentation{};
    bool external_presentation_mode = false;  // protected by presentation_lock, including after an explicit detach
};

// Runs f(handler) with the presentation lock held shared, so a detach cannot destroy the presentation mid-call. f
// returns false, without calling the presentation, when it has no function for the slot; the result says whether
// f handled the call.
// ponytail: a presenter callback must not call set_presentation_handler (exclusive inside shared deadlocks), and a
// detach waits for a WaitFrameEventX in flight up to its timeout.
template <typename F>
bool call_presentation(DeviceProxy* self, F&& f) {
    if (!self) return false;
    AcquireSRWLockShared(&self->presentation_lock);
    const bool handled = f(static_cast<const PresentationHandler&>(self->presentation));
    ReleaseSRWLockShared(&self->presentation_lock);
    return handled;
}

// Freed in proxy_release; defined where the states are complete.
void destroy_present_state(void* present_state);
void destroy_upload_state(void* upload_state);

constexpr unsigned kHeapSlots = 11;  // IUnknown + ID3D12Object + DeviceChild + Pageable + GetDesc + 2 handles

struct HeapProxy {
    void** vtable;          // offset 0: the caller's ID3D12DescriptorHeap* points here
    ID3D12DescriptorHeap* real;
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    std::atomic<unsigned long> refs;
    // Non-zero when the title's XMem allocation hook supplied the heap's backing address: slot 9 returns
    // custom_cpu_start and device descriptor calls translate [custom_cpu_start, +custom_byte_size) to real_cpu_start.
    SIZE_T custom_cpu_start;
    SIZE_T custom_byte_size;
    SIZE_T real_cpu_start;
    DescriptorHeapBackingFree custom_free;
    // An RTV heap's real CPU range, [rtv_real_start, +rtv_real_bytes): the null-view marks in it
    // (DeviceProxy::null_rtvs) are dropped when the heap goes, so a later heap at the same address starts unmarked.
    SIZE_T rtv_real_start = 0;
    SIZE_T rtv_real_bytes = 0;
};

struct DescriptorRange {
    SIZE_T custom_start;
    SIZE_T custom_end;  // exclusive
    SIZE_T real_start;
    SIZE_T stride;
    bool mapped;
    const DeviceProxy* device;
    const HeapProxy* heap;
};

// Immortal: late COM Release calls during process teardown may still unregister a heap or check the table.
std::mutex& descriptor_range_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

std::vector<DescriptorRange>& descriptor_ranges() {
    static auto* v = new std::vector<DescriptorRange>;
    return *v;
}

std::atomic<std::size_t> g_custom_range_count{0};

std::mutex& heap_alloc_mutex() {
    static auto* m = new std::mutex;
    return *m;
}

DescriptorHeapBackingAlloc g_heap_alloc = nullptr;
DescriptorHeapBackingFree g_heap_free = nullptr;
bool g_heap_alloc_explicit = false;

bool register_descriptor_range(const DeviceProxy* device, const HeapProxy* heap, SIZE_T custom_start,
                               SIZE_T byte_size, SIZE_T real_start, SIZE_T stride) {
    if (custom_start == 0 || byte_size == 0 || custom_start > (std::numeric_limits<SIZE_T>::max)() - byte_size)
        return false;
    const bool mapped = device->descriptor_mapping && device->descriptor_mapping(heap->real, real_start, byte_size);
    try {
        std::lock_guard lock(descriptor_range_mutex());
        descriptor_ranges().push_back({custom_start, custom_start + byte_size, real_start, stride, mapped, device, heap});
        g_custom_range_count.store(descriptor_ranges().size(), std::memory_order_release);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

void unregister_descriptor_range(const HeapProxy* heap) {
    if (g_custom_range_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard lock(descriptor_range_mutex());
    auto& ranges = descriptor_ranges();
    for (auto it = ranges.begin(); it != ranges.end();) {
        if (it->heap == heap) {
            it = ranges.erase(it);
        } else {
            ++it;
        }
    }
    g_custom_range_count.store(ranges.size(), std::memory_order_release);
}

D3D12_CPU_DESCRIPTOR_HANDLE translate_cpu_handle(const DeviceProxy* device, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (handle.ptr == 0 || g_custom_range_count.load(std::memory_order_acquire) == 0) return handle;
    std::lock_guard lock(descriptor_range_mutex());
    const auto& ranges = descriptor_ranges();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        if (device && it->device != device) continue;
        if (handle.ptr >= it->custom_start && handle.ptr < it->custom_end) {
            return D3D12_CPU_DESCRIPTOR_HANDLE{it->real_start + (handle.ptr - it->custom_start)};
        }
    }
    return handle;
}

// LOCAL (title-backed descriptor heaps): the title copies descriptors between its heaps' backing memory itself, with
// no API call, so (1) every descriptor the layer writes for a title-backed slot is also stored at the title's address,
// and (2) when a table is bound, what the title wrote at that table's address is carried into the real heap.
// LEONIDA_DESC_MIRROR=0 turns both off.
// Adapted from the patches descriptor-write-through (1) and descriptor-table-refresh (2) of
// Sigtryggur-Mani-Gudmundsson/graphics-runtime-patches at d9a0bc1b (author smg, as "Codex"), GPL-3.0: the functions
// from here to local_mirror_table, DescriptorRange's stride and mapped fields and local_real_gpu_start.
constexpr SIZE_T kLocalDescriptorBytes = 32;
constexpr SIZE_T kLocalMirrorSlots = 64;

bool local_desc_mirror() {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_DESC_MIRROR"); return !(v && *v == '0'); }();
    return on;
}

// Whether [address, address + bytes) is committed memory that can be read (and, with `write`, written). A handle
// that is only a number (a mock's, or a range the title has released) is left alone instead of faulting.
bool local_accessible(std::uint64_t address, std::uint64_t bytes, bool write) {
    const std::uint64_t end = address + bytes;
    if (!address || end < address) return false;
    while (address < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT)
            return false;
        const DWORD protect = info.Protect & 0xff;
        if (info.Protect & PAGE_GUARD) return false;
        const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                              protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
        const bool readable = writable || protect == PAGE_READONLY || protect == PAGE_EXECUTE_READ;
        if (write ? !writable : !readable) return false;
        address = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

void local_write_through(const DeviceProxy* device, D3D12_CPU_DESCRIPTOR_HANDLE custom, SIZE_T count) {
    if (!local_desc_mirror() || !custom.ptr || !count) return;
    if (!g_custom_range_count.load(std::memory_order_acquire)) return;
    std::lock_guard lock(descriptor_range_mutex());
    const auto& ranges = descriptor_ranges();
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        if (it->device != device || custom.ptr < it->custom_start || custom.ptr >= it->custom_end) continue;
        const SIZE_T offset = custom.ptr - it->custom_start;
        if (!it->stride || offset % it->stride || count > (it->custom_end - custom.ptr) / it->stride) return;
        const SIZE_T bytes = count * it->stride;
        if (offset > (std::numeric_limits<SIZE_T>::max)() - it->real_start) return;
        const SIZE_T real = it->real_start + offset;
        // On Wine, VirtualQuery can report a live vkMapMemory span as MEM_FREE.
        if (real == custom.ptr || (!it->mapped && !local_accessible(real, bytes, false)) ||
            !local_accessible(custom.ptr, bytes, true)) return;
        std::memcpy(reinterpret_cast<void*>(custom.ptr), reinterpret_cast<const void*>(real), bytes);
        static std::atomic<bool> said{false};
        if (!said.exchange(true))
            std::fprintf(stderr, "[rt] d3d12_xs: local: descriptors written for a title-backed heap are also stored at the "
                                 "title's address\n");
        return;
    }
}

// Caller holds descriptor_range_mutex, keeping the backing alive for the complete copy.
std::uint64_t copy_title_slots(std::uint8_t* dest, SIZE_T source, SIZE_T bytes) {
    const SIZE_T slots = bytes / kLocalDescriptorBytes;
    if (!local_accessible(source, bytes, false)) return 0;
    std::uint64_t copied = 0;
    for (SIZE_T i = 0; i < slots; ++i) {
        const SIZE_T at = i * kLocalDescriptorBytes;
        const auto* from = reinterpret_cast<const std::uint64_t*>(source + at);
        if (!(from[0] | from[1] | from[2] | from[3])) continue;  // keep slots populated only in the real heap
        std::memcpy(dest + at, from, kLocalDescriptorBytes);
        ++copied;
    }
    return copied;
}

// The queue already bounds this scan by the root signature and real heap. Resolve the CPU address again at
// submission so a list can be recorded once and see successive writes to its title-backed descriptors.
std::uint64_t pull_title_descriptors(void*, std::uint8_t* real, std::size_t bytes) {
    if (!local_desc_mirror() || !bytes || !g_custom_range_count.load(std::memory_order_acquire)) return 0;
    const auto addr = reinterpret_cast<SIZE_T>(real);
    std::lock_guard lock(descriptor_range_mutex());
    for (const DescriptorRange& range : descriptor_ranges()) {
        const SIZE_T size = range.custom_end - range.custom_start;
        if (addr < range.real_start || addr - range.real_start >= size) continue;
        const SIZE_T offset = addr - range.real_start;
        if (offset % kLocalDescriptorBytes) return 0;
        // real is the queue's live vkMapMemory destination. Wine can report it as free to VirtualQuery.
        return copy_title_slots(real, range.custom_start + offset, std::min(bytes, size - offset));
    }
    return 0;
}

D3D12_GPU_DESCRIPTOR_HANDLE local_real_gpu_start(const HeapProxy* heap);

void local_mirror_table(const DeviceProxy* device, std::uint64_t gpu_handle) {
    if (!local_desc_mirror() || !gpu_handle || g_custom_range_count.load(std::memory_order_acquire) == 0) return;
    std::lock_guard lock(descriptor_range_mutex());
    for (const DescriptorRange& range : descriptor_ranges()) {
        if ((device && range.device != device) || !range.heap) continue;
        const std::uint64_t gpu_start = local_real_gpu_start(range.heap).ptr;
        const std::uint64_t bytes = range.custom_end - range.custom_start;
        if (!gpu_start || gpu_handle < gpu_start || gpu_handle >= gpu_start + bytes) continue;
        const std::uint64_t offset = (gpu_handle - gpu_start) / kLocalDescriptorBytes * kLocalDescriptorBytes;
        const std::uint64_t slots = (std::min<std::uint64_t>)(kLocalMirrorSlots, (bytes - offset) / kLocalDescriptorBytes);
        const SIZE_T copy_bytes = slots * kLocalDescriptorBytes;
        if (!local_accessible(range.real_start + offset, copy_bytes, true)) return;
        const auto copied = copy_title_slots(reinterpret_cast<std::uint8_t*>(range.real_start + offset),
                                             range.custom_start + offset, copy_bytes);
        static std::atomic<int> said{0};
        if (said.fetch_add(1) < 4)
            std::fprintf(stderr, "[rt] d3d12_xs: local: table at heap offset 0x%llx: %llu of %llu slots taken from the "
                                 "title's backing memory\n", static_cast<unsigned long long>(offset),
                         static_cast<unsigned long long>(copied), static_cast<unsigned long long>(slots));
        return;
    }
}

// ---- Render-target views made with no resource (DeviceProxy::null_rtvs) ----

// Device slot 20 wrote an RTV at the real handle `ptr`: marked when the title passed no resource, unmarked otherwise.
void mark_null_rtv(DeviceProxy* device, SIZE_T ptr, bool no_resource) {
    if (!ptr || (!no_resource && device->null_rtv_count.load(std::memory_order_acquire) == 0)) return;
    std::lock_guard lock(device->null_rtv_lock);
    try {
        if (no_resource)
            device->null_rtvs.insert(ptr);
        else
            device->null_rtvs.erase(ptr);
    } catch (const std::bad_alloc&) {
        // The mark is lost: a clear of this view keeps the PC list's answer, as before the mark existed.
    }
    device->null_rtv_count.store(device->null_rtvs.size(), std::memory_order_release);
}

bool is_null_rtv(DeviceProxy* device, SIZE_T ptr) {
    if (!ptr || device->null_rtv_count.load(std::memory_order_acquire) == 0) return false;
    std::lock_guard lock(device->null_rtv_lock);
    return device->null_rtvs.count(ptr) != 0;
}

// A descriptor copy moves the marks with the descriptors: `count` RTV descriptors from the real handle `src` to
// `dest`, `increment` bytes apart. Caller holds null_rtv_lock.
void copy_null_rtv_marks_locked(DeviceProxy* device, SIZE_T dest, SIZE_T src, UINT count, UINT increment) {
    for (UINT i = 0; i < count; ++i) {
        const SIZE_T to = dest + static_cast<SIZE_T>(i) * increment;
        if (device->null_rtvs.count(src + static_cast<SIZE_T>(i) * increment))
            device->null_rtvs.insert(to);
        else
            device->null_rtvs.erase(to);
    }
}

// CopyDescriptors / CopyDescriptorsSimple of RTVs, after the copy, with the handles the real device got. A null
// size array means ranges of one descriptor, as in D3D12. Nothing to do while no view is marked: a copy can only
// move a mark, never make one.
void copy_null_rtv_marks(DeviceProxy* device, UINT num_dest, const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts,
                         const UINT* dest_sizes, UINT num_src, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                         const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV || device->null_rtv_count.load(std::memory_order_acquire) == 0) return;
    if (!dest_starts || !src_starts) return;
    const UINT increment = device->real->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!increment) return;
    std::lock_guard lock(device->null_rtv_lock);
    try {
        UINT d = 0, s = 0, d_used = 0, s_used = 0;
        while (d < num_dest && s < num_src) {
            const UINT d_size = dest_sizes ? dest_sizes[d] : 1;
            const UINT s_size = src_sizes ? src_sizes[s] : 1;
            if (d_used == d_size) { ++d; d_used = 0; continue; }
            if (s_used == s_size) { ++s; s_used = 0; continue; }
            const UINT d_left = d_size - d_used, s_left = s_size - s_used;
            const UINT run = d_left < s_left ? d_left : s_left;
            copy_null_rtv_marks_locked(device, dest_starts[d].ptr + static_cast<SIZE_T>(d_used) * increment,
                                       src_starts[s].ptr + static_cast<SIZE_T>(s_used) * increment, run, increment);
            d_used += run;
            s_used += run;
        }
    } catch (const std::bad_alloc&) {
    }
    device->null_rtv_count.store(device->null_rtvs.size(), std::memory_order_release);
}

// An RTV heap was released: its real handles can name another heap's views from now on, so no mark may outlive it.
void drop_null_rtv_marks(DeviceProxy* device, SIZE_T start, SIZE_T bytes) {
    if (!device || !start || !bytes || device->null_rtv_count.load(std::memory_order_acquire) == 0) return;
    const SIZE_T end = start > (std::numeric_limits<SIZE_T>::max)() - bytes ? (std::numeric_limits<SIZE_T>::max)()
                                                                             : start + bytes;
    std::lock_guard lock(device->null_rtv_lock);
    device->null_rtvs.erase(device->null_rtvs.lower_bound(start), device->null_rtvs.lower_bound(end));
    device->null_rtv_count.store(device->null_rtvs.size(), std::memory_order_release);
}

std::atomic<std::uint64_t> g_hits[kMaxSlots];
std::atomic<bool> g_first[kMaxSlots];
std::atomic<bool> g_beyond_warned[kMaxSlots];
std::atomic<std::uint64_t> g_heap_hits[kHeapSlots];
std::atomic<bool> g_heap_first[kHeapSlots];
// The hit counters are for tests. They count from the first call of a *_slot_hits reader or of
// reset_device_proxy_stats, so the calls of a title, which reads no counter, write no shared counter.
std::atomic<bool> g_count_hits{false};

// For the readers and the reset. The flag is written one time only.
void start_hit_counts() {
    if (!g_count_hits.load(std::memory_order_relaxed)) g_count_hits.store(true, std::memory_order_relaxed);
}

void count_hit(std::atomic<std::uint64_t>& hits) {
    if (g_count_hits.load(std::memory_order_relaxed)) hits.fetch_add(1, std::memory_order_relaxed);
}

// True for the first call of a slot only. The flag is read first: a compare-exchange takes the cache line for
// writing also when it fails, and the flags of all slots share one or two lines.
bool first_call(std::atomic<bool>& seen) {
    if (seen.load(std::memory_order_relaxed)) return false;
    bool expected = false;
    return seen.compare_exchange_strong(expected, true, std::memory_order_relaxed);
}

bool trace_enabled() {
    static const bool enabled = [] {
        char value[8] = {};
        const DWORD length = GetEnvironmentVariableA("XS_TRACE", value, sizeof(value));
        return length > 0 && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

const char* slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 8: return "CreateCommandQueue -> queue proxy";
        case 9: return "CreateCommandAllocator (PC slot 9)";
        case 10: return "CreateGraphicsPipelineState (PC slot 10)";
        case 11: return "CreateComputePipelineState (PC slot 11)";
        case 14: return "CreateDescriptorHeap";
        case 16: return "CreateRootSignature";
        case 17: return "CreateConstantBufferView (CPU handle translated)";
        case 18: return "CreateShaderResourceView (CPU handle translated)";
        case 19: return "CreateUnorderedAccessView (CPU handle translated)";
        case 20: return "CreateRenderTargetView (CPU handle translated)";
        case 21: return "CreateDepthStencilView (CPU handle translated)";
        case 22: return "CreateSampler (CPU handle translated)";
        case 23: return "CopyDescriptors (CPU handles translated)";
        case 24: return "CopyDescriptorsSimple (CPU handles translated)";
        case 25: return "GetResourceAllocationInfo (C or PC form)";
        case 26: return "GetCustomHeapProperties (C or PC form)";
        case 27: return "CreateCommittedResource (initial state mapped)";
        case 31: return "CreateSharedHandle (proxies unwrapped)";
        case 32: return "OpenSharedHandle (a fence comes back wrapped)";
        case 34: return "MakeResident (proxies unwrapped)";
        case 35: return "Evict (proxies unwrapped)";
        case 36: return "CreateFence -> fence proxy";
        case 41: return "CreateCommandSignature (Xbox argument type 105 as DISPATCH)";
        case 43: return "GetAdapterLuid";
        case 45: return "SetEventOnMultipleFenceCompletion (fence proxies unwrapped)";
        case 46: return "SetResidencyPriority (proxies unwrapped)";
        case 48: return "Xbox CreateCommandQueueX (32-byte desc) -> queue proxy";
        case 50: return "EnqueueMakeResident (proxies unwrapped)";
        case 53: return "CreateCommittedResource1 (initial state mapped)";
        case 56: return "GetResourceAllocationInfo1 (C or PC form)";
        case 57: return "Xbox native graphics pipeline / PC lifetime tracker";
        case 61: return "Xbox CreateSamplerX -> PC CreateSampler (a host caller gets PC CreateMetaCommand)";
        case 62: return "Xbox CreateComputePipelineState";
        case 67: return "Xbox DeserializeComputePipelineStateX";
        case 70: return "Xbox shader-resource view of placed memory";
        case 71: return "Xbox unordered-access view of placed memory";
        case 73: return "Xbox CreateCommandListX -> console list proxy";
        case 74: return "Xbox (pointer, u32, pointer) stand-in";
        case 75: return "CreateCommandQueue1 -> queue proxy";
        case 76: return "Xbox resource creation";
        case 127: return "Xbox CreateCommittedResource3";
        case 78: return "Xbox SetFrameIntervalX";
        case 79: return "Xbox ScheduleFrameEventX";
        case 80: return "Xbox WaitFrameEventX";
        case 55: return "Xbox CreateDerivedPipelineState -> PC CreatePipelineState of the patched stream (slot 47)";
        case 81: return "Xbox GetFrameStatisticsX";
        case 86: return "Xbox CreatePipelineState -> PC CreatePipelineState (slot 47)";
        case 101: return "Xbox CreateStateObject -> PC CreateStateObject (slot 62)";
        case 108: return "Xbox GetResourceAllocationInfo2 -> PC GetResourceAllocationInfo2 (slot 68)";
        case 133: return "Xbox CreatePlacedResourceX (a buffer on the title's pages, else committed)";
        default: return "forwarded";
    }
}

void note_slot(unsigned slot) {
    if (slot >= kMaxSlots) return;
    count_hit(g_hits[slot]);
    const bool first = first_call(g_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) first call\n", slot, slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: device slot %u (%s) call\n", slot, slot_name(slot));
    }
}

// Xbox device slots (public HLE Freminet/ScarlettX, an observation, not Microsoft documentation). The defaults are
// the HLE's; an environment variable that names another slot in 3..127 replaces one (a whole-table move for a trace
// that disagrees). A value outside that range is logged once and ignored. The table is read once, on the first
// generic slot call, so a test that wants other slots sets the variables before it creates a device.
struct XboxDeviceSlotRoutes {
    unsigned frame_interval = 78;  // SetFrameIntervalX(output, interval_us, latency, flags)
    unsigned schedule_event = 79;  // ScheduleFrameEventX(event, offset_us, reserved, flags)
    unsigned wait_event = 80;      // WaitFrameEventX(event, timeout_ms, reserved, flags, &token)
    unsigned frame_statistics = 81;  // GetFrameStatisticsX(token, type_mask, count, out)
    unsigned queue_x = 48;         // CreateCommandQueueX(desc, riid, out)
    unsigned list_x = 73;          // CreateCommandListX(desc, allocator, initial_state, riid, out)
};

// Reads a slot number in lo..hi from `name`: an unset variable gives `fallback`; a bad value is logged (`what` names
// the kind of slot) and ignored.
unsigned env_slot(const char* name, unsigned fallback, unsigned lo, unsigned hi, const char* what) {
    char value[32] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0) return fallback;
    if (length >= sizeof(value)) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s is too long for a %s; the default is kept\n", name, what);
        return fallback;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long slot = std::strtoul(value, &end, 0);
    if (errno != 0 || end == value || *end != '\0' || slot < lo || slot > hi) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s=%s is not a %s in %u..%u; the default is kept\n", name, value, what,
                     lo, hi);
        return fallback;
    }
    return static_cast<unsigned>(slot);
}

const XboxDeviceSlotRoutes& xbox_device_slot_routes() {
    static const XboxDeviceSlotRoutes routes = [] {
        XboxDeviceSlotRoutes r;
        const unsigned frame_interval = env_slot("LEONIDA_XS_FRAME_INTERVAL_SLOT", r.frame_interval, 3, 127,
                                                 "device slot");
        const unsigned schedule_event = env_slot("LEONIDA_XS_SCHEDULE_EVENT_SLOT", r.schedule_event, 3, 127,
                                                 "device slot");
        const unsigned wait_event = env_slot("LEONIDA_XS_WAIT_EVENT_SLOT", r.wait_event, 3, 127, "device slot");
        const unsigned frame_statistics = env_slot("LEONIDA_XS_FRAME_STATISTICS_SLOT", r.frame_statistics, 3, 127,
                                                   "device slot");
        const unsigned queue_x = env_slot("LEONIDA_XS_QUEUE_X_SLOT", r.queue_x, 3, 127, "device slot");
        const unsigned list_x = env_slot("LEONIDA_XS_LIST_X_SLOT", r.list_x, 3, 127, "device slot");
        const bool moved = frame_interval != r.frame_interval || schedule_event != r.schedule_event ||
                           wait_event != r.wait_event || frame_statistics != r.frame_statistics ||
                           queue_x != r.queue_x || list_x != r.list_x;
        r.frame_interval = frame_interval;
        r.schedule_event = schedule_event;
        r.wait_event = wait_event;
        r.frame_statistics = frame_statistics;
        r.queue_x = queue_x;
        r.list_x = list_x;
        if (moved)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: Xbox device slots: SetFrameIntervalX=%u ScheduleFrameEventX=%u "
                         "WaitFrameEventX=%u GetFrameStatisticsX=%u CreateCommandQueueX=%u CreateCommandListX=%u\n",
                         r.frame_interval, r.schedule_event, r.wait_event, r.frame_statistics, r.queue_x, r.list_x);
        return r;
    }();
    return routes;
}

// The public HLE call shapes. SetFrameIntervalX's output and flags are unused while there is no output or pacing;
// ScheduleFrameEventX's reserved event pointer is a3 in the console call and a4 is its flags. The logged words are the
// caller's, so a trace names the slot even when an override moved it.
void log_xbox_route_once(unsigned slot, const char* name, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                         std::uint64_t a4, std::uint64_t a5) {
    static std::atomic<bool> logged[kMaxSlots];
    if (slot >= kMaxSlots) return;
    bool expected = false;
    if (!logged[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: device slot %u routed to %s (args 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                 name, static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                 static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4),
                 static_cast<unsigned long long>(a5));
}

const char* heap_slot_name(unsigned slot) {
    switch (slot) {
        case 0: return "QueryInterface";
        case 1: return "AddRef";
        case 2: return "Release";
        case 7: return "GetDevice";
        case 8: return "GetDesc";
        case 9: return "GetCPUDescriptorHandleForHeapStart";
        case 10: return "GetGPUDescriptorHandleForHeapStart";
        default: return "forwarded";
    }
}

void note_heap_slot(unsigned slot) {
    if (slot >= kHeapSlots) return;
    count_hit(g_heap_hits[slot]);
    const bool first = first_call(g_heap_first[slot]);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) first call\n", slot, heap_slot_name(slot));
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: heap slot %u (%s) call\n", slot, heap_slot_name(slot));
    }
}

std::uint64_t hr_to_u64(HRESULT hr) {
    return static_cast<std::uint64_t>(static_cast<std::uint32_t>(hr));
}

unsigned long proxy_add_ref(DeviceProxy* self) {
    note_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long proxy_release(DeviceProxy* self) {
    note_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        destroy_present_state(self->present_state);
        destroy_upload_state(self->upload_state.load(std::memory_order_relaxed));
        ID3D12Device* real = self->real;
        delete self;
        if (real) real->Release();
    }
    return refs;
}

bool is_device_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(ID3D12Device))) return true;
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device14))) return true;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device13))) return true;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device12))) return true;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device11))) return true;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device10))) return true;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (IsEqualIID(iid, __uuidof(ID3D12Device9))) return true;
#endif
    if (IsEqualIID(iid, __uuidof(ID3D12Device8))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device7))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device6))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device5))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device4))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device3))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device2))) return true;
    if (IsEqualIID(iid, __uuidof(ID3D12Device1))) return true;
    return false;
}

HRESULT proxy_query_interface(DeviceProxy* self, const IID* riid, void** out) {
    note_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (IsEqualIID(*riid, __uuidof(IUnknown)) || IsEqualIID(*riid, __uuidof(ID3D12Object))) {
        *out = self;
        proxy_add_ref(self);
        return S_OK;
    }
    if (IsEqualIID(*riid, __uuidof(IDXGIObject)) || IsEqualIID(*riid, __uuidof(IDXGIDevice)) ||
        IsEqualIID(*riid, __uuidof(IDXGIDevice1))) {
        *out = &self->dxgi_vtable;
        proxy_add_ref(self);
        return S_OK;
    }
    if (!is_device_iid(*riid)) return E_NOINTERFACE;

    // Only claim an interface the PC device really implements; the proxy vtable covers its slots.
    void* supported = nullptr;
    HRESULT hr = self->real->QueryInterface(*riid, &supported);
    if (FAILED(hr)) return hr;
    if (!supported) return E_NOINTERFACE;
    static_cast<IUnknown*>(supported)->Release();
    *out = self;
    proxy_add_ref(self);
    return S_OK;
}

// ---- IDXGIDevice1 on the device ---------------------------------------------------------------------------------
//
// A GDKX title finds its output through the device: QueryInterface for IDXGIDevice1, GetAdapter, EnumOutputs(0),
// and hands that output to SetFrameIntervalX (docs/d3d12_vulkan.md section 4.5, from the public samples). A PC
// ID3D12Device has no IDXGIDevice, so the proxy answers it itself: a second vtable pointer inside the proxy object,
// with the proxy's reference count and QueryInterface, so the two pointers are one COM object. Only IDXGIDevice and
// IDXGIDevice1 are answered (what the samples ask for). The 14 slots are the public dxgi.h order; that the console
// keeps it is the same assumption as for the queue and the fence.
//
// GetAdapter and GetParent hand out the adapter of the device behind the proxy. The provider says how: the default
// asks the host's DXGI for the adapter with the device's LUID (the host backend); set_device_adapter_provider puts
// the Vulkan layer's own adapter there, so that backend never loads the host's dxgi.dll for it.
constexpr unsigned kDxgiDeviceSlots = 14;  // IUnknown 3, IDXGIObject 4, IDXGIDevice 5, IDXGIDevice1 2

DeviceProxy* dxgi_owner(void* self) {
    return reinterpret_cast<DeviceProxy*>(static_cast<char*>(self) - offsetof(DeviceProxy, dxgi_vtable));
}

HRESULT host_adapter(ID3D12Device* real, REFIID riid, void** out) {
    using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    static const CreateFactoryFn create_factory = [] {
        const HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return dxgi ? reinterpret_cast<CreateFactoryFn>(
                          reinterpret_cast<void*>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
                    : nullptr;
    }();
    if (!create_factory) return DXGI_ERROR_NOT_FOUND;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = create_factory(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = factory->EnumAdapterByLuid(real->GetAdapterLuid(), riid, out);
    factory->Release();
    return hr;
}

HRESULT dxgi_adapter(DeviceProxy* self, REFIID riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    const DxgiAdapterProvider provider = self->adapter_provider ? self->adapter_provider : &host_adapter;
    const HRESULT hr = provider(self->real, riid, out);
    if (FAILED(hr)) *out = nullptr;
    return hr;
}

void note_dxgi(const char* method) {
    d3d12_trace::record(method);
}

HRESULT WINAPI dxgi_query_interface(void* self, const IID* riid, void** out) {
    return proxy_query_interface(dxgi_owner(self), riid, out);
}
ULONG WINAPI dxgi_add_ref(void* self) { return proxy_add_ref(dxgi_owner(self)); }
ULONG WINAPI dxgi_release(void* self) { return proxy_release(dxgi_owner(self)); }

// IDXGIObject's private data is the device's: ID3D12Object has the same three methods with the same arguments.
HRESULT WINAPI dxgi_set_private_data(void* self, const GUID* name, UINT size, const void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_set_private_data_interface(void* self, const GUID* name, const IUnknown* object) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->SetPrivateDataInterface(*name, object);
}
HRESULT WINAPI dxgi_get_private_data(void* self, const GUID* name, UINT* size, void* data) {
    if (!name) return E_INVALIDARG;
    return dxgi_owner(self)->real->GetPrivateData(*name, size, data);
}
HRESULT WINAPI dxgi_get_parent(void* self, const IID* riid, void** out) {
    note_dxgi("XboxIDXGIDevice::GetParent");
    if (!riid) return E_POINTER;
    return dxgi_adapter(dxgi_owner(self), *riid, out);
}
HRESULT WINAPI dxgi_get_adapter(void* self, IDXGIAdapter** out) {
    note_dxgi("XboxIDXGIDevice::GetAdapter");
    return dxgi_adapter(dxgi_owner(self), __uuidof(IDXGIAdapter), reinterpret_cast<void**>(out));
}
// CreateSurface and QueryResourceResidency are D3D10/11-era methods no D3D12 title has a use for; what the console
// answers is not measured, so they are refused and say so once.
HRESULT WINAPI dxgi_create_surface(void*, const DXGI_SURFACE_DESC*, UINT, DXGI_USAGE, const DXGI_SHARED_RESOURCE*,
                                   IDXGISurface** out) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: IDXGIDevice::CreateSurface is not implemented; returning E_NOTIMPL\n");
    if (out) *out = nullptr;
    return E_NOTIMPL;
}
HRESULT WINAPI dxgi_query_resource_residency(void*, IUnknown* const*, DXGI_RESIDENCY*, UINT) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: IDXGIDevice::QueryResourceResidency is not implemented; returning E_NOTIMPL\n");
    return E_NOTIMPL;
}
// The priority and the frame latency are kept and read back; nothing acts on them (the Xbox presenter's latency is
// SetFrameIntervalX's). The ranges are the documented ones: -7..7, and 1..16 with 0 for the default of 3.
HRESULT WINAPI dxgi_set_gpu_thread_priority(void* self, INT priority) {
    if (priority < -7 || priority > 7) return E_INVALIDARG;
    dxgi_owner(self)->gpu_thread_priority.store(priority, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_gpu_thread_priority(void* self, INT* priority) {
    if (!priority) return E_POINTER;
    *priority = dxgi_owner(self)->gpu_thread_priority.load(std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_set_maximum_frame_latency(void* self, UINT latency) {
    if (latency > 16) return DXGI_ERROR_INVALID_CALL;
    dxgi_owner(self)->max_frame_latency.store(latency ? latency : 3, std::memory_order_relaxed);
    return S_OK;
}
HRESULT WINAPI dxgi_get_maximum_frame_latency(void* self, UINT* latency) {
    if (!latency) return E_POINTER;
    *latency = dxgi_owner(self)->max_frame_latency.load(std::memory_order_relaxed);
    return S_OK;
}

void** get_dxgi_vtable() {
    static void* table[kDxgiDeviceSlots] = {
        reinterpret_cast<void*>(&dxgi_query_interface),          // 0
        reinterpret_cast<void*>(&dxgi_add_ref),                  // 1
        reinterpret_cast<void*>(&dxgi_release),                  // 2
        reinterpret_cast<void*>(&dxgi_set_private_data),         // 3  IDXGIObject
        reinterpret_cast<void*>(&dxgi_set_private_data_interface),
        reinterpret_cast<void*>(&dxgi_get_private_data),
        reinterpret_cast<void*>(&dxgi_get_parent),
        reinterpret_cast<void*>(&dxgi_get_adapter),              // 7  IDXGIDevice
        reinterpret_cast<void*>(&dxgi_create_surface),
        reinterpret_cast<void*>(&dxgi_query_resource_residency),
        reinterpret_cast<void*>(&dxgi_set_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_get_gpu_thread_priority),
        reinterpret_cast<void*>(&dxgi_set_maximum_frame_latency),  // 12 IDXGIDevice1
        reinterpret_cast<void*>(&dxgi_get_maximum_frame_latency),
    };
    return table;
}

unsigned long heap_add_ref(HeapProxy* self) {
    note_heap_slot(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

unsigned long heap_release(HeapProxy* self) {
    note_heap_slot(2);
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) {
        if (self->custom_cpu_start != 0) unregister_descriptor_range(self);
        drop_null_rtv_marks(self->device, self->rtv_real_start, self->rtv_real_bytes);
        void* const backing = reinterpret_cast<void*>(self->custom_cpu_start);
        const DescriptorHeapBackingFree free_fn = self->custom_free;
        ID3D12DescriptorHeap* real = self->real;
        DeviceProxy* device = self->device;
        delete self;
        if (real) real->Release();
        if (device) proxy_release(device);
        if (backing && free_fn) free_fn(backing, kXmemDescriptorHeapAttributes);
    }
    return refs;
}

bool is_heap_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(ID3D12DescriptorHeap)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(IUnknown));
}

HRESULT heap_query_interface(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_heap_iid(*riid)) return E_NOINTERFACE;
    *out = self;
    heap_add_ref(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice on the heap must hand back the Xbox-ABI device, not the raw PC device.
HRESULT heap_get_device(HeapProxy* self, const IID* riid, void** out) {
    note_heap_slot(7);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

// The PC methods write through the hidden return pointer (RDX) on the measured runtime; the Xbox ABI wants the
// 8-byte value in RAX. Call with both conventions and keep whichever produced a value, so a runtime that really
// returns in RAX also works. The extra register is harmless when the callee ignores it.
D3D12_CPU_DESCRIPTOR_HANDLE raw_heap_get_cpu_handle(ID3D12DescriptorHeap* real, void** real_vtable) {
    constexpr SIZE_T kUnset = ~static_cast<SIZE_T>(0);
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_CPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_CPU_DESCRIPTOR_HANDLE*);
    const D3D12_CPU_DESCRIPTOR_HANDLE returned = reinterpret_cast<Fn>(real_vtable[9])(real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

D3D12_CPU_DESCRIPTOR_HANDLE heap_get_cpu_handle(HeapProxy* self) {
    note_heap_slot(9);
    if (self->custom_cpu_start != 0) return D3D12_CPU_DESCRIPTOR_HANDLE{self->custom_cpu_start};
    return raw_heap_get_cpu_handle(self->real, self->real_vtable);
}

D3D12_GPU_DESCRIPTOR_HANDLE heap_get_gpu_handle(HeapProxy* self) {
    note_heap_slot(10);
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned =
        reinterpret_cast<Fn>(self->real_vtable[10])(self->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

D3D12_GPU_DESCRIPTOR_HANDLE local_real_gpu_start(const HeapProxy* heap) {
    constexpr UINT64 kUnset = ~static_cast<UINT64>(0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle{};
    handle.ptr = kUnset;
    using Fn = D3D12_GPU_DESCRIPTOR_HANDLE(WINAPI*)(ID3D12DescriptorHeap*, D3D12_GPU_DESCRIPTOR_HANDLE*);
    const D3D12_GPU_DESCRIPTOR_HANDLE returned = reinterpret_cast<Fn>(heap->real_vtable[10])(heap->real, &handle);
    return handle.ptr != kUnset ? handle : returned;
}

template <unsigned Slot>
std::uint64_t WINAPI heap_slot(HeapProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(heap_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return heap_add_ref(self);
    } else if constexpr (Slot == 2) {
        return heap_release(self);
    } else if constexpr (Slot == 7) {
        return hr_to_u64(heap_get_device(self, reinterpret_cast<const IID*>(a1),
                                         reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 9) {
        return static_cast<std::uint64_t>(heap_get_cpu_handle(self).ptr);
    } else if constexpr (Slot == 10) {
        return static_cast<std::uint64_t>(heap_get_gpu_handle(self).ptr);
    } else {
        note_heap_slot(Slot);
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <std::size_t... I>
void fill_heap_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&heap_slot<static_cast<unsigned>(I)>) ), ...);
}

void** get_heap_vtable() {
    static void** vtable = [] {
        static void* storage[kHeapSlots];
        fill_heap_vtable(storage, std::make_index_sequence<kHeapSlots>{});
        return storage;
    }();
    return vtable;
}

HRESULT create_heap_proxy(ID3D12DescriptorHeap* real, DeviceProxy* device, REFIID riid, void** out,
                          SIZE_T custom_cpu_start = 0, SIZE_T custom_byte_size = 0, SIZE_T real_cpu_start = 0,
                          DescriptorHeapBackingFree custom_free = nullptr, SIZE_T stride = 0) {
    if (!real || !device) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    HeapProxy* proxy = new (std::nothrow) HeapProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_heap_vtable();
    proxy->real = real;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->custom_cpu_start = custom_cpu_start;
    proxy->custom_byte_size = custom_byte_size;
    proxy->real_cpu_start = real_cpu_start;
    proxy->custom_free = custom_free;
    proxy_add_ref(device);  // the heap keeps its Xbox-ABI device alive for GetDevice

    if (custom_cpu_start != 0 &&
        !register_descriptor_range(device, proxy, custom_cpu_start, custom_byte_size, real_cpu_start, stride)) {
        delete proxy;
        proxy_release(device);
        return E_OUTOFMEMORY;
    }

    const HRESULT hr = heap_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its references to `real` and `device`.
        if (custom_cpu_start != 0) unregister_descriptor_range(proxy);
        delete proxy;
        proxy_release(device);
        return hr;
    }
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: heap proxy %p wraps PC heap %p (device proxy %p)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), static_cast<void*>(device));
    heap_release(proxy);
    return S_OK;
}

// ---- Command queue and fence proxies --------------------------------------------------------------------------
//
// Device slots 8 (CreateCommandQueue), 75 (CreateCommandQueue1), 36 (CreateFence) and 32 (OpenSharedHandle, for a
// fence) wrap what they create, so a title never holds a PC or Vulkan-layer queue or fence. The proxies forward
// the public chain in PC order, exactly like the device proxy; on a fence a slot past the PC vtable is logged and
// refused with E_NOTIMPL. The one exception is fence slot 12, which returns the address of the completed value (its
// section below). PresentX, SuspendX and ResumeX are queue methods past the public chain
// (docs/d3d12_command_objects.md §5.3). A sibling layer measured two queue layouts on retail titles
// (docs/d3d12_xs_console_abi.md §4, unverified here): the older has Signal 14, Wait 15,
// GetTimestampFrequency 16, GetClockCalibration 17, GetDesc 18, SignalX 23, PresentX 30; a newer game
// OS inserts two methods before Signal, shifting to Signal 16, Wait 17, frequency 18, SignalX 25,
// PresentX 32 — a fence passed to slot 16 or 17 marks the newer layout, and the detection is shared
// across queues through the device. PresentX is wired to the Vulkan layer's Xbox presentation,
// SignalX forwards as Signal, and the unmodelled console-only slots answer S_OK (the PresentX
// section below); the first run of the executable confirms which layout.
//
// Slot numbers are those of the Windows SDK 10.0.26100 d3d12.h: ID3D12CommandQueue has 19, ID3D12Fence 11 and
// ID3D12Fence1 12. That the console keeps these public slots, and keeps device slots 8/32/36/75, is an assumption
// the shipped binaries cannot test (§3.5: the package has no queue or fence client). Every queue and fence method
// takes integer and pointer arguments only; GetDesc returns its struct through the hidden pointer in RDX, which is
// forwarded unchanged (no Xbox variant is measured, §6).
//
// The refused range is the proxy's own table: 128 queue slots and 64 fence slots, every one a thunk. The table sits
// at the end of the first page of a 64 KiB reservation whose other pages are never committed, so a slot index past
// the table makes the vtable read itself fault, at an address that names the slot (a vectored handler logs it and
// lets the fault continue), instead of jumping through whatever memory follows. A title whose vtable read lands
// further away than that reservation is not covered.
//
// Objects cross back into the real runtime wherever a device or queue method takes them, and the real object is
// passed there: queue Signal/Wait (14/15) take a fence; device CreateSharedHandle (31) a device child,
// MakeResident/Evict (34/35), SetResidencyPriority (46) and EnqueueMakeResident (50) pageables, and
// SetEventOnMultipleFenceCompletion (45) and EnqueueMakeResident fences. The heap proxies of slot 14 are pageables
// and device children too and are unwrapped in the same places. Command lists from device slot 12 come back
// console-shaped (the list proxy below): queue ExecuteCommandLists (10) unwraps the list array, the list's own
// Reset and ExecuteBundle unwrap allocator and bundle proxies, and its SetDescriptorHeaps unwraps heap proxies —
// all through proxy_real_object, so any object kind added later unwraps in every place at once.
//
// QueryInterface answers only the interfaces the proxy models (the base chain and its own interface, Fence1 when
// the real fence has it), like the device and heap proxies, so the COM identity holds and no real object leaks to
// the title. Leonida's own code reaches the real object through queue_proxy_real_queue / fence_proxy_real_fence /
// allocator_proxy_real_allocator.
//
// Command allocators (device slot 9) are the third kind: their public chain is 9 slots (Reset at 8). Slot 9 wraps
// only an allocator that answers kLeonidaCommandAllocatorTag, because a list takes the title's allocator in
// CreateCommandList and Reset and only the Vulkan layer's lists resolve a proxy there (CommandAllocator::from);
// a PC runtime allocator is handed out unchanged until a command-list proxy unwraps it at Reset.

enum class ChildKind { queue, fence, allocator };

constexpr unsigned kQueueSlots = 128;        // the table: 19 public slots plus a refused tail
constexpr unsigned kFenceSlots = 64;         // the table: 12 public slots plus a refused tail
constexpr unsigned kQueuePublicSlots = 19;   // ID3D12CommandQueue
constexpr unsigned kFencePublicSlots = 11;   // ID3D12Fence
constexpr unsigned kFence1PublicSlots = 12;  // ID3D12Fence1
constexpr unsigned kAllocatorSlots = 64;     // the table: 9 public slots plus a refused tail
constexpr unsigned kAllocatorPublicSlots = 9;  // ID3D12CommandAllocator: the base chain and Reset (slot 8)
constexpr SIZE_T kTableRegion = 64 * 1024;   // one allocation granule: the table page plus 15 never-committed pages
static_assert(kQueueSlots > kQueuePublicSlots && kFenceSlots > kFence1PublicSlots &&
                  kAllocatorSlots > kAllocatorPublicSlots,
              "the tables need a tail");
static_assert(kQueueSlots * sizeof(void*) <= 4096 && kFenceSlots * sizeof(void*) <= 4096 &&
                  kAllocatorSlots * sizeof(void*) <= 4096,
              "a table must fit in the first page of its reservation");

constexpr unsigned kQueueExecuteCommandLists = 10;   // (UINT, ID3D12CommandList* const*)
constexpr unsigned kQueueSignal = 14, kQueueWait = 15;  // (ID3D12Fence*, UINT64)
constexpr unsigned kQueueGetTimestampFrequency = 16, kQueueGetDesc = 18;  // the PC slots
bool is_console_caller(const void* address);  // defined with the device proxy
constexpr unsigned kQueueExecuteCommandListsX = 35;  // console ExecuteCommandLists(count, lists) variant
static_assert(kQueueExecuteCommandLists < kQueuePublicSlots && kQueueExecuteCommandListsX < kQueueSlots,
              "queue execution slots must stay within their respective tables");
// CopyPageMappingsBatchX(count, batches, pool, pool pages, flags), past the public chain (queue_copy_page_mappings).
constexpr unsigned kQueueCopyPageMappingsBatchX = 31;
constexpr unsigned kFenceSignal = 10;                    // ID3D12Fence::Signal(UINT64), from the CPU
constexpr unsigned kFenceCompletedValueSlot = 12;        // one past ID3D12Fence1: the completed value's address
static_assert(kFenceCompletedValueSlot >= kFence1PublicSlots && kFenceCompletedValueSlot < kFenceSlots,
              "fence slot 12 is past the public chain and inside the table");

struct FenceMirror;

// Queue slots that take queue_order: the ones that may put work on the queue or change page mappings. Not the debug
// markers (SetMarker, BeginEvent, EndEvent: 11-13 in both layouts) and not slot 18, a getter in both layouts (the
// older layout's GetDesc, the newer one's GetTimestampFrequency). The title's render thread calls slot 18 every frame
// and a queued frequency query can wait for whole submissions. Slots 16-17 and 19-20 keep the lock here:
// which of them are getters depends on the layout (16-17 are Signal/Wait in the newer one). The caller drops the lock
// for a 16/17 call that passes no fence: that is the older layout's frequency/calibration getter, not Signal/Wait.
constexpr bool queue_slot_orders_work(unsigned slot) {
    if (slot < 8 || slot > kQueueExecuteCommandListsX) return false;
    if (slot >= 11 && slot <= 13) return false;  // SetMarker, BeginEvent, EndEvent
    return slot != 18;
}

struct ChildProxy {
    void** vtable;          // offset 0: the caller's ID3D12CommandQueue* / ID3D12Fence* points here
    IUnknown* real;         // offset 8: the PC or Vulkan-layer object every thunk forwards to
    DeviceProxy* device;    // keeps the Xbox-ABI device alive; GetDevice returns it
    void** real_vtable;
    unsigned slot_count;    // public slots the real object implements; the rest of the table is refused
    std::atomic<unsigned long> refs;
    // Queues only: the console runtime layout this title was built against (docs/d3d12_xs_console_abi.md §4),
    // detected by a fence reaching slot 16/17 or SignalX slot 25. Unused on fences and allocators. A queue is
    // used from many threads, so the cache of the device's shared detection is atomic like the shared flag
    // itself.
    static constexpr unsigned char kQueueLayoutUnknown = 0;
    static constexpr unsigned char kQueueLayoutOlder = 1;   // PC order; PresentX 30, SignalX 23
    static constexpr unsigned char kQueueLayoutNewer = 2;   // +2 shift; PresentX 32, SignalX 25
    std::mutex queue_order;  // serializes queue work across a host-side page remap
    // Queue slot 31's fence, made at its first call and signalled with a rising value at each (under queue_order);
    // kept for the queue's life. It was a new fence a call: a Vulkan timeline semaphore made and destroyed per batch.
    ID3D12Fence* map_fence = nullptr;
    std::uint64_t map_fence_value = 0;
    std::atomic<unsigned char> queue_layout{kQueueLayoutUnknown};
    // Allocators only: callback and context recorded by console slot 9 (SetAllocatorCallbackX).
    std::atomic<std::uintptr_t> allocator_callback{0};
    std::atomic<std::uintptr_t> allocator_callback_context{0};
    std::atomic<FenceMirror*> mirror{nullptr};  // fence: slot 12's completed value, made on its first call
    // Queues only: the gate and write fences of list memory waits and writes (d3d12_xs_list_values.h), made at the
    // first submission that needs them, under queue_order.
    std::shared_ptr<list_values::QueueValues> values;
    ChildKind kind = ChildKind::allocator;
    ChildProxy* live_next = nullptr;  // protected by g_live_child_lock; no identity-tracking allocation
};

SRWLOCK g_live_child_lock = SRWLOCK_INIT;
constexpr std::size_t kLiveChildBuckets = 256;
ChildProxy* g_live_children[kLiveChildBuckets]{};

std::size_t live_child_bucket(const void* object) {
    const auto address = reinterpret_cast<std::uintptr_t>(object);
    return ((address >> 4) ^ (address >> 12)) & (kLiveChildBuckets - 1);
}

void register_live_child(ChildProxy* child) {
    AcquireSRWLockExclusive(&g_live_child_lock);
    auto& head = g_live_children[live_child_bucket(child)];
    child->live_next = head;
    head = child;
    ReleaseSRWLockExclusive(&g_live_child_lock);
}

void unregister_live_child(ChildProxy* child) {
    AcquireSRWLockExclusive(&g_live_child_lock);
    for (auto** at = &g_live_children[live_child_bucket(child)]; *at; at = &(*at)->live_next) {
        if (*at != child) continue;
        *at = child->live_next;
        child->live_next = nullptr;
        break;
    }
    ReleaseSRWLockExclusive(&g_live_child_lock);
}

// Identity is the live allocation and its kind, not a candidate's first word. Hold the shared lock while a
// reference is acquired so destruction cannot free the allocation; a zero count must never be resurrected.
template <ChildKind K>
ChildProxy* pin_live_child(const void* candidate) {
    ChildProxy* pinned = nullptr;
    AcquireSRWLockShared(&g_live_child_lock);
    for (auto* child = g_live_children[live_child_bucket(candidate)]; child; child = child->live_next) {
        if (child != candidate || child->kind != K) continue;
        auto refs = child->refs.load(std::memory_order_relaxed);
        while (refs && refs != (std::numeric_limits<unsigned long>::max)()) {
            if (child->refs.compare_exchange_weak(refs, refs + 1, std::memory_order_relaxed)) {
                pinned = child;
                break;
            }
        }
        break;
    }
    ReleaseSRWLockShared(&g_live_child_lock);
    return pinned;
}

template <ChildKind K>
bool is_live_child(const void* candidate) {
    bool found = false;
    AcquireSRWLockShared(&g_live_child_lock);
    for (const auto* child = g_live_children[live_child_bucket(candidate)]; child; child = child->live_next) {
        if (child != candidate || child->kind != K) continue;
        found = child->refs.load(std::memory_order_relaxed) != 0;
        break;
    }
    ReleaseSRWLockShared(&g_live_child_lock);
    return found;
}

template <ChildKind K>
struct ChildStats {
    static constexpr unsigned kSlots = K == ChildKind::queue   ? kQueueSlots
                                       : K == ChildKind::fence ? kFenceSlots
                                                               : kAllocatorSlots;
    static inline std::atomic<std::uint64_t> hits[kSlots];
    static inline std::atomic<bool> first[kSlots];
    static inline std::atomic<bool> beyond_warned[kSlots];
    static inline std::atomic<std::uint64_t> overruns{0};
    // Where the table and its reservation are: written once, before the table is handed out.
    static inline std::atomic<std::uintptr_t> table_begin{0}, table_end{0}, region_end{0};
};

template <ChildKind K>
constexpr const char* child_kind_name() {
    return K == ChildKind::queue ? "queue" : K == ChildKind::fence ? "fence" : "allocator";
}

template <ChildKind K>
const char* child_public_name(unsigned slot) {
    static constexpr const char* object[] = {"QueryInterface", "AddRef", "Release", "GetPrivateData",
                                             "SetPrivateData", "SetPrivateDataInterface", "SetName", "GetDevice"};
    static constexpr const char* queue[] = {"UpdateTileMappings", "CopyTileMappings", "ExecuteCommandLists",
                                            "SetMarker", "BeginEvent", "EndEvent", "Signal", "Wait",
                                            "GetTimestampFrequency", "GetClockCalibration", "GetDesc"};
    static constexpr const char* fence[] = {"GetCompletedValue", "SetEventOnCompletion", "Signal",
                                            "GetCreationFlags"};
    if (slot < 8) return object[slot];
    if constexpr (K == ChildKind::queue) {
        if (slot < kQueuePublicSlots) return queue[slot - 8];
        if (slot == kQueueExecuteCommandListsX) return "ExecuteCommandListsX";
    } else if constexpr (K == ChildKind::fence) {
        if (slot < kFence1PublicSlots) return fence[slot - 8];
    } else {
        if (slot == 8) return "Reset";
        if (slot == 9) return "SetAllocatorCallbackX";
    }
    return nullptr;
}

// Fixed names for the trace channel (d3d12_trace::record takes fixed strings): one per table slot, built once, in
// the trace grammar of runtime/README.md and check_d3d12_trace.py (an identifier with `::`, no spaces):
// XboxID3D12CommandQueue::Signal, and XboxID3D12CommandQueue::Slot40 for a slot past the public chain.
template <ChildKind K>
const char* child_trace_name(unsigned slot) {
    static const auto names = [] {
        static char storage[ChildStats<K>::kSlots][64];
        const char* iface = K == ChildKind::queue   ? "XboxID3D12CommandQueue"
                            : K == ChildKind::fence ? "XboxID3D12Fence"
                                                    : "XboxID3D12CommandAllocator";
        for (unsigned s = 0; s < ChildStats<K>::kSlots; ++s) {
            const char* method = child_public_name<K>(s);
            if (method)
                std::snprintf(storage[s], sizeof(storage[s]), "%s::%s", iface, method);
            else
                std::snprintf(storage[s], sizeof(storage[s]), "%s::Slot%u", iface, s);
        }
        return &storage;
    }();
    return (*names)[slot < ChildStats<K>::kSlots ? slot : 0];
}

template <ChildKind K>
void note_child_slot(unsigned slot) {
    using S = ChildStats<K>;
    if (slot >= S::kSlots) return;
    count_hit(S::hits[slot]);
    const bool first = first_call(S::first[slot]);
    const char* method = child_public_name<K>(slot);
    if (first) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) first call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    } else if (slot >= 3 && trace_enabled()) {
        std::fprintf(stderr, "[rt] d3d12_xs: %s slot %u (%s) call\n", child_kind_name<K>(), slot,
                     method ? method : "past the public chain");
    }
}

// The counters reset_device_proxy_stats clears for one kind: per-slot hits, first-call and beyond-table flags, and
// the faulted reads past the table.
template <ChildKind K>
void reset_child_stats() {
    using S = ChildStats<K>;
    for (unsigned slot = 0; slot < S::kSlots; ++slot) {
        S::hits[slot].store(0, std::memory_order_relaxed);
        S::first[slot].store(false, std::memory_order_relaxed);
        S::beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    S::overruns.store(0, std::memory_order_relaxed);
}

// ---- Fence slot 12: the completed value at an address ----
//
// Measured on the retail executable (a run past the PC chain, #6982): fence slot 12, one past ID3D12Fence1, is called
// as (value, descriptor, flags), where the descriptor is four words whose second is a code pointer, and the title
// reads the return as a pointer to the fence's 64-bit completed value. It compares that value with ~0 (device
// removed) and subtracts the value it waits for. Refused with E_NOTIMPL, the title dereferences 0x80004001.
//
// A console fence keeps that value in memory the GPU writes. Here each fence gets a mirror that stays live without
// the title calling anything. A thread-pool wait refreshes on a completion event or a timeout. A timeout can
// observe a rewind and subsequent values below the outstanding event's threshold. It does not add another
// completion event. Slot 12 and CPU Signal refresh synchronously. After device removal the callback stops waiting.
//
// The descriptor's code pointer is not called: the measured run goes on without it, and its contract (when it runs,
// on which thread, with what) is not known. The first call is logged with its arguments.
// LEONIDA_MIRROR_LAG=1: how late the mirror shows a value the GPU has reached. Each mirror also has the device's
// fence-wait thread call back the moment the timeline reaches the next value (it only notes when); the refresh that
// then writes that value adds now - that moment to the stats. LEONIDA_MIRROR_EXACT=1: that callback writes the
// mirror itself and asks for the next value, so the title sees a completion as soon as our dispatcher does (the
// thread-pool wait stays as the backstop for rewinds).
struct FenceMirror;
struct MirrorLink {  // shared with the fence-wait callbacks; destroy_child clears `mirror` before the fence goes
    SRWLOCK lock = SRWLOCK_INIT;
    FenceMirror* mirror = nullptr;            // under lock
    std::atomic<std::uint64_t> reached_value{0};  // the value the callback saw, 0: none outstanding
    std::atomic<std::int64_t> reached_at{0};      // QueryPerformanceCounter when it saw it
};
bool mirror_lag_enabled() {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_MIRROR_LAG"); return v && *v == '1'; }();
    return on;
}
bool mirror_exact_enabled() {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_MIRROR_EXACT"); return v && *v == '1'; }();
    return on;
}

struct FenceMirror {
    alignas(8) volatile std::uint64_t completed = 0;  // what the title reads through slot 12's pointer
    std::shared_ptr<MirrorLink> link;  // LEONIDA_MIRROR_LAG / LEONIDA_MIRROR_EXACT only
    ID3D12Fence* fence = nullptr;  // the real fence; the proxy holds the reference
    SRWLOCK lock = SRWLOCK_INIT;   // orders refreshes
    HANDLE event = nullptr;        // auto-reset; the real fence sets it at the armed value
    PTP_WAIT wait = nullptr;      // the thread-pool wait on the completion event
    bool stopping = false;       // guarded by lock
    std::atomic<bool> arm_failed{false};
};

void note_mirror_lag(std::int64_t ticks, bool exact);

std::uint64_t refresh_mirror(FenceMirror* m, bool exact = false) {
    AcquireSRWLockExclusive(&m->lock);
    const std::uint64_t value = m->fence->GetCompletedValue();
    const bool advanced = m->completed != value;
    if (advanced) m->completed = value;
    ReleaseSRWLockExclusive(&m->lock);
    if (advanced && m->link && value != ~std::uint64_t{0}) {
        std::uint64_t reached = m->link->reached_value.load(std::memory_order_acquire);
        if (reached && reached <= value && m->link->reached_value.compare_exchange_strong(reached, 0)) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            note_mirror_lag(now.QuadPart - m->link->reached_at.load(std::memory_order_acquire), exact);
        }
    }
    return value;
}

// Every 10 s with LEONIDA_MIRROR_LAG: from our dispatcher seeing a fence reach a value to the mirror showing it.
void note_mirror_lag(std::int64_t ticks, bool exact) {
    static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    static std::atomic<std::uint64_t> count{0}, total_us{0}, max_us{0}, over_1ms{0}, exact_count{0};
    static std::atomic<ULONGLONG> last{0};
    const std::uint64_t us = ticks > 0 ? std::uint64_t(ticks) * 1000000 / std::uint64_t(frequency.QuadPart) : 0;
    count.fetch_add(1, std::memory_order_relaxed);
    exact_count.fetch_add(exact, std::memory_order_relaxed);
    total_us.fetch_add(us, std::memory_order_relaxed);
    over_1ms.fetch_add(us > 1000, std::memory_order_relaxed);
    for (std::uint64_t m = max_us.load(); us > m && !max_us.compare_exchange_weak(m, us);) {
    }
    const ULONGLONG now = GetTickCount64();
    ULONGLONG was = last.load(std::memory_order_relaxed);
    if (!was) {
        last.compare_exchange_strong(was, now);
        return;
    }
    if (now - was < 10000 || !last.compare_exchange_strong(was, now)) return;
    const std::uint64_t n = count.exchange(0);
    std::fprintf(stderr, "[rt] d3d12_xs: fence slot 12 mirror lag (dispatcher saw the value -> mirror shows it): %llu "
                         "updates in %.0f s (%llu written by the exact callback), avg %.0f us, max %llu us, %llu over 1 ms\n",
                 static_cast<unsigned long long>(n), double(now - was) / 1000.0,
                 static_cast<unsigned long long>(exact_count.exchange(0)),
                 n ? double(total_us.exchange(0)) / double(n) : 0.0,
                 static_cast<unsigned long long>(max_us.exchange(0)), static_cast<unsigned long long>(over_1ms.exchange(0)));
}

// Asks the fence-wait thread for a callback when the fence reaches `value`. Once per call chain: the dispatcher runs a
// callback at once when it is stopping or the device is lost, which would otherwise ask again without end.
thread_local int t_mirror_arming = 0;  // this thread is inside arm_mirror_callback

void arm_mirror_callback(const std::shared_ptr<MirrorLink>& link, ID3D12Fence* fence, std::uint64_t value) {
#ifndef LEONIDA_VULKAN_DEVICE
    (void)link, (void)fence, (void)value;  // no fence-wait thread to ask: the thread-pool wait alone
#else
    if (t_mirror_arming) return;
    ++t_mirror_arming;
    rt::d3d12::fence_callback_at(fence, value, [link] {
        // Run at once inside the arming call (dispatcher stopping): this thread may hold link->lock already.
        if (t_mirror_arming) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        AcquireSRWLockExclusive(&link->lock);
        FenceMirror* m = link->mirror;
        if (m) {
            const std::uint64_t seen = m->fence->GetCompletedValue();
            if (seen != ~std::uint64_t{0}) {
                // Only a value the mirror does not show yet: one another path (slot 12, a CPU Signal) already wrote
                // would otherwise stay outstanding until the next advance and count that wait as lag.
                if (!link->reached_value.load(std::memory_order_acquire) && m->completed < seen) {
                    link->reached_at.store(now.QuadPart, std::memory_order_release);
                    link->reached_value.store(seen, std::memory_order_release);
                }
                if (mirror_exact_enabled()) refresh_mirror(m, true);
                arm_mirror_callback(link, m->fence, seen + 1);
            }
        }
        ReleaseSRWLockExclusive(&link->lock);
    });
    --t_mirror_arming;
#endif
}

void wait_for_fence_progress(FenceMirror* mirror) {
    constexpr LONGLONG kRewindPollInterval = -10 * 10'000;  // 10 ms in relative 100 ns units
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(kRewindPollInterval);
    FILETIME timeout{due.LowPart, due.HighPart};
    SetThreadpoolWait(mirror->wait, mirror->event, &timeout);
}

// Every 10 s: how many fence mirrors are live and how often their thread-pool waits ran (on a completion event or on
// the 10 ms timeout). Each run is a wake of Wine's wait-queue thread and a callback on a pool worker.
std::atomic<std::uint64_t> g_mirrors_live{0}, g_mirror_signals{0}, g_mirror_timeouts{0};
std::atomic<ULONGLONG> g_mirror_report{0};

void note_mirror_run(TP_WAIT_RESULT result) {
    (result == WAIT_OBJECT_0 ? g_mirror_signals : g_mirror_timeouts).fetch_add(1, std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_mirror_report.load(std::memory_order_relaxed);
    if (!last) {
        g_mirror_report.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10000 || !g_mirror_report.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    std::fprintf(stderr, "[rt] d3d12_xs: fence slot 12 mirrors: %llu live; in %.0f s %llu waits ended on an event, "
                         "%llu on the 10 ms timeout\n",
                 static_cast<unsigned long long>(g_mirrors_live.load(std::memory_order_relaxed)),
                 double(now - last) / 1000.0,
                 static_cast<unsigned long long>(g_mirror_signals.exchange(0, std::memory_order_relaxed)),
                 static_cast<unsigned long long>(g_mirror_timeouts.exchange(0, std::memory_order_relaxed)));
}

VOID CALLBACK on_fence_progress(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT result) {
    auto* mirror = static_cast<FenceMirror*>(context);
    note_mirror_run(result);
    const std::uint64_t value = refresh_mirror(mirror);
    if (value == ~std::uint64_t{0}) return;
    if (result == WAIT_OBJECT_0) {
        const HRESULT hr = mirror->fence->SetEventOnCompletion(value + 1, mirror->event);
        if (FAILED(hr) && !mirror->arm_failed.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: fence slot 12: SetEventOnCompletion failed (0x%08lX); the completed "
                         "value at the returned address now uses periodic refresh\n",
                         static_cast<unsigned long>(hr));
    }
    AcquireSRWLockExclusive(&mirror->lock);
    if (!mirror->stopping) wait_for_fence_progress(mirror);
    ReleaseSRWLockExclusive(&mirror->lock);
}

// The fence's mirror, made on the first slot 12 call. Null only when it cannot be allocated.
FenceMirror* fence_mirror(ChildProxy* self) {
    if (FenceMirror* m = self->mirror.load(std::memory_order_acquire)) return m;
    static SRWLOCK create_lock = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&create_lock);
    FenceMirror* m = self->mirror.load(std::memory_order_relaxed);
    if (!m && (m = new (std::nothrow) FenceMirror)) {
        m->fence = reinterpret_cast<ID3D12Fence*>(self->real);
        refresh_mirror(m);
        if (mirror_lag_enabled() || mirror_exact_enabled()) {
            m->link = std::make_shared<MirrorLink>();
            m->link->mirror = m;
            arm_mirror_callback(m->link, m->fence, m->completed + 1);
        }
        m->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m->event) m->wait = CreateThreadpoolWait(on_fence_progress, m, nullptr);
        if (m->wait) {
            g_mirrors_live.fetch_add(1, std::memory_order_relaxed);
            wait_for_fence_progress(m);
            SetEvent(m->event);  // the callback makes the first SetEventOnCompletion
        } else {
            std::fprintf(stderr, "[rt] d3d12_xs: fence slot 12: no wait for the completed value (error %lu); it "
                         "moves only on slot 12 and Signal\n", GetLastError());
            if (m->event) CloseHandle(m->event);
            m->event = nullptr;
            m->wait = nullptr;
        }
        self->mirror.store(m, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&create_lock);
    return m;
}

// Fence slot 12 (value, descriptor, flags): the address of the fence's completed value, refreshed now.
std::uint64_t fence_completed_value_address(ChildProxy* self, std::uint64_t value, std::uint64_t descriptor,
                                            std::uint64_t flags) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: fence slot 12 (value %llu, descriptor 0x%llx, flags 0x%llx): returns "
                     "the address of the fence's completed value, kept live; the descriptor is not used\n",
                     static_cast<unsigned long long>(value), static_cast<unsigned long long>(descriptor),
                     static_cast<unsigned long long>(flags));
    FenceMirror* m = fence_mirror(self);
    if (!m) {
        std::fprintf(stderr, "[rt] d3d12_xs: fence slot 12: out of memory for the completed value\n");
        return 0;
    }
    refresh_mirror(m);
    return reinterpret_cast<std::uint64_t>(&m->completed);
}

// Frees a proxy whose last reference is gone, with the real object and the device reference it held. A fence's
// mirror wait is unregistered first, waiting for a callback in flight, so none runs on a released fence.
//
// The mirror's last SetEventOnCompletion can still be pending on the real fence, and nothing cancels one: only the
// fence's destruction drops it. So the event is closed only when this release destroyed the fence. When another
// reference keeps the fence alive, the event stays open (one handle per such fence) and a later completion sets
// that event, not a closed or reused handle.
void destroy_child(ChildProxy* self) {
    unregister_live_child(self);  // no foreign Release callback runs under the registry lock
    IUnknown* real = self->real;
    DeviceProxy* device = self->device;
    FenceMirror* mirror = self->mirror.load(std::memory_order_acquire);
    if (mirror && mirror->wait) {
        AcquireSRWLockExclusive(&mirror->lock);
        mirror->stopping = true;
        SetThreadpoolWait(mirror->wait, nullptr, nullptr);
        ReleaseSRWLockExclusive(&mirror->lock);
        WaitForThreadpoolWaitCallbacks(mirror->wait, TRUE);
        CloseThreadpoolWait(mirror->wait);
        g_mirrors_live.fetch_sub(1, std::memory_order_relaxed);
    }
    if (mirror && mirror->link) {  // a fence-wait callback still pending (or running) no longer reaches the mirror
        AcquireSRWLockExclusive(&mirror->link->lock);
        mirror->link->mirror = nullptr;
        ReleaseSRWLockExclusive(&mirror->link->lock);
    }
    ID3D12Fence* const map_fence = self->map_fence;  // queue slot 31's; an async mapping in flight holds its own
    delete self;
    if (map_fence) map_fence->Release();
    const unsigned long left = real ? real->Release() : 0;
    if (mirror) {
        if (mirror->event && left == 0) CloseHandle(mirror->event);
        delete mirror;
    }
    if (device) proxy_release(device);
}

unsigned long release_child_reference(ChildProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_child(self);
    return refs;
}

template <ChildKind K>
unsigned long child_add_ref(ChildProxy* self) {
    note_child_slot<K>(1);
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <ChildKind K>
unsigned long child_release(ChildProxy* self) {
    note_child_slot<K>(2);
    return release_child_reference(self);
}

// The interfaces the proxy answers: the base chain and the object's own interface.
template <ChildKind K>
bool is_child_iid(const IID& iid) {
    if (IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
        IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12Pageable)))
        return true;
    if constexpr (K == ChildKind::queue) {
        return IsEqualIID(iid, __uuidof(ID3D12CommandQueue));
    } else if constexpr (K == ChildKind::fence) {
        return IsEqualIID(iid, __uuidof(ID3D12Fence)) || IsEqualIID(iid, __uuidof(ID3D12Fence1));
    } else {
        return IsEqualIID(iid, __uuidof(ID3D12CommandAllocator));
    }
}

bool supports_iid(IUnknown* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// An interface the real object implements but the proxy does not model is refused where the PC runtime would answer:
// logged once per interface (the first 32 of them), so a title that stops after such a refusal shows what it asked
// for. Also reached from wrap_child, for the interface a title creates a queue or fence as.
void log_refused_iid(const char* kind, const IID& iid) {
    static SRWLOCK lock = SRWLOCK_INIT;
    static IID seen[32];
    static unsigned count = 0;
    AcquireSRWLockExclusive(&lock);
    bool known = false;
    for (unsigned i = 0; i < count && !known; ++i) known = IsEqualIID(seen[i], iid) != 0;
    const bool log = !known && count < std::size(seen);
    if (log) seen[count++] = iid;
    ReleaseSRWLockExclusive(&lock);
    if (!log) return;
    std::fprintf(stderr,
                 "[rt] d3d12_xs: %s proxy refuses {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}, which the real "
                 "object implements; E_NOINTERFACE\n",
                 kind, iid.Data1, iid.Data2, iid.Data3, iid.Data4[0], iid.Data4[1], iid.Data4[2], iid.Data4[3],
                 iid.Data4[4], iid.Data4[5], iid.Data4[6], iid.Data4[7]);
}

template <ChildKind K>
HRESULT child_query_interface(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    // Only what the proxy models, and of that only what the real object implements (ID3D12Fence1 is optional).
    if (!is_child_iid<K>(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid(child_kind_name<K>(), *riid);
        return E_NOINTERFACE;
    }
    if (!supports_iid(self->real, *riid)) return E_NOINTERFACE;
    *out = self;
    child_add_ref<K>(self);
    return S_OK;
}

// ID3D12DeviceChild::GetDevice hands back the Xbox-ABI device, not the real one. Without an output it is the real
// object's own answer (what the PC runtime returns for a null pointer).
template <ChildKind K>
HRESULT child_get_device(ChildProxy* self, const IID* riid, void** out) {
    note_child_slot<K>(7);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure (bc59ef46)
    if (!riid) return E_POINTER;
    if (!out) {
        using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
        return reinterpret_cast<Fn>(self->real_vtable[7])(self->real, riid, nullptr);
    }
    if (!self->device) return E_NOINTERFACE;
    return proxy_query_interface(self->device, riid, out);
}

void** get_heap_vtable();
void** get_list_vtable();

template <ChildKind K>
void** get_child_vtable();

// The command-list proxy's helpers (defined with the list proxy below): proxy_real_object checks lists through
// list_real_or_null, since the ListProxy type is complete only there.
IUnknown* list_real_or_null(const void* object);

template <ChildKind K>
bool is_child_proxy(const void* object) {
    return is_live_child<K>(object);
}

// The real object behind a heap, queue, fence or list proxy, or null when `object` is none of them.
IUnknown* proxy_real_object(const void* object) {
    if (!object) return nullptr;
    void* const vtable = *reinterpret_cast<void* const*>(object);
    if (vtable == reinterpret_cast<void*>(get_heap_vtable())) return static_cast<const HeapProxy*>(object)->real;
    // Native objects take no registry lock or walk. A child-vtable match only selects the one identity lookup;
    // matching bytes alone still do not establish that the candidate is a live child proxy.
    if ((vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::queue>()) &&
         is_child_proxy<ChildKind::queue>(object)) ||
        (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::fence>()) &&
         is_child_proxy<ChildKind::fence>(object)) ||
        (vtable == reinterpret_cast<void*>(get_child_vtable<ChildKind::allocator>()) &&
         is_child_proxy<ChildKind::allocator>(object)))
        return static_cast<const ChildProxy*>(object)->real;
    if (IUnknown* real = list_real_or_null(object)) return real;
    return nullptr;
}

// `object` with a proxy replaced by its real object; anything else (a real object, null) unchanged.
inline std::uint64_t unwrap_object(std::uint64_t object) {
    IUnknown* real = proxy_real_object(reinterpret_cast<const void*>(object));
    return real ? reinterpret_cast<std::uint64_t>(real) : object;
}

// A copy of an array of interface pointers with every proxy replaced by its real object. Nothing here throws, since
// an exception must not leave a thunk for the title's code: up to 16 entries are kept inline (no allocation, which
// also suits a per-frame ExecuteCommandLists), a longer array is allocated with new (std::nothrow).
class UnwrappedArray {
public:
    // Copies `count` entries (a UINT in every PC prototype) of `objects`. False on allocation failure.
    bool fill(std::uint64_t objects, std::uint64_t count) {
        const auto n = static_cast<std::uint32_t>(count);
        if (!objects || !n) return true;
        std::uint64_t* copy = inline_;
        if (n > std::size(inline_)) {
            heap_.reset(new (std::nothrow) std::uint64_t[n]);
            if (!heap_) return false;
            copy = heap_.get();
        }
        const auto* in = reinterpret_cast<const std::uint64_t*>(objects);
        for (std::uint32_t i = 0; i < n; ++i) copy[i] = unwrap_object(in[i]);
        data_ = copy;
        return true;
    }
    // The copy to pass on, or 0 when there was no array (the caller's argument then goes on unchanged).
    std::uint64_t data() const { return reinterpret_cast<std::uint64_t>(data_); }

private:
    std::uint64_t inline_[16];
    std::unique_ptr<std::uint64_t[]> heap_;
    std::uint64_t* data_ = nullptr;
};

// The vtable read for a slot past a table faults in the reservation behind it; name the slot. Only reads the
// exception record and writes with WriteFile, so it is safe whatever the faulting thread holds.
template <ChildKind K>
void report_table_overrun(std::uintptr_t address) {
    using S = ChildStats<K>;
    const std::uintptr_t begin = S::table_begin.load(std::memory_order_acquire);
    const std::uintptr_t end = S::table_end.load(std::memory_order_acquire);
    const std::uintptr_t limit = S::region_end.load(std::memory_order_acquire);
    if (!end || address < end || address >= limit) return;
    S::overruns.fetch_add(1, std::memory_order_relaxed);
    char line[160];
    unsigned long long slot = (address - begin) / sizeof(void*);
    char digits[24];
    int d = 0;
    do {
        digits[d++] = static_cast<char>('0' + slot % 10);
        slot /= 10;
    } while (slot && d < 23);
    int n = 0;
    const auto put = [&](const char* text) {
        while (*text && n < static_cast<int>(sizeof(line)) - 1) line[n++] = *text++;
    };
    put("[rt] d3d12_xs: ");
    put(child_kind_name<K>());
    put(" slot ");
    while (d > 0 && n < static_cast<int>(sizeof(line)) - 1) line[n++] = digits[--d];
    put(" is past the proxy table; the vtable read faults\n");
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(n), &written, nullptr);
}

LONG CALLBACK table_overrun_handler(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
    if (record && record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = static_cast<std::uintptr_t>(record->ExceptionInformation[1]);
        report_table_overrun<ChildKind::queue>(address);
        report_table_overrun<ChildKind::fence>(address);
        report_table_overrun<ChildKind::allocator>(address);
    }
    return EXCEPTION_CONTINUE_SEARCH;  // diagnostics only: the fault goes on to the title's handlers or the crash
}

// ---- PresentX on the queue (LEONIDA_VULKAN_DEVICE) ---------------------------------------------------------
//
// The measured console queue slots (docs/d3d12_xs_console_abi.md §4): the older runtime keeps the PC order —
// Signal 14, Wait 15, frequency 16, calibration 17, GetDesc 18 — and presents at slot 30, with SignalX at 23. A
// newer game OS inserts two methods before Signal: Signal 16, Wait 17, frequency 18, calibration 19, GetDesc 20,
// SignalX 25, PresentX 32. The layout is detected once, by a fence object reaching slot 16 or 17 (the older
// layout's frequency and calibration getters take no object argument) or by a fence reaching SignalX slot 25 —
// a title that signals only through SignalX would otherwise stay undetected and lose every signal.
//
// Every other console-only queue method answers S_OK (sibling layer: title frame pacing flows through them, and a
// refusal wedges the present loop), still recorded by note_child_slot. Slot 148's AllocateUploadMemory is a list
// method, not a queue method, so it is not part of this table.

#ifdef LEONIDA_VULKAN_DEVICE

struct PresentState {
    ~PresentState() {
        // The fallback window belongs to its own thread (fallback_window_thread): that thread destroys it.
        if (window) PostMessageW(window, WM_APP + 1, 0, 0);
    }
    HWND window = nullptr;  // the runtime's own fallback window, when the title has no window to present in
    std::unique_ptr<rt::d3d12::XboxPresentation> xbox;
    std::unique_ptr<TitleHud> hud;  // LEONIDA_TITLE_HUD: counters in the window's title bar (title_hud.h)
};

void destroy_present_state(void* present_state) {
    delete static_cast<PresentState*>(present_state);
}

LRESULT CALLBACK present_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_APP + 1:  // ~PresentState
        DestroyWindow(window);
        return 0;
    case WM_CLOSE:  // the user closed the only window the run has: the run ends with it
        TerminateProcess(GetCurrentProcess(), 0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_ERASEBKGND:
        return 1;  // the frames cover the client area; nothing to erase
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

// The runtime's own window, for a title that has made none by its first PresentX. It is shown, and it lives on a
// thread of its own that reads its messages: a visible window whose thread never does is "not responding" to the
// window manager. LEONIDA_PRESENT_WINDOW=hidden keeps it hidden (frames are still rendered and captured).
struct FallbackWindowStart {
    HANDLE ready = nullptr;
    HWND window = nullptr;
    bool show = true;
};

DWORD WINAPI fallback_window_thread(LPVOID parameter) {
    auto* start = static_cast<FallbackWindowStart*>(parameter);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = present_window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"LeonidaPresentX";
    window_class.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    HWND window = nullptr;
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::fprintf(stderr, "[rt] d3d12_xs: PresentX window class failed (error %lu)\n", GetLastError());
    } else {
        // A 1920x1080 client area where the desktop has room for it; the window manager may still size it.
        RECT frame{0, 0, 1920, 1080};
        AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
        window = CreateWindowExW(0, window_class.lpszClassName, L"Leonida", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
                                 window_class.hInstance, nullptr);
        if (!window) std::fprintf(stderr, "[rt] d3d12_xs: PresentX window failed (error %lu)\n", GetLastError());
    }
    if (window && start->show) ShowWindow(window, SW_SHOWNOACTIVATE);
    start->window = window;
    SetEvent(start->ready);  // `start` belongs to the creator again from here on
    if (!window) return 0;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

HWND create_fallback_window(bool show) {
    FallbackWindowStart start;
    start.show = show;
    start.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!start.ready) return nullptr;
    HANDLE thread = CreateThread(nullptr, 0, fallback_window_thread, &start, 0, nullptr);
    if (thread) {
        WaitForSingleObject(start.ready, INFINITE);
        CloseHandle(thread);
    }
    CloseHandle(start.ready);
    return thread ? start.window : nullptr;
}

// Device slots 78 to 81 answer from the device's token counter, with no wait: that is what the retail boot was
// measured with through the story load. LEONIDA_FRAME_PACING=1 (the --first-run-ui profile sets it) hands them to
// the device's own presentation once the first PresentX has built it, with the title's interval and origin event
// replayed to its pacer; without that replay the next WaitFrameEventX would fail.
bool frame_pacing_enabled() {
    static const bool enabled = [] {
        const auto* p = std::getenv("LEONIDA_FRAME_PACING");
        return p && *p && *p != '0';
    }();
    return enabled;
}

// Lazily builds the window, presenter and XboxPresentation a console PresentX needs, once per device. Returns the
// state even when `xbox` stays null (no presentation support): the failure is logged once, not retried.
//
// Where the frames go (LEONIDA_PRESENT_WINDOW): by default the title's own top-level window when it has one by now
// (title_window.h: a GDKX title makes one and shows it, and with nothing presented in it the user sees an empty white
// window), else a window of the runtime's own, shown. "own" always uses the runtime's window; "hidden" uses the
// runtime's window and never shows it, which is what every build before this one did.
PresentState* present_state_for(DeviceProxy* device, bool splash_only = false) {
    std::lock_guard lock(device->present_state_mutex);
    if (auto* state = static_cast<PresentState*>(device->present_state)) return state;
    char mode[16] = {};
    std::size_t mode_length = 0;
    if (getenv_s(&mode_length, mode, sizeof(mode), "LEONIDA_PRESENT_WINDOW") != 0) mode[0] = '\0';
    const bool hidden = std::strcmp(mode, "hidden") == 0;
    const bool own = hidden || std::strcmp(mode, "own") == 0;
    HWND target = own ? nullptr : rt::d3d12::find_title_window(GetCurrentProcessId());
    RECT client{};
    if (target && (!GetClientRect(target, &client) || client.right <= client.left || client.bottom <= client.top))
        target = nullptr;  // minimised or zero-sized: nothing to present in
    Microsoft::WRL::ComPtr<ID3D12Resource> splash;
    if (splash_only) {
        const auto* info = rt::d3d12::vulkan_device_info(device->real);
        if (!target || !info || !info->presentation) return nullptr;
        splash.Attach(rt::package_splash::create_texture(device->real, UINT(client.right), UINT(client.bottom)));
        if (!splash) return nullptr;
    }
    std::unique_ptr<PresentState> pending(new (std::nothrow) PresentState);
    if (!pending) return nullptr;
    PresentState* state = pending.get();
    if (!splash_only) device->present_state = pending.release();
    const bool titles = target != nullptr;
    // LOCAL: LEONIDA_WINDOW=WxH resizes the title's window without waiting on its thread (SWP_ASYNCWINDOWPOS).
    if (titles) {
        if (const char* size = std::getenv("LEONIDA_WINDOW")) {
            int w = 0, h = 0;
            if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
                SetWindowPos(target, nullptr, 0, 0, w, h,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_ASYNCWINDOWPOS);
        }
    }
    if (!target) {
        state->window = create_fallback_window(!hidden);
        target = state->window;
        if (!target) return state;
        GetClientRect(target, &client);
    }
    // No device reference for this presenter: the device's outer identity is this proxy, which owns the state, so
    // a reference would be a cycle and the proxy, the presenter and the window would never go. The proxy destroys
    // the state before it releases its device (proxy_release).
    auto presenter = rt::d3d12::create_window_presenter(device->real, target, /*hold_device=*/false);
    if (!presenter) {
        if (splash_only) return nullptr;  // ordinary PresentX will still try
        std::fprintf(stderr, "[rt] d3d12_xs: no window presenter on this device; PresentX is a no-op\n");
        return state;
    }
    if (splash_only) {
        // Show the shell frame before the title starts creating its pipelines. Use the same swapchain for PresentX.
        if (!presenter->present_splash(splash.Get())) return nullptr;
        rt::startup_trace::event("package_splash_present");
    }
    // The title takes its frame tokens from device slot 80 (WaitFrameEventX), which numbers them from the device's
    // FrameTokenSource. The pacer refuses a token above the ones its source issued, so it must count from the same
    // source: with a pacer-owned one every PresentX after the first wait is refused with E_INVALIDARG (#7496, #7685).
    // With LEONIDA_FRAME_PACING=1 the pacer also answers a zero-timeout origin wait with the newest passed origin
    // (current_origin_poll), as the tree that paced these slots was measured; otherwise it is the pacer as it was.
    state->xbox = std::make_unique<rt::d3d12::XboxPresentation>(std::move(presenter), nullptr, device->frame_tokens,
                                                                frame_pacing_enabled());
    // The title registers its interval and origin event before the first PresentX builds this.
    if (const std::uint32_t interval = device->frame_interval_us.load(std::memory_order_relaxed);
        interval && frame_pacing_enabled()) {
        const HRESULT set = state->xbox->SetFrameIntervalX(nullptr, interval,
                                                           device->frame_latency.load(std::memory_order_relaxed), 0);
        const HRESULT armed = device->frame_event_scheduled.load(std::memory_order_relaxed)
                                  ? state->xbox->ScheduleFrameEventX(
                                        rt::d3d12::xbox_frame_event_origin,
                                        device->frame_event_offset_us.load(std::memory_order_relaxed), 0)
                                  : S_FALSE;
        std::fprintf(stderr, "[rt] d3d12_xs: frame interval %u us replayed (0x%08lx), origin event (0x%08lx)\n",
                     interval, static_cast<unsigned long>(set), static_cast<unsigned long>(armed));
    }
    if (TitleHud::enabled()) state->hud = std::make_unique<TitleHud>(target);
    if (splash_only) device->present_state = pending.release();
    std::fprintf(stderr, "[rt] d3d12_xs: PresentX ready (window %p: %s, client area %ldx%ld)\n",
                 static_cast<void*>(target),
                 titles ? "the title's own window" : hidden ? "the runtime's window, hidden" : "the runtime's window",
                 static_cast<long>(client.right - client.left), static_cast<long>(client.bottom - client.top));
    return state;
}

rt::d3d12::XboxPresentation* device_presentation(DeviceProxy* device) {
    if (!frame_pacing_enabled()) return nullptr;
    std::lock_guard lock(device->present_state_mutex);
    const auto* state = static_cast<PresentState*>(device->present_state);
    return state ? state->xbox.get() : nullptr;
}

std::uint64_t queue_present_x(ChildProxy* self, std::uint64_t plane_count, std::uint64_t planes,
                              std::uint64_t params) {
    PresentState* state = self->device ? present_state_for(self->device) : nullptr;
    if (!state || !state->xbox) return hr_to_u64(S_OK);  // a title still runs with nothing presented
    const HRESULT result = state->xbox->PresentX(
        static_cast<UINT>(plane_count), reinterpret_cast<const rt::d3d12::XboxPresentPlaneParameters*>(planes),
        reinterpret_cast<const rt::d3d12::XboxPresentParameters*>(params));
    if (SUCCEEDED(result) && state->hud) state->hud->note_present();
    return hr_to_u64(result);
}

#else  // !LEONIDA_VULKAN_DEVICE: this translation unit stands alone (mock and COM tests)

void destroy_present_state(void*) {}

std::uint64_t queue_present_x(ChildProxy*, std::uint64_t, std::uint64_t, std::uint64_t) {
    return hr_to_u64(S_OK);
}

#endif

// Defined with the list proxy: a submission naming a list with memory writes or waits (or one cut at a wait) goes
// part by part. False when no list of the submission has any, and the plain path of queue_execute_command_lists runs.
bool execute_with_values(ChildProxy* self, UINT count, void* const* lists);

// Queue slots 10 and 35 (the console's ExecuteCommandLists(count, lists) variant). The array is copied with each
// list proxy replaced by its real list (a bundle on the list side unwraps the same way at the console's
// ExecuteBundle); the real queue never sees a proxy.
std::uint64_t queue_execute_command_lists(ChildProxy* self, std::uint64_t count_word, std::uint64_t lists_word) {
    if (lists_word &&
        execute_with_values(self, static_cast<UINT>(count_word), reinterpret_cast<void* const*>(lists_word)))
        return 0;
    UnwrappedArray unwrapped;
    if (!unwrapped.fill(lists_word, count_word)) return hr_to_u64(E_OUTOFMEMORY);
    if (unwrapped.data()) lists_word = unwrapped.data();
    // Pass zero-extended UINT in 64-bit RDX so both 32-bit PC implementations and 64-bit mock thunks see clean count.
    using ExecuteFn = void(WINAPI*)(void*, std::uint64_t, ID3D12CommandList* const*);
    reinterpret_cast<ExecuteFn>(self->real_vtable[kQueueExecuteCommandLists])(
        self->real, static_cast<std::uint64_t>(static_cast<UINT>(count_word)),
        reinterpret_cast<ID3D12CommandList* const*>(lists_word));
    return 0;
}

// ---- Queue slot 31: CopyPageMappingsBatchX ----
//
// Measured on the retail executable (static): game_scarlett_master.exe+0xf2cc7f calls the queue's slot 31
// (`call [vtable + 0xf8]`) with EDX the batch count, R8 the batches, R9 the base of its physical-page pool, the pool's
// size in 64 KiB pages as the fifth argument and 0 as the sixth; the result is not tested. It fills a batch at +0xf2ca74
// as {UINT64 destination; UINT32 range count; const range* ranges} (24 bytes) and a range as {UINT32 kind, start,
// count} (12 bytes). The kinds: 0 maps pool pages start, start + 1, ... at the destination; 1 maps pool page `start` at
// every destination page; 2 leaves the destination unmapped; 3 leaves it as it is. Each range moves the destination on
// by count pages, whatever its kind. The same call shape is in VIctory's queue (copy_page_mappings, "newer" layout,
// where every slot from 14 on is two later than in older titles). In a boot the pool size matched the title's own
// XMemAllocatePhysicalPages count exactly. The console maps GPU page tables; here each range becomes xmem's
// copy_page_mappings_fn, which maps the physical page behind each pool page at its destination, the aliasing two
// XMemMapPhysicalPages calls with one page give.
//
// Without xmem (tests that compile this proxy alone) the slot is refused as before. A range outside the pool, of an
// unknown kind, or that xmem refuses is skipped and logged once; the call then returns E_INVALIDARG.
struct ConsolePageRange {
    std::uint32_t kind, start, count;
};
struct ConsolePageBatch {
    std::uint64_t destination;
    std::uint32_t count;
    const ConsolePageRange* ranges;
};
static_assert(sizeof(ConsolePageRange) == 12 && sizeof(ConsolePageBatch) == 24, "the title's page batch layout");

constexpr std::uint64_t kConsolePageSize = 0x10000;

bool copy_console_page_range(const ConsolePageRange& range, std::uint64_t destination, std::uint64_t pool,
                             std::uint32_t pool_pages) {
    void* const at = reinterpret_cast<void*>(destination);
    const void* const from = reinterpret_cast<const void*>(pool + std::uint64_t{range.start} * kConsolePageSize);
    switch (range.kind) {
        case 0:
            return std::uint64_t{range.start} + range.count <= pool_pages &&
                   rt::xmem::copy_page_mappings_fn(at, from, range.count, false);
        case 1:
            return range.start < pool_pages && rt::xmem::copy_page_mappings_fn(at, from, range.count, true);
        case 2:
            return rt::xmem::copy_page_mappings_fn(at, nullptr, range.count, false);
        case 3:
            return true;
        default:
            return false;
    }
}

HRESULT queue_copy_page_mappings(std::uint64_t count_word, std::uint64_t batches_word, std::uint64_t pool,
                                 std::uint64_t pool_pages_word) {
    const auto count = static_cast<std::uint32_t>(count_word);
    const auto pool_pages = static_cast<std::uint32_t>(pool_pages_word);
    const auto* const batches = reinterpret_cast<const ConsolePageBatch*>(batches_word);
    if (count && !batches) return E_INVALIDARG;
    static std::atomic<bool> first_call{true}, first_refusal{true};
    if (first_call.exchange(false, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: queue slot 31 (CopyPageMappingsBatchX) first call: %u batch(es), pool "
                     "0x%llx of %u pages\n", count, static_cast<unsigned long long>(pool), pool_pages);
    bool all = true;
    for (std::uint32_t b = 0; b < count; ++b) {
        if (!batches[b].ranges) continue;
        std::uint64_t at = batches[b].destination;
        for (std::uint32_t r = 0; r < batches[b].count; ++r) {
            const ConsolePageRange& range = batches[b].ranges[r];
            if (!copy_console_page_range(range, at, pool, pool_pages)) {
                all = false;
                if (first_refusal.exchange(false, std::memory_order_relaxed))
                    std::fprintf(stderr, "[rt] d3d12_xs: queue slot 31: range kind %u, pool pages %u+%u at 0x%llx not "
                                 "mapped (pool of %u pages, error %lu); logged once\n", range.kind, range.start,
                                 range.count, static_cast<unsigned long long>(at), pool_pages, GetLastError());
            }
            at += std::uint64_t{range.count} * kConsolePageSize;
        }
    }
    return all ? S_OK : E_INVALIDARG;
}

// PresentX is at queue slot 30 in an older runtime layout and 32 in a newer one (public corroboration, XCL; see
// docs/d3d12_vulkan.md). 30 is the default and LEONIDA_XS_PRESENT_X_SLOT sets the other. SuspendX and ResumeX have no
// default: 0 means not routed, so they answer only when their variable is set.
struct XboxQueueSlotRoutes {
    unsigned present_x = 30;  // PresentX(plane_count, planes, params)
    bool present_x_set = false;  // LEONIDA_XS_PRESENT_X_SLOT names the slot: the detected layout does not move it
    unsigned suspend_x = 0;   // SuspendX(flags)
    unsigned resume_x = 0;    // ResumeX()
};

const XboxQueueSlotRoutes& xbox_queue_slot_routes() {
    static const XboxQueueSlotRoutes routes = [] {
        XboxQueueSlotRoutes r;
        r.present_x = env_slot("LEONIDA_XS_PRESENT_X_SLOT", r.present_x, kQueuePublicSlots, kQueueSlots - 1,
                               "queue slot");
        char set[2] = {};
        r.present_x_set = GetEnvironmentVariableA("LEONIDA_XS_PRESENT_X_SLOT", set, sizeof(set)) != 0;
        r.suspend_x = env_slot("LEONIDA_XS_SUSPEND_X_SLOT", r.suspend_x, kQueuePublicSlots, kQueueSlots - 1,
                               "queue slot");
        r.resume_x = env_slot("LEONIDA_XS_RESUME_X_SLOT", r.resume_x, kQueuePublicSlots, kQueueSlots - 1,
                              "queue slot");
        // Two routes on one slot cannot both answer: PresentX keeps it, then SuspendX; the later route is unrouted.
        if (r.suspend_x && r.suspend_x == r.present_x) {
            std::fprintf(stderr, "[rt] d3d12_xs: SuspendX slot %u is PresentX's slot; SuspendX is not routed\n",
                         r.suspend_x);
            r.suspend_x = 0;
        }
        if (r.resume_x && (r.resume_x == r.present_x || r.resume_x == r.suspend_x)) {
            std::fprintf(stderr, "[rt] d3d12_xs: ResumeX slot %u is already routed; ResumeX is not routed\n",
                         r.resume_x);
            r.resume_x = 0;
        }
        return r;
    }();
    return routes;
}
// LEONIDA_SLOT31_ASYNC=1 (for runs that only need the title to go on): queue slot 31 signals the queue, hands a
// copy of the batches to one worker thread and returns; the worker waits for that signal and maps, in call order. The
// caller (the title's streaming of its GPU pool) no longer waits for the mapping: after the load it spent 2.4-4.0 s of
// every 10 s in it (speed-23), and the title's render thread waited on it. Copies of the GPU-only window made before
// the worker gets there read the old pages: wrong pixels for a moment (each mapping change is journalled, so they are
// copied again after it).
bool slot31_async() {
    static const bool on = [] {
        const char* v = std::getenv("LEONIDA_SLOT31_ASYNC");
        return v && std::strcmp(v, "1") == 0;
    }();
    return on;
}

struct AsyncMapping {
    CopyPageMappingsHook map;
    ID3D12Fence* fence = nullptr;
    std::uint64_t value = 0;
    std::vector<xmem::PageBatch> batches;
    std::vector<std::vector<xmem::PageRange>> ranges;
    std::uintptr_t pool;
    UINT pages, flags;
};

struct AsyncMapper {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<AsyncMapping> jobs;
    bool started = false;
    std::uint64_t done = 0, failed = 0;
    std::chrono::steady_clock::duration wait{}, map{};
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();

    void run() {
        using clock = std::chrono::steady_clock;
        for (;;) {
            AsyncMapping job;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return !jobs.empty(); });
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            const auto t0 = clock::now();
            HRESULT hr = job.fence->SetEventOnCompletion(job.value, nullptr);
            job.fence->Release();  // the reference the call took for this job: the queue may be gone by now
            const auto t1 = clock::now();
            bool ok = SUCCEEDED(hr) &&
                      job.map(std::uint32_t(job.batches.size()), job.batches.data(), job.pool, job.pages, job.flags);
            const auto t2 = clock::now();
            std::lock_guard lock(mutex);
            ++done;
            failed += !ok;
            wait += t1 - t0;
            map += t2 - t1;
            if (!ok && failed <= 8)
                std::fprintf(stderr, "[rt] d3d12_xs: queue slot 31 (async): a batch could not be mapped (hr 0x%08x, "
                                     "error %lu)\n", unsigned(hr), GetLastError());
            if (t2 - since >= std::chrono::seconds(10)) {
                const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
                std::fprintf(stderr, "[rt] d3d12_xs: queue slot 31 (async worker): %llu calls mapped in %.0f s "
                                     "(%llu failed): %.1f ms waiting for the queue, %.1f ms mapping; %zu queued\n",
                             static_cast<unsigned long long>(done), std::chrono::duration<double>(t2 - since).count(),
                             static_cast<unsigned long long>(failed), ms(wait), ms(map), jobs.size());
                done = failed = 0;
                wait = map = {};
                since = t2;
            }
        }
    }

    void push(AsyncMapping job) {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(job));
        if (!started) {
            started = true;
            std::thread([this] { run(); }).detach();
        }
        wake.notify_one();
    }
};

AsyncMapper& async_mapper() {
    static AsyncMapper* mapper = new AsyncMapper;  // never destroyed: its thread runs to the process's end
    return *mapper;
}

// The caller frees its batch and range arrays on return. Complete earlier work on this queue
// (including held waits), then update the aliases while later submissions are still excluded.
// CopyPageMappingsBatchX's batches, copied: the remap of a deferred call runs after the caller freed its arrays.
struct PageMappingCopy {
    std::vector<xmem::PageBatch> batches;
    std::vector<std::vector<xmem::PageRange>> ranges;
    std::uintptr_t pool = 0;
    UINT pages = 0, flags = 0;

    PageMappingCopy(UINT count, const xmem::PageBatch* in, std::uintptr_t pool_, UINT pages_, UINT flags_)
        : batches(in, in + count), ranges(count), pool(pool_), pages(pages_), flags(flags_) {
        for (UINT b = 0; b < count; ++b) {
            if (!in[b].ranges) continue;  // xmem refuses a batch with ranges but no array; it still sees none
            ranges[b].assign(in[b].ranges, in[b].ranges + in[b].count);
            batches[b].ranges = ranges[b].data();
        }
    }
};

// How long CopyPageMappingsBatchX waits at the call for the queue's earlier work before the remap is deferred.
constexpr unsigned kPageMappingWaitMs = 16;

// The production xmem seam is a range callback. Adapt it to the queue's copied batch shape when no test hook has
// been installed; the explicit hook remains useful for proxy-only tests and alternate backends.
bool copy_page_mappings_default(std::uint32_t count, const xmem::PageBatch* batches, std::uintptr_t pool,
                                std::uint32_t pool_pages, std::uint32_t flags) {
    const auto copy = rt::xmem::copy_page_mappings_fn;
    if (!copy || !count) return true;
    if (!batches || flags) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    constexpr std::uint64_t page_size = 0x10000;
    bool all = true;
    for (std::uint32_t b = 0; b < count; ++b) {
        if (!batches[b].ranges) continue;
        std::uint64_t destination = batches[b].destination;
        for (std::uint32_t r = 0; r < batches[b].count; ++r) {
            const xmem::PageRange& range = batches[b].ranges[r];
            bool ok = false;
            switch (range.kind) {
                case 0:
                    ok = std::uint64_t(range.start) + range.count <= pool_pages &&
                         copy(reinterpret_cast<void*>(destination),
                              reinterpret_cast<const void*>(pool + std::uint64_t(range.start) * page_size),
                              range.count, false);
                    break;
                case 1:
                    ok = range.start < pool_pages &&
                         copy(reinterpret_cast<void*>(destination),
                              reinterpret_cast<const void*>(pool + std::uint64_t(range.start) * page_size),
                              range.count, true);
                    break;
                case 2:
                    ok = copy(reinterpret_cast<void*>(destination), nullptr, range.count, false);
                    break;
                case 3:
                    ok = true;
                    break;
                default:
                    SetLastError(ERROR_INVALID_PARAMETER);
                    ok = false;
                    break;
            }
            if (!ok) all = false;
            destination += std::uint64_t(range.count) * page_size;
        }
    }
    return all;
}

// The caller frees its batch and range arrays on return. The aliases change after the queue's earlier work (which
// reads the old ones) and before its later work (which reads the new ones): the console orders the call on the
// queue's timeline, and its CPU does not wait for it.
// - Normally that earlier work is done within a few milliseconds: the call waits for it and remaps here, while the
//   queue's later submissions are still excluded (queue_order).
// - Work held behind a list memory wait (d3d12_xs_list_values.h) can take up to the wait's timeout, and work behind
//   it on another queue as long. After kPageMappingWaitMs the remap is deferred and the call returns: the queue's
//   write fence marks the point, the list-value releaser remaps once the GPU passes it and only then signals the
//   gate fence the queue's later work waits on, so the GPU's order is the same as at the call. Only the CPU sees the
//   call return before the remap, as on the console; an error is logged and counted then, not returned.
// With LEONIDA_LIST_VALUES=0 (no gates), without the value fences or the list-value threads, or when the queue's Wait
// for the remap cannot be submitted, the call waits for the earlier work, however long, as it did before.
// With LEONIDA_SLOT31_ASYNC=1 neither: the call signals the queue map fence and returns, and the async worker
// maps once the GPU passes it (slot31_async), in call order. The remap runs xmem's copy_page_mappings
// (g_copy_page_mappings) on every path, deferred or not.
HRESULT queue_copy_page_mappings(ChildProxy* self, UINT count, const xmem::PageBatch* batches,
                                 std::uintptr_t pool, UINT pages, UINT flags) {
    auto map = g_copy_page_mappings.load(std::memory_order_acquire);
    if (!map && rt::xmem::copy_page_mappings_fn) map = &copy_page_mappings_default;
    if (!map || !count) return S_OK;
    if (!batches || flags) return E_INVALIDARG;
    // Which implementation remaps, once: a boot's read-fault reports belong to that route's tests.
    static std::atomic<bool> route_logged{false};
    if (!route_logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: CopyPageMappingsBatchX remaps through %s\n",
                     map == &copy_page_mappings_default ? "xmem's range callback (copy_page_mappings_fn)"
                                                        : "the installed batch hook (set_copy_page_mappings_hook)");
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(self->real);
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    HRESULT hr = S_OK;
    bool deferred = false;  // the remap is the list-value releaser's now: nothing is mapped here
    // LEONIDA_SLOT31_ASYNC=1 asks for the async worker below instead of the bounded wait and the gate.
    const bool gated = list_values::enabled() && !slot31_async();
    if (gated && !self->values)
        self->values = list_values::QueueValues::create(self->device->real, queue);
    if (list_values::QueueValues* values = gated ? self->values.get() : nullptr) {
        auto& counters = list_values::counters();
        list_values::Pump& pump = list_values::Pump::get();
        const std::uint64_t point = ++values->written_value;
        hr = queue->Signal(values->written, point);
        if (FAILED(hr)) return hr;
        // The bound is kept with a high-resolution timer: a plain timed wait rounds to the system tick (15.6 ms),
        // which would defer at once or hold queue_order for two ticks. Neither handle is ever closed: a fence may still
        // set the event after a wait here gave up (auto-reset, so a late set costs one more look at the fence).
        static thread_local const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        static thread_local const HANDLE timer =
            CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const auto deadline = t0 + std::chrono::milliseconds(kPageMappingWaitMs);
        bool reached = values->written->GetCompletedValue() >= point;
        if (!reached && event && SUCCEEDED(values->written->SetEventOnCompletion(point, event))) {
            for (;;) {
                reached = values->written->GetCompletedValue() >= point;
                const auto now = clock::now();
                if (reached || now >= deadline) break;
                const auto left = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
                LARGE_INTEGER due{};
                due.QuadPart = -(std::max)(LONGLONG{1}, static_cast<LONGLONG>(left / 100));  // relative, 100 ns units
                if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    const HANDLE handles[2] = {event, timer};
                    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                } else {
                    WaitForSingleObject(event, static_cast<DWORD>((left + 999999) / 1000000));
                }
            }
        }
        list_values::raise_to(counters.remap_wait_us_max,
                              static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                             clock::now() - t0)
                                                             .count()));
        // After stop_list_values (the process is ending) the queue's gates are not signalled any more and its earlier
        // work may never run: the remap is done now rather than holding queue_order until ExitProcess.
        if (!reached && !pump.stopped()) {
            std::shared_ptr<PageMappingCopy> copy;
            try {
                copy = std::make_shared<PageMappingCopy>(count, batches, pool, pages, flags);
            } catch (const std::bad_alloc&) {
            }
            const std::uint64_t gate = values->gate_value + 1;
            // The queue's Wait first: unless its later work is held behind the remap, the remap is not deferred (a
            // later call could remap at once, and this one, run last, would win).
            if (copy && pump.start() && SUCCEEDED(queue->Wait(values->gate, gate))) {
                values->gate_value = gate;
                values->written->SetEventOnCompletion(point, pump.wake());
                if (pump.add_action(self->values, gate, point, [copy, map] {
                        if (map(static_cast<std::uint32_t>(copy->batches.size()), copy->batches.data(), copy->pool,
                                copy->pages, copy->flags))
                            return;
                        const DWORD error = GetLastError();
                        list_values::bump(list_values::counters().remaps_failed);
                        static std::atomic<unsigned> logged{0};
                        if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
                            std::fprintf(stderr, "[rt] CopyPageMappingsBatchX (deferred) failed: error %lu "
                                                 "batches=%zu pool=%p pages=%u\n",
                                         error, copy->batches.size(), reinterpret_cast<void*>(copy->pool),
                                         copy->pages);
                    })) {
                    const std::uint64_t so_far =
                        counters.remaps_deferred.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (so_far == 1 || (so_far & (so_far - 1)) == 0)
                        std::fprintf(stderr, "[rt] d3d12_xs: CopyPageMappingsBatchX waits behind held queue work: "
                                             "the remap runs when the queue gets there (%llu deferred so far)\n",
                                     static_cast<unsigned long long>(so_far));
                    deferred = true;
                }
                // Otherwise stop_list_values came in between: nothing opens the gate any more; the remap is done now.
            } else {
                hr = values->written->SetEventOnCompletion(point, nullptr);  // not deferred: wait, as before
                if (FAILED(hr)) return hr;
            }
        }
        if (!deferred) {
            if (values->written->GetCompletedValue() == UINT64_MAX) hr = DXGI_ERROR_DEVICE_REMOVED;
            else list_values::bump(counters.remaps_now);
        }
    } else {
        if (!self->map_fence) {
            hr = self->device->real->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&self->map_fence));
            if (FAILED(hr)) return hr;
            if (!self->map_fence) return E_FAIL;
        }
        ID3D12Fence* const fence = self->map_fence;
        const std::uint64_t value = ++self->map_fence_value;
        hr = queue->Signal(fence, value);
        if (SUCCEEDED(hr) && slot31_async()) {
            AsyncMapping job{map, fence, value, {}, {}, pool, pages, flags};
            job.batches.assign(batches, batches + count);
            job.ranges.resize(count);
            for (UINT b = 0; b < count; ++b) {
                if (batches[b].count && batches[b].ranges)
                    job.ranges[b].assign(batches[b].ranges, batches[b].ranges + batches[b].count);
                job.batches[b].ranges = job.ranges[b].data();
            }
            fence->AddRef();  // the worker releases it: destroy_child releases the queue's own reference
            async_mapper().push(std::move(job));
            return S_OK;
        }
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(value, nullptr);
        if (SUCCEEDED(hr) && fence->GetCompletedValue() == UINT64_MAX) hr = DXGI_ERROR_DEVICE_REMOVED;
    }
    const auto t1 = clock::now();
    if (SUCCEEDED(hr) && !deferred && !map(count, batches, pool, pages, flags)) {
        const DWORD error = GetLastError();
        hr = error ? HRESULT_FROM_WIN32(error) : E_FAIL;
    }
    const auto t2 = clock::now();
    // Every 10 s: how long the calls of slot 31 kept the caller (the title's render thread) waiting for the queue's
    // earlier work, and then mapping.
    static std::mutex stats_mutex;
    static struct {
        std::uint64_t calls = 0, ranges = 0;
        clock::duration wait{}, map{}, max{};
        clock::time_point since = clock::now();
    } stats;
    {
        std::lock_guard lock(stats_mutex);
        ++stats.calls;
        stats.ranges += count;
        stats.wait += t1 - t0;
        stats.map += t2 - t1;
        stats.max = (std::max)(stats.max, t2 - t0);
        if (t2 - stats.since >= std::chrono::seconds(10)) {
            const auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
            std::fprintf(stderr, "[rt] d3d12_xs: queue slot 31: %llu calls (%llu batches) in %.0f s: %.1f ms waiting "
                                 "for the queue, %.1f ms mapping, %.1f ms the longest call\n",
                         static_cast<unsigned long long>(stats.calls), static_cast<unsigned long long>(stats.ranges),
                         std::chrono::duration<double>(t2 - stats.since).count(), ms(stats.wait), ms(stats.map),
                         ms(stats.max));
            stats = {};
            stats.since = t2;
        }
    }
    return hr;
}

template <ChildKind K, unsigned Slot>
std::uint64_t WINAPI child_slot(ChildProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_query_interface<K>(self, reinterpret_cast<const IID*>(a1),
                                                  reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return child_add_ref<K>(self);
    } else if constexpr (Slot == 2) {
        return child_release<K>(self);
    } else if constexpr (Slot == 7) {
        d3d12_trace::record(child_trace_name<K>(Slot));
        return hr_to_u64(child_get_device<K>(self, reinterpret_cast<const IID*>(a1),
                                             reinterpret_cast<void**>(a2)));
    } else if constexpr (K == ChildKind::fence && Slot == kFenceCompletedValueSlot) {
        // Past ID3D12Fence1, so whether the real fence has Fence1 does not matter here.
        note_child_slot<K>(Slot);
        d3d12_trace::record(child_trace_name<K>(Slot));
        return fence_completed_value_address(self, a1, a2, a3);
    } else {
        note_child_slot<K>(Slot);
        // A reentrant Release can drop the caller's last reference while this call is still forwarding.
        // The internal references also contribute to AddRef and Release return counts until this call returns.
        using Pin = std::unique_ptr<ChildProxy, decltype(&release_child_reference)>;
        Pin queue_pin(nullptr, &release_child_reference), fence_pin(nullptr, &release_child_reference);
        if constexpr (K == ChildKind::queue) {
            queue_pin.reset(pin_live_child<ChildKind::queue>(self));
            if (!queue_pin) return hr_to_u64(E_INVALIDARG);
            if constexpr (Slot == kQueueSignal || Slot == kQueueWait || Slot == 16 || Slot == 17 ||
                          Slot == 23 || Slot == 25)
                fence_pin.reset(pin_live_child<ChildKind::fence>(reinterpret_cast<const void*>(a1)));
        }
        const auto fence_argument = [&]() {
            return fence_pin ? fence_pin->real : proxy_real_object(reinterpret_cast<const void*>(a1));
        };
        std::unique_lock<std::mutex> queue_order;
        // Both queue-order fixes: only work that orders on the queue takes queue_order. The markers do not, and
        // neither does slot 18 - an ExecuteCommandListsX holds it for its whole replay, which can include building a
        // pipeline for minutes, while the title's render thread asks the timestamp frequency every frame (a frozen
        // frame loop, measured). 16/17 keep the lock when a fence comes with them (the newer layout's Signal/Wait);
        // without one they are the older layout's frequency/calibration getter. 19/20 are the newer layout's
        // GetClockCalibration and GetDesc (PC 17/18 shifted by two): getters there, so no lock once that layout is
        // known (the title uses it natively); in the older layout they keep it. Queues only: for other proxy kinds
        // a1 is not an object pointer and must not be read.
        if constexpr (K == ChildKind::queue) {
            bool orders = queue_slot_orders_work(Slot);
            if (orders && (Slot == 16 || Slot == 17))
                orders = fence_argument() != nullptr;
            if (orders && (Slot == 19 || Slot == 20)) {
                const unsigned char own = self->queue_layout.load(std::memory_order_relaxed);
                const bool newer = own == ChildProxy::kQueueLayoutNewer ||
                                   (own == ChildProxy::kQueueLayoutUnknown &&
                                    self->device->console_layout.load(std::memory_order_relaxed) ==
                                        ChildProxy::kQueueLayoutNewer);
                if (newer) orders = false;
            }
            if (orders) queue_order = std::unique_lock(self->queue_order);
        }
        // CopyPageMappingsBatchX when something can remap: a hook (set_copy_page_mappings_hook) or xmem. Without
        // either (a proxy compiled without xmem) the slot keeps the answer every unmodelled queue slot past the
        // public chain gets below: console-only S_OK, or E_NOTIMPL on a queue whose presentation is managed outside.
        if constexpr (K == ChildKind::queue && Slot == kQueueCopyPageMappingsBatchX) {
            if (g_copy_page_mappings.load(std::memory_order_acquire) || rt::xmem::copy_page_mappings_fn) {
                const HRESULT hr = queue_copy_page_mappings(
                    self, UINT(a1), reinterpret_cast<const xmem::PageBatch*>(a2), a3, UINT(a4), UINT(a5));
                if (FAILED(hr)) {
                    static std::atomic<unsigned> failures{0};
                    if (failures.fetch_add(1, std::memory_order_relaxed) < 8)
                        std::fprintf(stderr, "[rt] CopyPageMappingsBatchX failed: hr=0x%08x batches=%u pool=%p "
                                     "pages=%u flags=%u\n", unsigned(hr), UINT(a1), reinterpret_cast<void*>(a3),
                                     UINT(a4), UINT(a5));
                }
                return hr_to_u64(d3d12_trace::result(child_trace_name<K>(Slot), hr));
            }
        }
        if constexpr (K == ChildKind::queue && Slot == kQueueExecuteCommandListsX) {
            d3d12_trace::record(child_trace_name<K>(Slot));
            return queue_execute_command_lists(self, a1, a2);
        }
        if constexpr (K == ChildKind::allocator && Slot == 9) {
            // Console-only ID3D12CommandAllocator::SetAllocatorCallbackX(callback, context, ...), observed past the
            // 9-slot PC chain on the retail executable (call shape: function pointer, context, plus trailing words).
            // The PC/Vulkan layer manages command-buffer backing directly without invoking a title allocator callback,
            // so record the callback and context on the proxy and return S_OK while leaving slots 10+ refused.
            self->allocator_callback.store(static_cast<std::uintptr_t>(a1), std::memory_order_relaxed);
            self->allocator_callback_context.store(static_cast<std::uintptr_t>(a2), std::memory_order_relaxed);
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: allocator slot 9 (SetAllocatorCallbackX) recorded "
                             "(callback=0x%llx context=0x%llx a3=0x%llx a4=0x%llx)\n",
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot));
            return hr_to_u64(S_OK);
        }
        if constexpr (K == ChildKind::queue) {
            // Layout detection (the comment at the PresentX section): a fence reaching slot 16 or 17 is the newer
            // layout's Signal/Wait; anything else there is the older layout's frequency/calibration getter. A
            // fence at slot 14 or 15 is the older Signal/Wait; a non-fence argument there is one of the newer
            // layout's two inserted console methods.
            // A queue made after another one detected the layout inherits it from the device; undetected queues
            // assume the older layout, whose unshifted slots are also the PC chain. A fence reaching SignalX
            // slot 25 while the layout is still unknown is itself the newer-layout evidence: a title that
            // signals only through SignalX never reaches Signal/Wait at 16/17, and an unanswered SignalX
            // returns S_OK having signalled nothing, leaving a waiter on that fence value hanging.
            const bool newer = [&] {
                const unsigned char own = self->queue_layout.load(std::memory_order_relaxed);
                if (own == ChildProxy::kQueueLayoutNewer) return true;
                const unsigned char shared =
                    self->device->console_layout.load(std::memory_order_relaxed);
                return own == ChildProxy::kQueueLayoutUnknown && shared == ChildProxy::kQueueLayoutNewer;
            }();
            if (Slot == 16 || Slot == 17) {
                if (IUnknown* fence = fence_argument()) {
                    self->queue_layout.store(ChildProxy::kQueueLayoutNewer, std::memory_order_relaxed);
                    self->device->console_layout.store(ChildProxy::kQueueLayoutNewer,
                                                       std::memory_order_relaxed);
                    using Fn = HRESULT(WINAPI*)(IUnknown*, IUnknown*, UINT64);
                    return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[kQueueSignal + (Slot - 16)])(
                        self->real, fence, a2));
                }
                if (newer) return hr_to_u64(S_OK);  // proven newer: a non-fence Signal/Wait arg is malformed,
                // not evidence of the older layout — do not downgrade the shared detection on it
                if (self->queue_layout.load(std::memory_order_relaxed) ==
                    ChildProxy::kQueueLayoutUnknown) {
                    self->queue_layout.store(ChildProxy::kQueueLayoutOlder, std::memory_order_relaxed);
                    self->device->console_layout.store(ChildProxy::kQueueLayoutOlder,
                                                       std::memory_order_relaxed);
                }
            } else if ((Slot == 14 || Slot == 15) && !fence_argument() &&
                       newer) {
                return hr_to_u64(S_OK);  // the newer layout's two inserted console methods, unmeasured
            }
            if (newer && Slot >= 18 && Slot <= 20) {
                // Newer layout: frequency 18, calibration 19, GetDesc 20 are the PC slots 16-18 shifted.
                d3d12_trace::record(child_trace_name<K>(Slot));
                using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t);
                auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot - 2]);
                return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            }
            if (Slot == (newer ? 25u : 23u) ||
                (Slot == 25u && self->queue_layout.load(std::memory_order_relaxed) ==
                                    ChildProxy::kQueueLayoutUnknown &&
                 self->device->console_layout.load(std::memory_order_relaxed) ==
                     ChildProxy::kQueueLayoutUnknown)) {
                // SignalX: console Signal (older 23, newer 25); the extra X word is dropped. The slot-25
                // alternative is the fence itself carrying the layout evidence, per the comment above; a
                // proven-older queue still leaves slot 25 to the console-only no-op, since its fence-taking
                // methods sit at 14/15/23 there.
                if (IUnknown* fence = fence_argument()) {
                    if (Slot == 25u && !newer) {
                        self->queue_layout.store(ChildProxy::kQueueLayoutNewer,
                                                 std::memory_order_relaxed);
                        self->device->console_layout.store(ChildProxy::kQueueLayoutNewer,
                                                           std::memory_order_relaxed);
                    }
                    static std::atomic<bool> logged{false};
                    if (!logged.exchange(true, std::memory_order_relaxed))
                        std::fprintf(stderr, "[rt] d3d12_xs: SignalX (queue slot %u) forwarded as Signal\n", Slot);
                    using Fn = HRESULT(WINAPI*)(IUnknown*, IUnknown*, UINT64);
                    return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[kQueueSignal])(self->real, fence, a2));
                }
                return hr_to_u64(S_OK);
            }
            // CopyPageMappingsBatchX (queue_copy_page_mappings), ahead of the presentation routes and the
            // console-only S_OK below; without xmem linked it stays with the console-only answer.
            if constexpr (Slot == kQueueCopyPageMappingsBatchX) {
                if (Slot >= self->slot_count && rt::xmem::copy_page_mappings_fn) {
                    d3d12_trace::record(child_trace_name<K>(Slot));
                    return hr_to_u64(queue_copy_page_mappings(a1, a2, a3, a4));
                }
            }
            // PresentX, SuspendX and ResumeX at their routed slots (xbox_queue_slot_routes). The default PresentX
            // slot is the older layout's; the newer layout has it two slots later unless the variable names it. A
            // presentation attached with set_presentation_handler answers first; without one PresentX goes to the
            // device's own presenter (queue_present_x).
            const XboxQueueSlotRoutes& qroutes = xbox_queue_slot_routes();
            const unsigned present_x_slot =
                newer && !qroutes.present_x_set ? qroutes.present_x + 2 : qroutes.present_x;
            if (Slot >= self->slot_count) {
                HRESULT hr = S_OK;
                bool external_queue = false;
                if (call_presentation(self->device, [&](const PresentationHandler& p) {
                        external_queue = self->device->external_presentation_mode;
                        if (Slot == present_x_slot && p.present_x) {
                            hr = p.present_x(p.context, static_cast<UINT>(a1), reinterpret_cast<const void*>(a2),
                                             reinterpret_cast<const void*>(a3));
                            return true;
                        }
                        if (Slot == qroutes.suspend_x && p.suspend_x) {
                            hr = p.suspend_x(p.context, static_cast<UINT>(a1));
                            return true;
                        }
                        if (Slot == qroutes.resume_x && p.resume_x) {
                            hr = p.resume_x(p.context);
                            return true;
                        }
                        return false;
                    }))
                    return hr_to_u64(hr);
                // An explicitly managed queue has no fallback after detachment. An unmapped
                // presentation slot cannot silently open a second window or acknowledge a suspend.
                if (external_queue && ((Slot >= 30 && Slot <= 32) || Slot == present_x_slot ||
                                       Slot == qroutes.suspend_x || Slot == qroutes.resume_x))
                    return hr_to_u64(E_NOTIMPL);
                if (Slot == present_x_slot) {
                    const std::uint64_t presented = queue_present_x(self, a1, a2, a3);
                    // LOCAL: one trace line per present, so a run's presents can be counted.
                    d3d12_trace::record("XboxID3D12CommandQueue::PresentX",
                                        static_cast<HRESULT>(presented) < 0 ? "failed" : "ok");
                    return presented;
                }
            }
            if (Slot >= self->slot_count || (newer && (Slot == 14 || Slot == 15))) {
                d3d12_trace::record(child_trace_name<K>(Slot), "console-only");
                return hr_to_u64(S_OK);  // unmodelled console queue methods: frame pacing flows through them
            }
        }
        if (Slot >= self->slot_count) {
            bool expected = false;
            if (ChildStats<K>::beyond_warned[Slot].compare_exchange_strong(expected, true,
                                                                            std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[rt] d3d12_xs: %s slot %u is beyond the PC %s vtable (%u slots); returning E_NOTIMPL "
                             "(args 0x%llx 0x%llx 0x%llx 0x%llx)\n",
                             child_kind_name<K>(), Slot, child_kind_name<K>(), self->slot_count,
                             static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                             static_cast<unsigned long long>(a3), static_cast<unsigned long long>(a4));
            }
            d3d12_trace::record(child_trace_name<K>(Slot), "unsupported");
            return hr_to_u64(E_NOTIMPL);
        }
        d3d12_trace::record(child_trace_name<K>(Slot));  // "called": the real object's result is not inspected
        if constexpr (K == ChildKind::queue && Slot == kQueueExecuteCommandLists) {
            return queue_execute_command_lists(self, a1, a2);
        }
        if constexpr (K == ChildKind::queue && Slot == kQueueGetDesc) {
            // Queue slot 18 is GetDesc on the PC: a struct return, 16 bytes written through the hidden pointer. The
            // retail title calls slot 18 with one pointer to 8 bytes, the shape of GetTimestampFrequency(UINT64*),
            // which the console layout with two methods inserted at 14/15 has there (PC slot 16). Forwarded as
            // GetDesc it wrote a whole description over the title's 8 bytes and what follows them. The title's call
            // therefore goes to the frequency getter, the only reading that cannot overrun its argument; a host
            // caller (set_console_caller, as device slots 61 and 74) keeps the PC GetDesc.
            if (is_console_caller(_ReturnAddress())) {
                using Fn = HRESULT(WINAPI*)(void*, UINT64*);
                return hr_to_u64(reinterpret_cast<Fn>(self->real_vtable[kQueueGetTimestampFrequency])(
                    self->real, reinterpret_cast<UINT64*>(a1)));
            }
        }
        if constexpr (K == ChildKind::queue && (Slot == kQueueSignal || Slot == kQueueWait))
            a1 = fence_pin ? reinterpret_cast<std::uint64_t>(fence_pin->real) : unwrap_object(a1);
        UnwrappedArray unwrapped_lists;
        if constexpr (K == ChildKind::queue && Slot == 10) {
            if (!unwrapped_lists.fill(a2, a1)) return hr_to_u64(E_OUTOFMEMORY);
            if (unwrapped_lists.data()) a2 = unwrapped_lists.data();
        }

        // The PC vtable entry with the real object as `this`; integer/pointer arguments, the hidden struct-return
        // pointer and stack arguments keep their positions, as in the device proxy.
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        if constexpr (K == ChildKind::fence && Slot == kFenceSignal) {
            const std::uint64_t result = fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            if (FenceMirror* m = self->mirror.load(std::memory_order_acquire)) refresh_mirror(m);
            return result;
        }
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    }
}

template <ChildKind K, std::size_t... I>
void fill_child_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&child_slot<K, static_cast<unsigned>(I)>)), ...);
}

// True when the fault logger is installed (once per process; a failure is logged once).
bool install_table_overrun_handler() {
    static const bool installed = [] {
        if (AddVectoredExceptionHandler(0, &table_overrun_handler)) return true;
        std::fprintf(stderr, "[rt] d3d12_xs: AddVectoredExceptionHandler failed (error %lu); a slot past a queue or "
                     "fence proxy table faults without being named\n", GetLastError());
        return false;
    }();
    return installed;
}

template <ChildKind K>
void** get_child_vtable() {
    static void** vtable = [] {
        using S = ChildStats<K>;
        constexpr std::size_t bytes = S::kSlots * sizeof(void*);
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        const SIZE_T page = system.dwPageSize;
        if (void* base = page >= bytes ? VirtualAlloc(nullptr, kTableRegion, MEM_RESERVE, PAGE_NOACCESS) : nullptr) {
            if (VirtualAlloc(base, page, MEM_COMMIT, PAGE_READWRITE)) {
                auto* table = reinterpret_cast<void**>(static_cast<char*>(base) + page - bytes);
                fill_child_vtable<K>(table, std::make_index_sequence<S::kSlots>{});
                DWORD old = 0;
                if (!VirtualProtect(base, page, PAGE_READONLY, &old))  // the table works, but stays writable
                    std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table stays writable (VirtualProtect error %lu)\n",
                                 child_kind_name<K>(), GetLastError());
                S::table_begin.store(reinterpret_cast<std::uintptr_t>(table), std::memory_order_release);
                S::region_end.store(reinterpret_cast<std::uintptr_t>(base) + kTableRegion, std::memory_order_release);
                // table_end is what "guarded" means (command_object_tables_guarded, the fault logger): set only when
                // a fault past the table will be named. Without the logger the region still faults, unnamed.
                if (install_table_overrun_handler())
                    S::table_end.store(reinterpret_cast<std::uintptr_t>(base) + page, std::memory_order_release);
                return table;
            }
            VirtualFree(base, 0, MEM_RELEASE);
        }
        // No reservation: the table still works, but a slot past it is not caught.
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy table without a guard region\n", child_kind_name<K>());
        static void* storage[S::kSlots];
        fill_child_vtable<K>(storage, std::make_index_sequence<S::kSlots>{});
        return storage;
    }();
    return vtable;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
template <ChildKind K>
HRESULT wrap_child(IUnknown* created, DeviceProxy* device, const IID* riid, void** out) {
    if (!created) return E_FAIL;
    ChildProxy* proxy = new (std::nothrow) ChildProxy;
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_child_vtable<K>();
    proxy->real = created;
    proxy->device = device;
    proxy->real_vtable = *reinterpret_cast<void***>(created);
    if constexpr (K == ChildKind::queue) {
        proxy->slot_count = kQueuePublicSlots;
    } else if constexpr (K == ChildKind::fence) {
        proxy->slot_count = supports_iid(created, __uuidof(ID3D12Fence1)) ? kFence1PublicSlots : kFencePublicSlots;
    } else {
        proxy->slot_count = kAllocatorPublicSlots;
    }
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->kind = K;
    proxy_add_ref(device);  // the object keeps its Xbox-ABI device alive for GetDevice
    register_live_child(proxy);  // before QueryInterface publishes the output pointer

    const HRESULT hr = child_query_interface<K>(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_child(proxy);  // no reference escaped
        return hr;
    }
    // Titles create fences at run time: the first proxy of each kind is logged, every one only with XS_TRACE.
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: %s proxy %p wraps %p (device proxy %p, %u public slots)\n",
                     child_kind_name<K>(), static_cast<void*>(proxy), static_cast<void*>(created),
                     static_cast<void*>(device), proxy->slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// ---- Command list proxy ---------------------------------------------------------------------------------
//
// Device slot 12 (CreateCommandList) hands the title a console-shaped command list: the console vtable order, a
// 160-slot table, and the object fields the console SDK's inline writers touch (docs/d3d12_xs_console_abi.md
// §5). The write cursor (+0x18) is pinned at both limits (+0x28, +0x30), so a title always finds the record
// buffer full and takes the vtable path for every command — the proxy never has to interpret inline records.
// The flag bytes (+0x140..+0x143), the root-parameter header table (+0x344) and the pipeline record (+0x544)
// stay zeroed: they are only read while writing a record, which never happens at the limit.
//
// The console slot order keeps the PC order through slot 26 (measured: 9, 10, 12-17, 19, 20, 25, 26), places
// four console-only methods at 27-30, then the PC chain resumes shifted +4 — confirmed at console 32->PC 28
// (SetDescriptorHeaps), 33->29, 41->37, 43->39, 45->41, 48->44, 51->47 and 53->49, and the same shift fills the
// block through console 59 -> PC 55. The console has no PIX markers: console 60 is EndEvent (PC 58) and 61 is
// ExecuteIndirect (PC 59, package-measured). Console 146 is the enhanced Barrier taking one D3D12_BARRIER_GROUP
// (package-confirmed; PC equivalent is GCL7 slot 80). Console-only slots — the 27-30 extras, AllocateUploadMemory
// (148), the disputed tail past 61 and the newer-layout +2 tail — are recorded with their first argument words
// and return 0 without a PC call: a wrong forward is worse than a clean refusal.

constexpr unsigned kListSlots = 160;
// ---- Upload memory (console list slot 148, AllocateUploadMemory) ----
//
// On the console the CPU and GPU share one address space: the pointer AllocateUploadMemory(size, alignment)
// returns is written by the CPU and handed to the GPU unchanged — the package measures it going straight into a
// root SRV (docs/d3d12_xs_console_abi.md §5.3, d3d12_command_objects.md §3.3a). A PC has two address spaces, so
// each arena chunk is a persistently-mapped UPLOAD-heap buffer: the title gets the CPU pointer and writes the
// very memory the GPU samples, and the root-descriptor setters translate a chunk address to the buffer's device
// address (the same backing memory) before forwarding. Translation covers the six root VA setters (console
// 41-46); VAs inside view structs (IASetVertexBuffers/IASetIndexBuffer) and ExecuteIndirect arguments are not
// rewritten yet and stay title addresses.

struct UploadChunk {
    UploadChunk* next;
    ID3D12Resource* resource;      // the mapped buffer; released with the chunk
    std::uint8_t* cpu_base;
    D3D12_GPU_VIRTUAL_ADDRESS gpu_base;
    std::size_t size;
    std::size_t used;
};

struct UploadArena {
    UploadChunk* chunks = nullptr;
    std::mutex lock;
};

constexpr std::size_t kUploadChunkMin = 8 << 20;  // one chunk serves a title's whole frame of uploads

// LEONIDA_UPLOAD_LOG=1 (diagnostic): each upload chunk (its CPU base, device address and size), and the first 96 and
// then every 8192nd AllocateUploadMemory result as an offset into its chunk, so the addresses the title builds
// descriptors from can be matched with the memory it was handed.
inline bool upload_log() {
    static const bool on = [] {
        const char* v = std::getenv("LEONIDA_UPLOAD_LOG");
        return v && *v == '1';
    }();
    return on;
}
inline void log_upload_result(const UploadChunk* chunk, std::uint64_t result, std::uint64_t size,
                              std::uint64_t alignment) {
    if (!upload_log() || !chunk) return;
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t n = ++calls;
    if (n > 96 && (n % 8192)) return;
    const std::uint64_t base = reinterpret_cast<std::uint64_t>(chunk->cpu_base);
    std::fprintf(stderr,
                 "[rt] upload-log: #%llu result 0x%llx = chunk 0x%llx + offset 0x%llx (size %llu, alignment %llu; "
                 "chunk device address 0x%llx)\n",
                 static_cast<unsigned long long>(n), static_cast<unsigned long long>(result),
                 static_cast<unsigned long long>(base), static_cast<unsigned long long>(result - base),
                 static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment),
                 static_cast<unsigned long long>(chunk->gpu_base));
}

UploadChunk* upload_chunk_new(DeviceProxy* device, std::size_t size) {
    auto* chunk = new (std::nothrow) UploadChunk{};
    if (!chunk) return nullptr;
    chunk->size = size;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* resource = nullptr;
    const HRESULT hr = device->real->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&resource));
    void* cpu = nullptr;
    if (SUCCEEDED(hr) && resource && SUCCEEDED(resource->Map(0, nullptr, &cpu)) && cpu &&
        resource->GetGPUVirtualAddress() != 0) {
        chunk->resource = resource;
        chunk->cpu_base = static_cast<std::uint8_t*>(cpu);
        chunk->gpu_base = resource->GetGPUVirtualAddress();
        // LEONIDA_UPLOAD_ALIAS=1 (experiment): the title takes the pointer AllocateUploadMemory hands it for a GPU
        // address too (the console's memory is unified) and writes it into the descriptors it builds itself. Entered in
        // the title VA table, the chunk's CPU range answers with the chunk's console VA (its device address minus
        // heap_base), so a V# the title wrote with such a pointer is rewritten to the chunk instead of left empty. A
        // chunk below the arena has a negative console VA, usable in a V# only with LEONIDA_IMPORT_BELOW_ARENA=1.
        static const bool alias = [] {
            const char* v = std::getenv("LEONIDA_UPLOAD_ALIAS");
            return v && *v == '1';
        }();
        if (alias) {
            const bool added = leonida::title_va_aliases().add(reinterpret_cast<std::uint64_t>(chunk->cpu_base),
                                                               chunk->size, chunk->gpu_base);
            std::fprintf(stderr, "[rt] d3d12_xs: upload chunk 0x%llx (%llu bytes) entered as an alias of console VA "
                                 "0x%llx: %s\n",
                         static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(chunk->cpu_base)),
                         static_cast<unsigned long long>(chunk->size), static_cast<unsigned long long>(chunk->gpu_base),
                         added ? "ok" : "refused");
        }
        return chunk;
    }
    if (resource) resource->Release();
    // A device that cannot hand out a mapped buffer (a mock, or an older layer without buffer device
    // addresses) still gets memory to write; upload_translate leaves its addresses alone.
    auto* cpu_fallback = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!cpu_fallback) {
        delete chunk;
        return nullptr;
    }
    chunk->cpu_base = cpu_fallback;
    chunk->gpu_base = reinterpret_cast<D3D12_GPU_VIRTUAL_ADDRESS>(cpu_fallback);
    return chunk;
}

std::uint64_t upload_alloc(DeviceProxy* device, std::uint64_t size, std::uint64_t alignment) {
    if (!device || !size) return 0;
    alignment = alignment < 16 ? 16 : alignment;  // every measured call asked for 16
    // Refuse what cannot be served before any arithmetic runs: only power-of-two alignments are measured,
    // chunks map at 64K granularity at best, and a size too big to add the alignment to would wrap the
    // checks below — one such call returned a pointer and left the cursor so the next request re-handed
    // the same range (daemosofchaos's report on #2490).
    if ((alignment & (alignment - 1)) != 0 || alignment > (64 << 10) ||
        size > ~static_cast<std::size_t>(0) - alignment) {
        std::fprintf(stderr, "[rt] d3d12_xs: AllocateUploadMemory(%llu, %llu) refused\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
        return 0;
    }
    auto* arena = static_cast<UploadArena*>(device->upload_state.load(std::memory_order_acquire));
    if (!arena) {
        auto* fresh = new (std::nothrow) UploadArena;
        if (!fresh) return 0;
        void* expected = nullptr;  // a concurrent first call loses its arena but keeps the winner's chunks
        if (device->upload_state.compare_exchange_strong(expected, fresh, std::memory_order_acq_rel))
            arena = fresh;
        else {
            delete fresh;
            arena = static_cast<UploadArena*>(expected);
        }
    }
    std::lock_guard<std::mutex> lock(arena->lock);
    const std::size_t need = static_cast<std::size_t>(size);
    for (UploadChunk* chunk = arena->chunks; chunk; chunk = chunk->next) {
        const std::size_t aligned = (chunk->used + (alignment - 1)) & ~(static_cast<std::size_t>(alignment) - 1);
        if (aligned >= chunk->used && aligned <= chunk->size && need <= chunk->size - aligned) {
            chunk->used = aligned + need;
            const std::uint64_t result = reinterpret_cast<std::uint64_t>(chunk->cpu_base + aligned);
            log_upload_result(chunk, result, size, alignment);
            return result;
        }
    }
    const std::size_t chunk_size = need + alignment > kUploadChunkMin ? need + alignment : kUploadChunkMin;
    UploadChunk* chunk = upload_chunk_new(device, chunk_size);
    if (!chunk) {
        std::fprintf(stderr, "[rt] d3d12_xs: AllocateUploadMemory(%llu, %llu) failed\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment));
        return 0;
    }
    chunk->next = arena->chunks;
    arena->chunks = chunk;
    const std::size_t aligned = (alignment - 1) & ~(static_cast<std::size_t>(alignment) - 1);  // 0-based
    chunk->used = aligned + need;
    if (upload_log())
        std::fprintf(stderr, "[rt] upload-log: new chunk at 0x%llx, device address 0x%llx, %llu bytes (%s)\n",
                     static_cast<unsigned long long>(reinterpret_cast<std::uint64_t>(chunk->cpu_base)),
                     static_cast<unsigned long long>(chunk->gpu_base), static_cast<unsigned long long>(chunk->size),
                     chunk->resource ? "mapped upload buffer" : "VirtualAlloc fallback");
    const std::uint64_t result = reinterpret_cast<std::uint64_t>(chunk->cpu_base + aligned);
    log_upload_result(chunk, result, size, alignment);
    return result;
}

// An address inside an upload chunk becomes the chunk's device address plus the offset; anything else — a
// real resource VA the title already holds — passes through.
std::uint64_t upload_translate(DeviceProxy* device, std::uint64_t va) {
    auto* arena = static_cast<UploadArena*>(
        device ? device->upload_state.load(std::memory_order_acquire) : nullptr);
    if (!arena) return va;
    std::lock_guard<std::mutex> lock(arena->lock);
    for (UploadChunk* chunk = arena->chunks; chunk; chunk = chunk->next) {
        const std::uint64_t base = reinterpret_cast<std::uint64_t>(chunk->cpu_base);
        if (va >= base && va < base + chunk->size) return chunk->gpu_base + (va - base);
    }
    return va;
}

void destroy_upload_state(void* upload_state) {
    auto* arena = static_cast<UploadArena*>(upload_state);
    if (!arena) return;
    for (UploadChunk* chunk = arena->chunks; chunk;) {
        UploadChunk* next = chunk->next;
        if (chunk->resource) {
            const char* alias = std::getenv("LEONIDA_UPLOAD_ALIAS");
            if (alias && *alias == '1')
                leonida::title_va_aliases().remove(reinterpret_cast<std::uint64_t>(chunk->cpu_base), chunk->gpu_base);
            chunk->resource->Unmap(0, nullptr);
            chunk->resource->Release();
        } else {
            VirtualFree(chunk->cpu_base, 0, MEM_RELEASE);
        }
        delete chunk;
        chunk = next;
    }
    delete arena;
}

constexpr unsigned kConsoleSetDescriptorHeaps = 32;
constexpr unsigned kConsoleExecuteBundle = 31;
constexpr unsigned kConsoleOMSetRenderTargets = 50;
constexpr unsigned kConsoleClearDepthStencilView = 51;
constexpr unsigned kConsoleClearRenderTargetView = 52;
constexpr unsigned kConsoleClearUavUint = 53;
constexpr unsigned kConsoleClearUavFloat = 54;
constexpr unsigned kConsoleReset = 139;
constexpr unsigned kConsoleBarrier = 146;
constexpr unsigned kConsoleAllocateUpload = 148;

// console_to_pc_list_slot and kListNotForwarded are in d3d12_xs_device.h (the mock tests check the table).
static_assert(console_to_pc_list_slot(kConsoleReset) == 10 && console_to_pc_list_slot(kConsoleBarrier) == 80,
              "the console list table names Reset and Barrier");

struct ListProxy {
    void** vtable;                      // +0x00
    ID3D12GraphicsCommandList* real;    // +0x08
    DeviceProxy* device;                // +0x10: keeps the Xbox-ABI device alive; GetDevice returns it
    // +0x18..: the console object body a title's inline writers read. +0x18 holds the write cursor, +0x28 and
    // +0x30 the record limits; the rest is room for the measured flags, header table and pipeline record.
    alignas(8) std::uint8_t console_fields[0x600];
    std::atomic<unsigned long> refs;    // past the console body
    unsigned pc_slot_count;             // slots the real list implements; mapped slots past it are refused
    std::vector<D3D12_VERTEX_BUFFER_VIEW> vb_scratch;  // slot-48 views with their VAs translated
    std::vector<D3D12_STREAM_OUTPUT_BUFFER_VIEW> so_scratch;  // slot-49 views, same treatment

    // Memory writes and waits (d3d12_xs_list_values.h, "list cut" below). A wait recorded after work cuts the
    // recording: the real list so far is closed and kept in `parts`, and the title's calls go on into a spare real
    // list, which `real` then names, with the state the title had set replayed on it. The queue proxy submits the
    // parts in order with the waits between them. `primary` is the real list the proxy was made with; each Reset
    // makes it `real` again and puts the other lists back with the spares. Every list here holds one reference.
    struct Part {
        ID3D12GraphicsCommandList* list;
        std::vector<list_values::Write> pre_writes;  // recorded before the gate: land before the queue stands at it
        std::vector<list_values::Wait> gate;         // the queue stands at these before the part runs
        std::vector<list_values::Write> writes;      // land once the part has run
    };
    ID3D12GraphicsCommandList* primary = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;   // real allocator of the current recording (no reference)
    std::vector<Part> parts;                       // closed parts before `real`
    std::vector<list_values::Write> pre_writes;    // `real`'s part
    std::vector<list_values::Wait> gate;
    std::vector<list_values::Write> writes;
    std::vector<list_values::Wait> deferred;       // waits recorded while a query was open: cut at its end
    std::vector<ID3D12GraphicsCommandList*> spares;  // closed real lists for later cuts
    list_values::ListState state;
    unsigned open_queries = 0;                     // BeginQuery without its EndQuery yet: no cut may split them
    bool part_has_work = false;                    // `real` has recorded something a wait must follow
    bool real_closed = false;                      // `real` was closed (Close succeeded) since its last Reset
    // Set at creation. LEONIDA_LIST_VALUES=0 leaves both off and the list as it was before list values. A bundle
    // keeps its state (what it sets stays set in the list executing it) but no writes or waits: ExecuteBundle names
    // one real list, so a bundle is never cut.
    bool track_state = false;
    bool track_values = false;
    bool bundle = false;
    bool has_values() const { return !parts.empty() || !gate.empty() || !writes.empty() || !pre_writes.empty(); }

    void pin_cursor_at_limit() {
        const std::uint64_t at_limit = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(this) + 0x18 + 0x500);
        for (const size_t offset : {0x18, 0x28, 0x30})
            std::memcpy(reinterpret_cast<std::uint8_t*>(this) + offset, &at_limit, sizeof(at_limit));
    }
};
static_assert(offsetof(ListProxy, console_fields) == 0x18, "the console fields start at +0x18");

struct ListStats {
    static inline std::atomic<std::uint64_t> hits[kListSlots];
    static inline std::atomic<bool> first[kListSlots];
};

// The list tail (D3D-445). From CopyTextureRegionX on, the newer console layout sits two slots later than the older
// one (docs/d3d12_xs_console_abi.md 5.3). The table is keyed by the older-layout slot. Only CopyTextureRegionX has a
// measured name; the other measured pairs are named by their older slot, so that both layouts print the same text
// for the same method.
struct ListTailName {
    unsigned older_slot;
    const char* name;
};
constexpr ListTailName kListTailNames[] = {
    {83, "CopyTextureRegionX"},         {84, "tail method, older slot 84"}, {85, "tail method, older slot 85"},
    {88, "tail method, older slot 88"}, {89, "tail method, older slot 89"}, {90, "tail method, older slot 90"},
    {93, "tail method, older slot 93"}, {94, "tail method, older slot 94"},
};
constexpr unsigned kListTailShift = 2;
static_assert(kConsoleLayoutOlder == ChildProxy::kQueueLayoutOlder &&
                  kConsoleLayoutNewer == ChildProxy::kQueueLayoutNewer,
              "the list tail names use the queue proxy's layout values");

// Null while the layout is unknown, and for a slot that is not a measured tail slot of that layout: a slot number
// never gets the other layout's name.
const char* list_tail_name(unsigned slot, unsigned char console_layout) {
    if (console_layout != kConsoleLayoutOlder && console_layout != kConsoleLayoutNewer) return nullptr;
    const unsigned shift = console_layout == kConsoleLayoutNewer ? kListTailShift : 0;
    if (slot < shift) return nullptr;
    for (const ListTailName& entry : kListTailNames)
        if (entry.older_slot == slot - shift) return entry.name;
    return nullptr;
}

void note_list_slot(unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4,
                    unsigned char console_layout = 0) {
    if (slot >= kListSlots) return;
    ListStats::hits[slot].fetch_add(1, std::memory_order_relaxed);
    // A plain load first: a compare-exchange on a flag already set still takes its cache line exclusively, at every
    // list call from all of the title's recording threads.
    bool expected = false;
    if (!ListStats::first[slot].load(std::memory_order_relaxed) &&
        ListStats::first[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        const char* name = list_tail_name(slot, console_layout);
        std::fprintf(stderr, "[rt] d3d12_xs: list slot %u%s%s%s first call (args 0x%llx 0x%llx 0x%llx 0x%llx)\n", slot,
                     name ? " (" : "", name ? name : "", name ? ")" : "", static_cast<unsigned long long>(a1),
                     static_cast<unsigned long long>(a2), static_cast<unsigned long long>(a3),
                     static_cast<unsigned long long>(a4));
    }
}

// Measured Xbox ID3D12GraphicsCommandListX IID passed to device slot 73 (CreateCommandListX).
constexpr IID kXboxGraphicsCommandListXIid = {
    0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};

// The interfaces the console list answers: the base chain, ID3D12CommandList and every graphics-command-list
// interface the real list implements — on the console the list is a single object, so any of them returns the
// proxy.
bool is_list_iid(const IID& iid) {
    return IsEqualIID(iid, __uuidof(IUnknown)) || IsEqualIID(iid, __uuidof(ID3D12Object)) ||
           IsEqualIID(iid, __uuidof(ID3D12DeviceChild)) || IsEqualIID(iid, __uuidof(ID3D12CommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList1)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList2)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList3)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList4)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList5)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList6)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList7)) ||
           IsEqualIID(iid, __uuidof(ID3D12GraphicsCommandList8)) ||
           IsEqualIID(iid, kXboxGraphicsCommandListXIid);
}

unsigned long list_add_ref(ListProxy* self) { return self->refs.fetch_add(1, std::memory_order_relaxed) + 1; }

void destroy_list(ListProxy* self) {
    // No reference to the proxy is left: its real lists go first (the primary, the one recording if a cut made
    // another, the earlier parts and the spares), then the proxy and its device reference.
    for (const ListProxy::Part& part : self->parts)
        if (part.list != self->primary) part.list->Release();
    for (ID3D12GraphicsCommandList* spare : self->spares) spare->Release();
    if (self->real && self->real != self->primary) self->real->Release();
    if (self->primary) self->primary->Release();
    DeviceProxy* device = self->device;
    delete self;
    if (device) proxy_release(device);
}

unsigned long list_release(ListProxy* self) {
    const unsigned long refs = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (refs == 0) destroy_list(self);
    return refs;
}

HRESULT list_query_interface(ListProxy* self, const IID* riid, void** out) {
    note_list_slot(0, reinterpret_cast<std::uint64_t>(riid), reinterpret_cast<std::uint64_t>(out), 0, 0);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    if (!is_list_iid(*riid)) {
        if (supports_iid(self->real, *riid)) log_refused_iid("list", *riid);
        return E_NOINTERFACE;
    }
    // A console title asks for the console command-list IID (device slot 73 forwards it): the real list answers it
    // as ID3D12GraphicsCommandList, and the caller gets this console-shaped object back.
    const IID real_iid = IsEqualIID(*riid, kXboxGraphicsCommandListXIid)
                             ? __uuidof(ID3D12GraphicsCommandList)
                             : *riid;
    if (!supports_iid(self->real, real_iid)) return E_NOINTERFACE;
    *out = self;
    list_add_ref(self);
    return S_OK;
}

// Highest list interface the real object implements; its cumulative slot count bounds mapped forwards
// (ID3D12GraphicsCommandList8 ends at slot 81, OMSetFrontAndBackStencilRef).
unsigned detect_list_slot_count(IUnknown* real) {
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList8))) return 82;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList7))) return 81;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList6))) return 80;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList5))) return 79;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList4))) return 77;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList3))) return 68;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList2))) return 67;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList1))) return 66;
    if (supports_iid(real, __uuidof(ID3D12GraphicsCommandList))) return 60;
    return 9;
}

// ---- List memory writes and waits: recording and the list cut (d3d12_xs_list_values.h) ----------------------
//
// A write joins the part being recorded and lands once that part has run. A wait recorded before any work of the part
// (the retail title's usual place: the first calls after Reset) joins the part's gate: the queue stands at it before
// the part. Writes recorded before the part's first wait become its pre-writes, which land before the queue stands at
// the gate (behind the work submitted before the part): the title's frame lists write a counter and a timestamp and
// then wait, and its direct lists write job tags as their first calls and then wait for compute results whose lists
// wait for those tags, so a gate ahead of those writes would hold both queues until the timeout. A wait after work,
// or after a write that follows a wait, cuts the list (cut_list): the real list so far becomes a closed part, and the
// title's calls go on into a spare real list with the state replayed, so what was recorded before the wait is done
// before the queue stands at it and the work after it waits, as on the console. A query open across the wait would
// be split by the cut (D3D12 lists must end the queries they begin; the layer refuses such a list): the cut then
// waits for the query's EndQuery. Bundles record neither (note_bundle_value_op).

bool is_list_proxy_impl(const void* list);

// Console list slots whose PC call only sets state (replayed on the next part after a cut). Every other forwarded
// call but Close, Reset and the event marker is work a later wait must follow.
constexpr bool is_list_state_slot(unsigned slot) {
    return (slot >= 20 && slot <= 25) || (slot >= 32 && slot <= 50) || slot == 59;
}
constexpr unsigned kConsoleBeginQuery = 56, kConsoleEndQuery = 57, kConsoleEndEvent = 60;

// The state a forwarded call set, as the real list received it (the slots with their own translation note theirs
// in place: 32, 47-50).
template <unsigned Slot>
void note_list_state(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3, std::uint64_t a4) {
    using State = list_values::ListState;
    State& state = self->state;
    if constexpr (Slot == 20) {
        state.topology(a1);
    } else if constexpr (Slot == 21) {
        state.viewports(static_cast<UINT>(a1), reinterpret_cast<const D3D12_VIEWPORT*>(a2));
    } else if constexpr (Slot == 22) {
        state.scissors(static_cast<UINT>(a1), reinterpret_cast<const D3D12_RECT*>(a2));
    } else if constexpr (Slot == 23) {
        state.blend_factor(reinterpret_cast<const FLOAT*>(a1));
    } else if constexpr (Slot == 24) {
        state.stencil_ref(a1);
    } else if constexpr (Slot == 25) {
        state.pipeline(a1);
    } else if constexpr (Slot == 33 || Slot == 34) {
        state.root_signature(Slot == 34, a1);
    } else if constexpr (Slot == 35 || Slot == 36) {
        state.root_arg(Slot == 36, static_cast<UINT>(a1), State::kTable, a2);
    } else if constexpr (Slot == 37 || Slot == 38) {
        const auto value = static_cast<std::uint32_t>(a2);
        state.root_constants(Slot == 38, static_cast<UINT>(a1), 1, &value, static_cast<UINT>(a3));
    } else if constexpr (Slot == 39 || Slot == 40) {
        state.root_constants(Slot == 40, static_cast<UINT>(a1), static_cast<UINT>(a2),
                             reinterpret_cast<const std::uint32_t*>(a3), static_cast<UINT>(a4));
    } else if constexpr (Slot >= 41 && Slot <= 46) {
        // 41/42 CBV, 43/44 SRV, 45/46 UAV; the even slot is the graphics one.
        state.root_arg(Slot % 2 == 0, static_cast<UINT>(a1),
                       static_cast<State::RootKind>(State::kCbv + (Slot - 41) / 2), a2);
    } else if constexpr (Slot == 47) {
        state.index_buffer(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1));
    } else if constexpr (Slot == 48) {
        state.vertex_buffers(static_cast<UINT>(a1), static_cast<UINT>(a2),
                             reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(a3));
    } else if constexpr (Slot == 49) {
        state.so_targets(static_cast<UINT>(a1), static_cast<UINT>(a2),
                         reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(a3));
    } else if constexpr (Slot == 59) {
        state.predication(a1, a2, a3);
    }
    (void)self, (void)a1, (void)a2, (void)a3, (void)a4;
}

// A new recording (Reset): the primary list records again; the other real lists of the last recording are closed and
// wait with the spares for later cuts.
void restart_recording(ListProxy* self) {
    for (const ListProxy::Part& part : self->parts)
        if (part.list != self->primary) self->spares.push_back(part.list);
    self->parts.clear();
    if (self->real != self->primary) {
        // A title resets a list it closed; one still open (a D3D12 error, which the real Reset reports) is closed
        // here so that a later cut can Reset it.
        if (!self->real_closed) self->real->Close();
        self->spares.push_back(self->real);
        self->real = self->primary;
    }
    self->pre_writes.clear();
    self->gate.clear();
    self->writes.clear();
    self->deferred.clear();
    self->open_queries = 0;
    self->part_has_work = false;
}

// The list cut (the comment above): false when no spare real list could be had, and the caller keeps the wait in the
// current part's gate, ahead of its work and writes (stricter than the console; a deadlock on the part's own write is
// bounded by the wait's timeout).
bool cut_list(ListProxy* self, const list_values::Wait& wait) {
    auto log_failure = [](const char* what, HRESULT hr) {
        list_values::bump(list_values::counters().cut_failures);
        static std::atomic<unsigned> logged{0};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
            std::fprintf(stderr, "[rt] d3d12_xs: a list waits for a memory value after work, but %s (hr=0x%08x)\n",
                         what, static_cast<unsigned>(hr));
    };
    if (!self->allocator) {
        log_failure("its allocator is not known: the wait stands before the part", E_FAIL);
        return false;
    }
    // One list records into an allocator at a time: the part closes before the next list opens on it.
    const HRESULT closed = self->real->Close();
    if (FAILED(closed)) log_failure("the part before it does not close (it stays refused, as it was)", closed);
    ID3D12GraphicsCommandList* next = nullptr;
    HRESULT hr = E_FAIL;
    if (!self->spares.empty()) {
        next = self->spares.back();
        hr = next->Reset(self->allocator, nullptr);
        if (SUCCEEDED(hr)) {
            self->spares.pop_back();
        } else {
            next = nullptr;
        }
    }
    if (!next) {
        void* created = nullptr;
        hr = self->device->real->CreateCommandList(0, self->real->GetType(), self->allocator, nullptr,
                                                   __uuidof(ID3D12GraphicsCommandList), &created);
        next = SUCCEEDED(hr) ? static_cast<ID3D12GraphicsCommandList*>(created) : nullptr;
    }
    if (!next) {
        // The part is closed and nothing records: the title's next calls fail as on a closed list, and its Close
        // reports it. Not expected short of running out of memory.
        log_failure("no list can record the part after it", hr);
        self->real_closed = SUCCEEDED(closed);
        return false;
    }
    self->parts.push_back({self->real, std::move(self->pre_writes), std::move(self->gate), std::move(self->writes)});
    self->pre_writes.clear();
    self->gate.clear();
    self->writes.clear();
    self->gate.push_back(wait);
    self->real = next;
    self->real_closed = false;
    self->part_has_work = false;
    self->state.replay(next);
    const std::uint64_t cuts = list_values::counters().cuts.fetch_add(1, std::memory_order_relaxed) + 1;
    if (cuts == 1 || (cuts & (cuts - 1)) == 0)  // the first cut, then at powers of two
        std::fprintf(stderr, "[rt] d3d12_xs: a list is cut at a wait for a memory value (%llu cuts so far)\n",
                     static_cast<unsigned long long>(cuts));
    return true;
}

void record_wait(ListProxy* self, const list_values::Wait& wait) {
    list_values::bump(list_values::counters().waits_recorded);
    if (!self->part_has_work && (self->writes.empty() || self->gate.empty())) {
        // Nothing recorded to stand behind but writes before the part's first wait: the wait gates the part, and the
        // writes land before it.
        self->pre_writes.insert(self->pre_writes.end(), self->writes.begin(), self->writes.end());
        self->writes.clear();
        self->gate.push_back(wait);
    } else if (self->open_queries) {
        self->deferred.push_back(wait);
        list_values::bump(list_values::counters().cuts_deferred);
    } else if (!cut_list(self, wait)) {
        self->gate.push_back(wait);
    }
}

// EndQuery closed the last open query: the waits recorded meanwhile cut the list here.
void apply_deferred_waits(ListProxy* self) {
    if (self->deferred.empty() || self->open_queries) return;
    std::vector<list_values::Wait> waits = std::move(self->deferred);
    self->deferred.clear();
    for (const list_values::Wait& wait : waits) {
        if ((!self->part_has_work && self->writes.empty()) || !cut_list(self, wait)) self->gate.push_back(wait);
    }
}

// write64 with flag 1 (the GPU's timestamp) or 2 (the system's): the GPU takes its timestamp here, at the write's
// place in the list (the title's GPU timing reads these back: a begin/end pair must be its work's, not one landing
// time). Without the device's timestamp heap the write lands the CPU's clock, as before.
void take_timestamp(ListProxy* self, list_values::Write& write) {
    DeviceProxy* device = self->device;
    std::shared_ptr<list_values::Timestamps> stamps;
    {
        std::lock_guard lock(device->timestamps_mutex);
        if (!device->timestamps_tried) {
            device->timestamps_tried = true;
            device->timestamps = list_values::Timestamps::create(device->real);
            std::fprintf(stderr, device->timestamps
                                     ? "[rt] d3d12_xs: list timestamp writes take the GPU's timestamp at their place\n"
                                     : "[rt] d3d12_xs: no timestamp query heap: list timestamp writes take the CPU's "
                                       "clock when they land\n");
        }
        stamps = device->timestamps;
    }
    if (!stamps) return;
    const std::uint32_t slot = stamps->take();
    self->real->EndQuery(stamps->heap(), D3D12_QUERY_TYPE_TIMESTAMP, slot);
    self->real->ResolveQueryData(stamps->heap(), D3D12_QUERY_TYPE_TIMESTAMP, slot, 1, stamps->readback(),
                                 std::uint64_t{slot} * sizeof(std::uint64_t));
    write.stamps = std::move(stamps);
    write.slot = slot;
}

// Console list slots 71-74 (69-72 on the older layout): write32/write64(address, value, flush, flags) and
// wait32/wait64(address, comparison, reference, ?). The flush word and the wait's fourth word are not used.
void record_value_op(ListProxy* self, ConsoleValueOp op, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                     std::uint64_t a4) {
    if (!a1) return;  // nothing to write or read
    if (op == ConsoleValueOp::write32 || op == ConsoleValueOp::write64) {
        list_values::Write write{a1, a2, 8, list_values::WriteKind::value};
        if (op == ConsoleValueOp::write32) {
            write.value = a2 & 0xffffffffu;
            write.bytes = 4;
        } else if (static_cast<std::uint32_t>(a4) == 1) {
            write.kind = list_values::WriteKind::gpu_timestamp;
            take_timestamp(self, write);
        } else if (static_cast<std::uint32_t>(a4) == 2) {
            write.kind = list_values::WriteKind::system_timestamp;
            take_timestamp(self, write);
        }
        self->writes.push_back(write);
        list_values::bump(list_values::counters().writes_recorded);
        return;
    }
    const auto comparison = static_cast<std::uint32_t>(a2);
    if (comparison < 2 || comparison > 7) {  // always, never or unknown: met (console_value_met), never held
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: a list waits for a memory value with comparison %u, taken as met\n",
                         comparison);
        return;
    }
    record_wait(self, {a1, a3, comparison, op == ConsoleValueOp::wait64});
}

// A write or wait recorded on a bundle: a no-op, as before list values. A wait after work would cut the bundle, and
// the list executing it would run only the last part (ExecuteBundle names one real list); the title has not been
// seen doing this.
void note_bundle_value_op(ConsoleValueOp op) {
    list_values::bump(list_values::counters().bundle_ops);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: a bundle records a memory %s; bundles do not model them (no-op)\n",
                     op == ConsoleValueOp::write32 || op == ConsoleValueOp::write64 ? "write" : "wait");
}

// ---- Console list slot 146: Barrier(NumBarrierGroups, pBarrierGroups) -> GCL7 Barrier (PC 80) ----
//
// The title records texture barriers with no resource, which the console takes as a global memory and execution
// barrier. A PC texture barrier needs a resource (the Vulkan layer's list goes into its error state, so Close fails
// and the submission is dropped), so each such barrier becomes a D3D12_GLOBAL_BARRIER with the same sync and access;
// its layouts, subresource range and flags have nothing to apply to. A texture group with both kinds is split into
// runs in their order: texture runs point into the title's array, global runs into the call's own storage. A call
// with nothing to convert is forwarded with the title's own pointer.
constexpr UINT32 kMaxConsoleBarrierGroups = 1024;     // the measured calls pass one group
constexpr UINT32 kMaxConsoleBarriersPerGroup = 65536;

struct ConsoleBarrierStorage {  // lives for one call, on the recording thread's stack
    std::vector<D3D12_BARRIER_GROUP> groups;
    std::vector<D3D12_GLOBAL_BARRIER> globals;
};

// The groups to forward: `in` when nothing needs converting, `storage.groups` when something does, null with
// `reason` set when the array cannot be read safely and the call is refused. `out_count` is the group count to pass.
const D3D12_BARRIER_GROUP* console_barrier_groups(UINT32 count, const D3D12_BARRIER_GROUP* in,
                                                  ConsoleBarrierStorage& storage, UINT32& out_count,
                                                  const char*& reason) {
    const auto refuse = [&reason](const char* why) -> const D3D12_BARRIER_GROUP* {
        reason = why;
        return nullptr;
    };
    out_count = count;
    if (count == 0) return in;  // nothing to read: the PC call is a no-op either way
    if (!in) return refuse("null group array with a nonzero count");
    if (count > kMaxConsoleBarrierGroups) return refuse("group count over the limit");
    std::size_t null_textures = 0, texture_barriers = 0;
    for (UINT32 g = 0; g < count; ++g) {
        const D3D12_BARRIER_GROUP& group = in[g];
        if (group.Type != D3D12_BARRIER_TYPE_GLOBAL && group.Type != D3D12_BARRIER_TYPE_TEXTURE &&
            group.Type != D3D12_BARRIER_TYPE_BUFFER)
            return refuse("unknown barrier group type");
        if (group.NumBarriers > kMaxConsoleBarriersPerGroup) return refuse("barrier count over the limit");
        if (group.NumBarriers && !group.pGlobalBarriers)  // the union's pointer, whichever member the type names
            return refuse("null barrier array with a nonzero count");
        if (group.Type != D3D12_BARRIER_TYPE_TEXTURE) continue;
        texture_barriers += group.NumBarriers;
        for (UINT32 i = 0; i < group.NumBarriers; ++i)
            if (!group.pTextureBarriers[i].pResource) ++null_textures;
    }
    if (null_textures == 0) return in;
    try {
        storage.globals.reserve(null_textures);  // exact: the groups point into it, so it never reallocates
        storage.groups.reserve(count + texture_barriers);
        for (UINT32 g = 0; g < count; ++g) {
            const D3D12_BARRIER_GROUP& group = in[g];
            if (group.Type != D3D12_BARRIER_TYPE_TEXTURE) {
                storage.groups.push_back(group);
                continue;
            }
            const D3D12_TEXTURE_BARRIER* barriers = group.pTextureBarriers;
            for (UINT32 i = 0; i < group.NumBarriers;) {
                const UINT32 start = i;
                const bool no_resource = !barriers[i].pResource;
                while (i < group.NumBarriers && !barriers[i].pResource == no_resource) ++i;
                D3D12_BARRIER_GROUP run{};
                run.NumBarriers = i - start;
                if (no_resource) {
                    run.Type = D3D12_BARRIER_TYPE_GLOBAL;
                    run.pGlobalBarriers = storage.globals.data() + storage.globals.size();
                    for (UINT32 k = start; k < i; ++k)
                        storage.globals.push_back(D3D12_GLOBAL_BARRIER{barriers[k].SyncBefore, barriers[k].SyncAfter,
                                                                       barriers[k].AccessBefore,
                                                                       barriers[k].AccessAfter});
                } else {
                    run.Type = D3D12_BARRIER_TYPE_TEXTURE;
                    run.pTextureBarriers = barriers + start;
                }
                storage.groups.push_back(run);
            }
        }
    } catch (const std::bad_alloc&) {
        return refuse("out of memory converting the groups");
    }
    out_count = static_cast<UINT32>(storage.groups.size());
    return storage.groups.data();
}

// Logged once per reason. The call is dropped whole: nothing of it reaches the PC list.
void log_refused_barrier(const char* reason, UINT32 count, const void* groups) {
    static std::mutex lock;
    static std::set<const char*> seen;  // the reasons are string literals
    std::lock_guard guard(lock);
    try {
        if (!seen.insert(reason).second) return;
    } catch (const std::bad_alloc&) {
        return;
    }
    std::fprintf(stderr, "[rt] d3d12_xs: list slot 146 (Barrier) refused, nothing recorded: %s (%u groups at %p)\n",
                 reason, count, groups);
}

template <unsigned Slot>
std::uint64_t WINAPI list_slot(ListProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                               std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                               std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                               std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(list_query_interface(self, reinterpret_cast<const IID*>(a1),
                                              reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return list_add_ref(self);
    } else if constexpr (Slot == 2) {
        return list_release(self);
    } else if constexpr (Slot == 7) {  // GetDevice: the Xbox-ABI device, as on the other proxies
        note_list_slot(7, a1, a2, 0, 0);
        if (a2) *reinterpret_cast<void**>(a2) = nullptr;
        if (!a1) return hr_to_u64(E_POINTER);
        if (!a2) {  // no output: the real object's own answer
            using Fn = HRESULT(WINAPI*)(IUnknown*, const IID*, void**);
            return hr_to_u64(reinterpret_cast<Fn>((*reinterpret_cast<void***>(self->real))[7])(
                self->real, reinterpret_cast<const IID*>(a1), nullptr));
        }
        return hr_to_u64(proxy_query_interface(self->device, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else {
        const unsigned char layout = self->device->console_layout.load(std::memory_order_relaxed);
        note_list_slot(Slot, a1, a2, a3, a4, layout);
        constexpr unsigned pc = console_to_pc_list_slot(Slot);
        if constexpr (pc == kListNotForwarded) {
#ifdef LEONIDA_VULKAN_DEVICE
            if constexpr (Slot == 71 || Slot == 72) {
                // One owner per write: a list the list values track lands its slot 71-74 writes itself, at the part's
                // fence and with a D3D12 timestamp query for flags 1 and 2. The label path would store to the same
                // address again from its own command-buffer completion and a Vulkan query, so a late label could
                // rewind a rising counter or split a begin/end pair. It answers only for lists not tracked here
                // (LEONIDA_LIST_VALUES=0, bundles).
                if (leonida::label::enabled() && !self->track_values) {
                    list_values::bump(list_values::counters().labels_handed);
                    const unsigned width = Slot == 71 ? 4u : 8u;
                    const unsigned clock = Slot == 72 ? ((a4 & 1) ? 1u : ((a4 & 2) ? 2u : 0u)) : 0u;
                    auto target = leonida::label::memory().capture(a1, width);
                    if (!target.owner.lock() && rt::xmem::label_target_fn) {
                        // The pages the title maps are the title's memory too, and the xmem shim owns their page
                        // tables (runtime/shims/xmem.h).
                        leonida::label::Target mapped;
                        if (rt::xmem::label_target_fn(a1, width, mapped)) target = std::move(mapped);
                    }
                    if (target.owner.lock())
                        rt::d3d12::record_gpu_label(reinterpret_cast<ID3D12GraphicsCommandList*>(self->real),
                                                    {std::move(target), a2, clock});
                }
            }
#endif
            if constexpr (Slot == kConsoleAllocateUpload) {
                return upload_alloc(self->device, a1, a2);  // (size, alignment); the comment at UploadChunk
            }
            if constexpr (Slot >= 69 && Slot <= 74) {  // memory writes and waits, by the layout (console_value_op)
                const ConsoleValueOp op = console_value_op(Slot, layout);
                if (op != ConsoleValueOp::none) {
                    if (self->track_values)
                        record_value_op(self, op, a1, a2, a3, a4);
                    else if (self->bundle && self->track_state)
                        note_bundle_value_op(op);
                }
            }
            return 0;  // unmodelled console slot: recorded above, never a PC call
        } else {
            if (pc >= self->pc_slot_count) {
                static std::atomic<bool> beyond_warned[kListSlots];
                bool expected = false;
                if (beyond_warned[Slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
                    std::fprintf(stderr,
                                 "[rt] d3d12_xs: list slot %u maps to PC slot %u, beyond the real list's %u "
                                 "slots; returning E_NOTIMPL\n",
                                 Slot, pc, self->pc_slot_count);
                return hr_to_u64(E_NOTIMPL);
            }
            using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t, std::uint64_t,
                                                     std::uint64_t, std::uint64_t);
            auto** const real_vtable = *reinterpret_cast<void***>(self->real);
            // LOCAL: console SetComputeRootDescriptorTable (35) and SetGraphicsRootDescriptorTable (36)
            if constexpr (Slot == 35 || Slot == 36) local_mirror_table(self->device, a2);
            if constexpr (pc == 10) {
                // Reset(allocator, pso): console slot 10, and slot 139 as the retail title calls it. The title hands
                // its allocator proxy; the real list takes the real one (and a wrapped pipeline state the same way).
                a1 = unwrap_object(a1);
                a2 = unwrap_object(a2);
                restart_recording(self);
                self->pin_cursor_at_limit();  // a reset list starts its records again: keep the title on the API path
                const std::uint64_t result = reinterpret_cast<ForwardFn>((*reinterpret_cast<void***>(self->real))[pc])(
                    self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                if (SUCCEEDED(static_cast<HRESULT>(result))) {
                    self->allocator = reinterpret_cast<ID3D12CommandAllocator*>(a1);
                    self->real_closed = false;
                    if (self->track_state) {
                        self->state.clear();
                        if (a2) self->state.pipeline(a2);  // the initial pipeline state
                    }
                }
                return result;
            }
            if constexpr (Slot == 9) {  // Close
                self->deferred.clear();  // a query still open: the real list refuses itself anyway
                const std::uint64_t result = reinterpret_cast<ForwardFn>(real_vtable[pc])(
                    self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                if (SUCCEEDED(static_cast<HRESULT>(result))) self->real_closed = true;
                return result;
            }
            if constexpr (!is_list_state_slot(Slot) && Slot != kConsoleEndEvent) self->part_has_work = true;
            if constexpr (Slot == kConsoleBeginQuery) {
                if (static_cast<UINT>(a2) != D3D12_QUERY_TYPE_TIMESTAMP) ++self->open_queries;
            } else if constexpr (Slot == kConsoleEndQuery) {
                const std::uint64_t result = reinterpret_cast<ForwardFn>(real_vtable[pc])(
                    self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
                if (static_cast<UINT>(a2) != D3D12_QUERY_TYPE_TIMESTAMP && self->open_queries &&
                    --self->open_queries == 0)
                    apply_deferred_waits(self);
                return result;
            }
            if constexpr (Slot == 11) {
                // ClearState(pipeline): the list is back to its state at creation, with this pipeline state.
                if (self->track_state) {
                    self->state.clear();
                    if (a1) self->state.pipeline(a1);
                }
            } else if constexpr (Slot == kConsoleExecuteBundle) {
                // What the bundle set stays set here: a later cut replays it.
                if (self->track_state && is_list_proxy_impl(reinterpret_cast<const void*>(a1)))
                    self->state.inherit(reinterpret_cast<const ListProxy*>(a1)->state);
                a1 = unwrap_object(a1);  // the bundle is a list proxy too
            } else if constexpr (Slot >= 41 && Slot <= 46) {
                // The six root descriptor setters (index, GPU VA): an upload-arena CPU pointer becomes the
                // chunk's device address (d3d12_xs_console_abi.md §5.3 measures slots 41/43/45 for the compute
                // side; the graphics setters interleave on even slots).
                a2 = upload_translate(self->device, a2);
            } else if constexpr (Slot == 47) {
                // IASetIndexBuffer(pView): the view's BufferLocation is a GPU VA; translate in a copy so the
                // title's struct is not written.
                if (a1) {
                    auto view = *reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a1);
                    view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                    if (self->track_state) self->state.index_buffer(&view);
                    return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                        self->real, reinterpret_cast<std::uint64_t>(&view), a2, a3, a4, a5, a6, a7, a8, a9,
                        a10, a11, a12, a13, a14);
                }
            } else if constexpr (Slot == 48) {
                // IASetVertexBuffers(start, count, views): each view's BufferLocation is a GPU VA. The views are
                // translated in the proxy's scratch array for the same reason as the index view.
                if (a3 && a2) {
                    const auto* in = reinterpret_cast<const D3D12_VERTEX_BUFFER_VIEW*>(a3);
                    self->vb_scratch.assign(in, in + a2);
                    for (auto& view : self->vb_scratch)
                        view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                    if (self->track_state)
                        self->state.vertex_buffers(static_cast<UINT>(a1), static_cast<UINT>(a2),
                                                   self->vb_scratch.data());
                    return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                        self->real, a1, a2, reinterpret_cast<std::uint64_t>(self->vb_scratch.data()), a4, a5,
                        a6, a7, a8, a9, a10, a11, a12, a13, a14);
                }
            } else if constexpr (Slot == 49) {
                // SOSetTargets(start, count, views): BufferLocation and BufferFilledSizeLocation are both VAs.
                if (a3 && a2) {
                    const auto* in = reinterpret_cast<const D3D12_STREAM_OUTPUT_BUFFER_VIEW*>(a3);
                    self->so_scratch.assign(in, in + a2);
                    for (auto& view : self->so_scratch) {
                        view.BufferLocation = upload_translate(self->device, view.BufferLocation);
                        view.BufferFilledSizeLocation =
                            upload_translate(self->device, view.BufferFilledSizeLocation);
                    }
                    if (self->track_state)
                        self->state.so_targets(static_cast<UINT>(a1), static_cast<UINT>(a2), self->so_scratch.data());
                    return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                        self->real, a1, a2, reinterpret_cast<std::uint64_t>(self->so_scratch.data()), a4, a5,
                        a6, a7, a8, a9, a10, a11, a12, a13, a14);
                }
            } else if constexpr (Slot == kConsoleSetDescriptorHeaps) {
                UnwrappedArray heaps;
                if (!heaps.fill(a2, a1)) return 0;  // void on the PC side; the call simply does not happen
                const std::uint64_t real_heaps = heaps.data() ? heaps.data() : a2;
                if (self->track_state)
                    self->state.heaps(static_cast<UINT>(a1), reinterpret_cast<const std::uint64_t*>(real_heaps));
                return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, real_heaps, a3, a4, a5, a6,
                                                                  a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleOMSetRenderTargets) {
                // OMSetRenderTargets(NumRenderTargetDescriptors, pRenderTargetDescriptors,
                //                    RTsSingleHandleToDescriptorRange, pDepthStencilDescriptor):
                // translate title-owned CPU descriptor handles into local copies without mutating caller buffers.
                const UINT count = static_cast<UINT>(a1);
                const auto* in_rtvs = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2);
                const BOOL single_range = static_cast<BOOL>(a3);
                const auto* in_dsv = reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a4);
                D3D12_CPU_DESCRIPTOR_HANDLE rtvs[D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
                const D3D12_CPU_DESCRIPTOR_HANDLE* out_rtvs = in_rtvs;
                if (in_rtvs && count > 0) {
                    if (single_range) {
                        rtvs[0] = translate_cpu_handle(self->device, in_rtvs[0]);
                        out_rtvs = rtvs;
                    } else if (count <= D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) {
                        for (UINT i = 0; i < count; ++i) rtvs[i] = translate_cpu_handle(self->device, in_rtvs[i]);
                        out_rtvs = rtvs;
                    }
                }
                D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
                const D3D12_CPU_DESCRIPTOR_HANDLE* out_dsv = in_dsv;
                if (in_dsv) {
                    dsv = translate_cpu_handle(self->device, *in_dsv);
                    out_dsv = &dsv;
                }
                if (self->track_state) self->state.render_targets(count, out_rtvs, single_range, out_dsv);
                return reinterpret_cast<ForwardFn>(real_vtable[pc])(
                    self->real, a1, reinterpret_cast<std::uint64_t>(out_rtvs), a3,
                    reinterpret_cast<std::uint64_t>(out_dsv), a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleClearRenderTargetView) {
                // ClearRenderTargetView(RenderTargetView, ColorRGBA, NumRects, pRects): translate the RTV handle.
                a1 = static_cast<std::uint64_t>(
                    translate_cpu_handle(self->device, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a1)}).ptr);
                // A view the title made with no resource: the console clear does nothing, and the PC list would
                // take the view as invalid and fail its Close. Any other view goes to the PC list as before.
                if (is_null_rtv(self->device, static_cast<SIZE_T>(a1))) {
                    static std::atomic<bool> logged{false};
                    if (first_call(logged))
                        std::fprintf(stderr, "[rt] d3d12_xs: list slot 52 (ClearRenderTargetView) of a view made with "
                                             "no resource skipped, as on the console (handle 0x%llx)\n",
                                     static_cast<unsigned long long>(a1));
                    return 0;
                }
            } else if constexpr (Slot == kConsoleBarrier) {
                // Barrier(NumBarrierGroups, pBarrierGroups): console_barrier_groups above.
                ConsoleBarrierStorage storage;
                UINT32 count = 0;
                const char* reason = nullptr;
                const D3D12_BARRIER_GROUP* groups = console_barrier_groups(
                    static_cast<UINT32>(a1), reinterpret_cast<const D3D12_BARRIER_GROUP*>(a2), storage, count, reason);
                if (reason) {
                    log_refused_barrier(reason, static_cast<UINT32>(a1), reinterpret_cast<const void*>(a2));
                    return 0;
                }
                if (groups && groups == storage.groups.data()) {
                    static std::atomic<bool> logged{false};
                    if (first_call(logged))
                        std::fprintf(stderr, "[rt] d3d12_xs: list slot 146 (Barrier): texture barriers with no "
                                             "resource recorded as global barriers, as on the console\n");
                }
                return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, count,
                                                                  reinterpret_cast<std::uint64_t>(groups), a3, a4,
                                                                  a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            } else if constexpr (Slot == kConsoleClearUavUint || Slot == kConsoleClearUavFloat) {
                // ClearUnorderedAccessViewUint/Float(ViewGPUHandle, ViewCPUHandle, pResource, Values, NumRects,
                //                                    pRects): keep GPU handle, translate CPU handle.
                a2 = static_cast<std::uint64_t>(
                    translate_cpu_handle(self->device, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)}).ptr);
            }
            if constexpr (is_list_state_slot(Slot)) {
                if (self->track_state) note_list_state<Slot>(self, a1, a2, a3, a4);
            }
            return reinterpret_cast<ForwardFn>(real_vtable[pc])(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9,
                                                              a10, a11, a12, a13, a14);
        }
    }
}

// ClearDepthStencilView (console slot 51 -> PC slot 47) takes FLOAT Depth as its fourth argument (XMM3 on Windows
// x64 ABI). The all-uint64_t list_slot thunk would read R9 instead of XMM3 and clobber Depth, so this slot gets a
// typed thunk that preserves Depth and translates the title-owned CPU descriptor handle.
void WINAPI list_clear_depth_stencil_view(ListProxy* self, D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                          D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT num_rects,
                                          const D3D12_RECT* rects) {
    std::uint32_t depth_bits = 0;
    std::memcpy(&depth_bits, &depth, sizeof(depth_bits));
    note_list_slot(kConsoleClearDepthStencilView, static_cast<std::uint64_t>(dsv.ptr),
                   static_cast<std::uint64_t>(flags), depth_bits, stencil,
                   self->device->console_layout.load(std::memory_order_relaxed));
    constexpr unsigned pc = console_to_pc_list_slot(kConsoleClearDepthStencilView);
    if (pc >= self->pc_slot_count) {
        static std::atomic<bool> beyond_warned{false};
        bool expected = false;
        if (beyond_warned.compare_exchange_strong(expected, true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: list slot %u maps to PC slot %u, beyond the real list's %u slots\n",
                         kConsoleClearDepthStencilView, pc, self->pc_slot_count);
        return;
    }
    self->part_has_work = true;
    const D3D12_CPU_DESCRIPTOR_HANDLE translated = translate_cpu_handle(self->device, dsv);
    using ClearDsvFn = void(WINAPI*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS,
                                     FLOAT, UINT8, UINT, const D3D12_RECT*);
    auto** const real_vtable = *reinterpret_cast<void***>(self->real);
    reinterpret_cast<ClearDsvFn>(real_vtable[pc])(self->real, translated, flags, depth, stencil, num_rects, rects);
}

// OMSetDepthBoundsX (console slot 70 in the newer layout, this package's) -> ID3D12GraphicsCommandList1::
// OMSetDepthBounds (PC slot 62). Its FLOAT arguments arrive in XMM1 and XMM2, which list_slot's integer thunk would
// lose, so it has a typed thunk too. In the older layout slot 70 is a memory write (console_value_op), which this
// thunk cannot see: LEONIDA_DEPTH_BOUNDS_SLOT70=0 keeps the generic thunk for such a title.
constexpr unsigned kConsoleOMSetDepthBounds = 70, kPcOMSetDepthBounds = 62;
void WINAPI list_om_set_depth_bounds(ListProxy* self, FLOAT min, FLOAT max) {
    std::uint32_t lo = 0, hi = 0;
    std::memcpy(&lo, &min, sizeof lo);
    std::memcpy(&hi, &max, sizeof hi);
    const unsigned char layout = self->device->console_layout.load(std::memory_order_relaxed);
    note_list_slot(kConsoleOMSetDepthBounds, lo, hi, 0, 0, layout);
    if (layout == kConsoleLayoutOlder) {
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: list slot 70 in the older layout is a memory write the depth-bounds "
                                 "thunk drops; run with LEONIDA_DEPTH_BOUNDS_SLOT70=0\n");
        return;
    }
    if (kPcOMSetDepthBounds >= self->pc_slot_count) return;
    using Fn = void(WINAPI*)(IUnknown*, FLOAT, FLOAT);
    auto** const real_vtable = *reinterpret_cast<void***>(self->real);
    reinterpret_cast<Fn>(real_vtable[kPcOMSetDepthBounds])(self->real, min, max);
}

template <std::size_t... I>
void fill_list_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&list_slot<static_cast<unsigned>(I)>) ), ...);
    vtable[kConsoleClearDepthStencilView] = reinterpret_cast<void*>(&list_clear_depth_stencil_view);
    wchar_t v[4]{};
    const DWORD n = GetEnvironmentVariableW(L"LEONIDA_DEPTH_BOUNDS_SLOT70", v, 4);
    if (!(n == 1 && v[0] == L'0'))
        vtable[kConsoleOMSetDepthBounds] = reinterpret_cast<void*>(&list_om_set_depth_bounds);
}

void** get_list_vtable() {
    static void** vtable = [] {
        static void* storage[kListSlots];
        fill_list_vtable(storage, std::make_index_sequence<kListSlots>{});
        return storage;
    }();
    return vtable;
}

bool is_list_proxy_impl(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

IUnknown* list_real_or_null(const void* object) {
    return is_list_proxy_impl(object) ? static_cast<IUnknown*>(static_cast<const ListProxy*>(object)->real)
                                    : nullptr;
}

// ---- A submission with memory writes and waits (queue ExecuteCommandLists, d3d12_xs_list_values.h) ----------
//
// The lists' parts run in order. Consecutive parts with no wait between them go to the real queue in one
// ExecuteCommandLists (a group); a group whose parts write is followed by a Signal of the queue's `written` fence,
// whose value the pump lands the writes at. At a part's gate the group so far is submitted first, then each unmet
// wait becomes a queue Wait: on the fence of a write in flight that will meet it (relay), else on the queue's gate
// fence, which the pump signals once memory compares true or the wait timed out. The caller holds the queue's order
// lock, so the fence values rise in submission order.
class ValueSubmission {
public:
    explicit ValueSubmission(ChildProxy* queue) : queue_(queue) {}

    void add(ID3D12CommandList* list) { group_.push_back(list); }

    void part(ID3D12CommandList* list, const std::vector<list_values::Write>& pre_writes,
              const std::vector<list_values::Wait>& gate, const std::vector<list_values::Write>& writes) {
        // Pre-writes land with the group before the gate: behind the work submitted so far, before the queue stands
        // at the gate (stand_at submits the group first when a wait is unmet). A met gate is no gate: they then land
        // behind the part, with its writes. The memory is read once here, for both.
        bool met = true;
        for (const list_values::Wait& w : gate) met = met && list_values::met_now(w);
        for (const list_values::Write& write : pre_writes) {
            writes_.push_back(write);
            // Landing before the part has run, a GPU timestamp of the part's would not be written yet: the CPU's
            // clock then (the work before the part is done by then, as at the write's place).
            if (!met) writes_.back().stamps.reset();
        }
        if (!gate.empty()) stand_at(gate, met);
        group_.push_back(list);
        writes_.insert(writes_.end(), writes.begin(), writes.end());
    }

    void flush() {
        if (!group_.empty()) {
            using ExecuteFn = void(WINAPI*)(void*, std::uint64_t, ID3D12CommandList* const*);
            reinterpret_cast<ExecuteFn>(queue_->real_vtable[kQueueExecuteCommandLists])(
                queue_->real, static_cast<std::uint64_t>(group_.size()), group_.data());
            group_.clear();
        }
        if (writes_.empty()) return;
        list_values::QueueValues* values = queue_->values.get();
        if (values) {
            const std::uint64_t value = ++values->written_value;
            if (SUCCEEDED(signal(values->written, value))) {
                values->written->SetEventOnCompletion(value, list_values::Pump::get().wake());
                list_values::Pump::get().add_writes(queue_->values, value, std::move(writes_));
            } else {
                // Nothing would ever say when they may land: dropped, as without fences.
                list_values::bump(list_values::counters().writes_failed, writes_.size());
            }
        } else {
            list_values::bump(list_values::counters().writes_failed, writes_.size());
        }
        writes_.clear();
    }

private:
    HRESULT signal(ID3D12Fence* fence, std::uint64_t value) {
        using Fn = HRESULT(WINAPI*)(void*, ID3D12Fence*, UINT64);
        return reinterpret_cast<Fn>(queue_->real_vtable[kQueueSignal])(queue_->real, fence, value);
    }
    HRESULT wait(ID3D12Fence* fence, std::uint64_t value) {
        using Fn = HRESULT(WINAPI*)(void*, ID3D12Fence*, UINT64);
        return reinterpret_cast<Fn>(queue_->real_vtable[kQueueWait])(queue_->real, fence, value);
    }

    // `met`: every wait compared true when the part was taken (ValueSubmission::part).
    void stand_at(const std::vector<list_values::Wait>& waits, bool met) {
        auto& counters = list_values::counters();
        if (met) {
            list_values::bump(counters.waits_met_at_submit, waits.size());
            return;
        }
        // The work before the wait goes first; its writes are in flight from here on, so a relay can find them.
        flush();
        list_values::QueueValues* values = queue_->values.get();
        if (!values) return;  // no fences: the wait runs on, as before
        std::vector<list_values::Wait> unmet;
        for (const list_values::Wait& w : waits) {
            if (list_values::met_now(w)) {
                list_values::bump(counters.waits_met_at_submit);
                continue;
            }
            ID3D12Fence* fence = nullptr;
            std::uint64_t value = 0;
            if (list_values::Pump::get().relay(w, fence, value) && SUCCEEDED(wait(fence, value))) {
                const std::uint64_t relayed = counters.waits_relayed.fetch_add(1, std::memory_order_relaxed) + 1;
                if (relayed <= 4)  // the first few, to show the model at work in a boot log
                    std::fprintf(stderr, "[rt] d3d12_xs: a wait for 0x%llx is relayed onto a write in flight (fence "
                                         "value %llu)\n",
                                 static_cast<unsigned long long>(w.address), static_cast<unsigned long long>(value));
                continue;
            }
            unmet.push_back(w);
        }
        if (unmet.empty()) return;
        const std::uint64_t value = ++values->gate_value;
        list_values::bump(counters.gates_held);
        list_values::Pump::get().add_gate(queue_->values, value, std::move(unmet));
        wait(values->gate, value);
    }

    ChildProxy* queue_;
    std::vector<ID3D12CommandList*> group_;
    std::vector<list_values::Write> writes_;
};

bool execute_with_values(ChildProxy* self, UINT count, void* const* lists) {
    bool any = false;
    for (UINT i = 0; i < count && !any; ++i)
        any = is_list_proxy_impl(lists[i]) && static_cast<const ListProxy*>(lists[i])->has_values();
    if (!any) return false;
    if (!self->values) {
        self->values = list_values::QueueValues::create(self->device->real,
                                                        reinterpret_cast<ID3D12CommandQueue*>(self->real));
        static std::atomic<bool> logged{false};
        if (!self->values && !logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: no fences for list memory writes and waits: their parts run "
                                 "without them\n");
    }
    ValueSubmission submission(self);
    for (UINT i = 0; i < count; ++i) {
        if (!is_list_proxy_impl(lists[i])) {
            const std::uint64_t entry = unwrap_object(reinterpret_cast<std::uint64_t>(lists[i]));
            submission.add(reinterpret_cast<ID3D12CommandList*>(entry));
            continue;
        }
        const auto* list = static_cast<const ListProxy*>(lists[i]);
        for (const ListProxy::Part& part : list->parts)
            submission.part(part.list, part.pre_writes, part.gate, part.writes);
        submission.part(list->real, list->pre_writes, list->gate, list->writes);
    }
    submission.flush();
    return true;
}

// Wraps `created` (one reference, owned by the proxy on success, released on failure) and returns `riid` from the
// proxy through `out`.
HRESULT wrap_list(IUnknown* created, DeviceProxy* device, const IID* riid, void** out,
                  ID3D12CommandAllocator* allocator = nullptr, std::uint64_t pso = 0,
                  D3D12_COMMAND_LIST_TYPE type = D3D12_COMMAND_LIST_TYPE_DIRECT) {
    if (!created) return E_FAIL;
    ListProxy* proxy = new (std::nothrow) ListProxy();  // (): the console body starts zeroed
    if (!proxy) {
        created->Release();
        return E_OUTOFMEMORY;
    }
    proxy->vtable = get_list_vtable();
    proxy->real = static_cast<ID3D12GraphicsCommandList*>(created);
    proxy->primary = proxy->real;
    proxy->allocator = allocator;  // a created list records into it, as after a Reset
    proxy->bundle = type == D3D12_COMMAND_LIST_TYPE_BUNDLE;
    proxy->track_state = list_values::enabled();
    proxy->track_values = proxy->track_state && !proxy->bundle;
    if (pso && proxy->track_state) proxy->state.pipeline(pso);
    proxy->device = device;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->pc_slot_count = detect_list_slot_count(created);
    proxy->pin_cursor_at_limit();
    proxy_add_ref(device);  // the list keeps its Xbox-ABI device alive for GetDevice

    const HRESULT hr = list_query_interface(proxy, riid, out);
    if (FAILED(hr)) {
        destroy_list(proxy);  // no reference escaped
        return hr;
    }
    static std::atomic<bool> logged{false};
    if (!d3d12_trace::enabled() && (!logged.exchange(true, std::memory_order_relaxed) || trace_enabled()))
        std::fprintf(stderr, "[rt] d3d12_xs: list proxy %p wraps %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(created), proxy->pc_slot_count);
    proxy->refs.fetch_sub(1, std::memory_order_relaxed);  // drop the creation reference; `out` holds the caller's
    return S_OK;
}

// Device slot 12: CreateCommandList(node_mask, type, allocator, initial_pso, riid, out). The title hands its
// allocator proxy, which the real device unwraps to the real allocator; the created list goes back console-shaped.
HRESULT proxy_create_command_list(DeviceProxy* self, std::uint64_t node_mask, std::uint64_t type,
                                  std::uint64_t allocator, std::uint64_t pso, const IID* riid, void** out) {
    note_slot(12);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    if (!out) return self->real->CreateCommandList(static_cast<UINT>(node_mask),
                                                 static_cast<D3D12_COMMAND_LIST_TYPE>(type),
                                                 reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)),
                                                 reinterpret_cast<ID3D12PipelineState*>(pso), *riid, nullptr);
    if (!self->console_lists) {
        // PC caller (create_device_proxy's console_lists=false): the real list answers every PC slot, and the
        // console-only pieces (slot 148, the shifted tail) are simply not there for this caller to reach.
        return self->real->CreateCommandList(static_cast<UINT>(node_mask),
                                             static_cast<D3D12_COMMAND_LIST_TYPE>(type),
                                             reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)),
                                             reinterpret_cast<ID3D12PipelineState*>(pso), *riid, out);
    }
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandList(
        static_cast<UINT>(node_mask), static_cast<D3D12_COMMAND_LIST_TYPE>(type),
        reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)),
        reinterpret_cast<ID3D12PipelineState*>(pso), __uuidof(ID3D12GraphicsCommandList), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    return wrap_list(static_cast<IUnknown*>(created), self, riid, out,
                     reinterpret_cast<ID3D12CommandAllocator*>(unwrap_object(allocator)), pso,
                     static_cast<D3D12_COMMAND_LIST_TYPE>(type));
}

// Calls the real device's slot `Slot` with these five words in the positions the thunk received them (see "A thunk
// declares 14 argument words" at the top: no word may move to another stack position on the way).
template <unsigned Slot>
std::uint64_t forward_device(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                             std::uint64_t a4, std::uint64_t a5) {
    using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                      std::uint64_t);
    return reinterpret_cast<Fn>(self->real_vtable[Slot])(self->real, a1, a2, a3, a4, a5);
}

// A device slot past the PC device's vtable (the real device predates the interface): E_NOTIMPL, logged once per
// slot. The generic thunk and the slots the proxy handles itself (31/34/35/45/46/50, 75) log the same line.
HRESULT refuse_beyond_device(unsigned slot, unsigned slot_count) {
    bool expected = false;
    if (slot < kMaxSlots && g_beyond_warned[slot].compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: device slot %u (%s) is beyond the PC device vtable (%u slots); returning "
                     "E_NOTIMPL\n",
                     slot, slot_name(slot), slot_count);
    return E_NOTIMPL;
}

// Device slot 8: CreateCommandQueue(desc, riid, out). Without an output it is the device's own capability answer
// (S_FALSE for a valid description on the PC), as #558 does for slot 14.
HRESULT proxy_create_command_queue(DeviceProxy* self, const D3D12_COMMAND_QUEUE_DESC* desc, const IID* riid,
                                   void** out) {
    note_slot(8);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    if (!riid) return E_POINTER;
    // The caller's null output goes to the real device on purpose: its answer (S_FALSE, E_UNEXPECTED, ...) is the
    // one to give, although the SDK annotates the parameter as required.
#pragma warning(suppress : 6387)
    if (!out) return self->real->CreateCommandQueue(desc, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandQueue(desc, __uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    const HRESULT wrapped = wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
#ifdef LEONIDA_VULKAN_DEVICE
    if (SUCCEEDED(wrapped) && desc && desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT && rt::package_splash::configured())
        present_state_for(self, true);
#endif
    return wrapped;
}

// Device slot 48, measured on the retail title: (desc, riid, out) with riid = IID_ID3D12CommandQueue. On the PC the
// slot is OpenExistingHeapFromAddress, so forwarding it by number fails. desc is a PC D3D12_COMMAND_QUEUE_DESC
// (16 bytes) followed by four more 32-bit Xbox words of unknown meaning, logged once. Only the PC prefix is used and
// only the flag the PC knows is kept; the rest is the slot 8 path.
HRESULT proxy_create_command_queue_xbox(DeviceProxy* self, const std::uint32_t* desc, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc) return E_INVALIDARG;
    static_assert(sizeof(D3D12_COMMAND_QUEUE_DESC) == 16, "the PC prefix of the Xbox description is 16 bytes");
    D3D12_COMMAND_QUEUE_DESC pc{};
    std::memcpy(&pc, desc, sizeof(pc));
    pc.Flags &= D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 48 queue: flags=0x%x extra words=0x%x 0x%x 0x%x 0x%x\n",
                     static_cast<unsigned>(desc[2]), desc[4], desc[5], desc[6], desc[7]);
    return proxy_create_command_queue(self, &pc, riid, out);
}

// Xbox device slot 78 (public HLE): SetFrameIntervalX(output, interval_us, latency, flags). The output and flags are
// not used before a presenter exists. The period is a vblank period in microseconds in the public samples (16667 for
// 60 Hz), and latency is the allowed frames in flight. S_OK even for 0: a boot must not abort here, and nothing paces.
HRESULT proxy_set_frame_interval_x(DeviceProxy* self, std::uint64_t output, std::uint64_t interval_us,
                                   std::uint64_t latency, std::uint64_t flags) {
    HRESULT hr = S_OK;
    if (call_presentation(self, [&](const PresentationHandler& p) {
            if (!p.set_frame_interval) return false;
            hr = p.set_frame_interval(p.context, reinterpret_cast<IUnknown*>(output), static_cast<UINT>(interval_us),
                                      static_cast<UINT>(latency), static_cast<UINT>(flags));
            return true;
        }))
        return hr;
    self->frame_interval_us.store(static_cast<std::uint32_t>(interval_us), std::memory_order_relaxed);
    self->frame_latency.store(static_cast<std::uint32_t>(latency ? latency : 1), std::memory_order_relaxed);
#ifdef LEONIDA_VULKAN_DEVICE
    if (auto* presentation = device_presentation(self))
        return presentation->SetFrameIntervalX(reinterpret_cast<IUnknown*>(output), static_cast<UINT>(interval_us),
                                               static_cast<UINT>(latency), static_cast<UINT>(flags));
#endif
    return S_OK;
}

// Xbox device slot 79 (public HLE): ScheduleFrameEventX(event, offset_us, pEvent, flags); our pacer has only the origin.
HRESULT proxy_schedule_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t offset_us,
                                     std::uint64_t flags) {
    HRESULT hr = S_OK;
    if (call_presentation(self, [&](const PresentationHandler& p) {
            if (!p.schedule_frame_event) return false;
            hr = p.schedule_frame_event(p.context, static_cast<UINT>(event), static_cast<UINT>(offset_us),
                                        static_cast<UINT>(flags));
            return true;
        }))
        return hr;
    self->frame_event_offset_us.store(static_cast<std::uint32_t>(offset_us), std::memory_order_relaxed);
    self->frame_event_scheduled.store(true, std::memory_order_relaxed);
#ifdef LEONIDA_VULKAN_DEVICE
    if (auto* presentation = device_presentation(self))
        return presentation->ScheduleFrameEventX(static_cast<UINT>(event), static_cast<UINT>(offset_us),
                                                 static_cast<UINT>(flags));
#endif
    return S_OK;
}

// Xbox device slot 80 (public HLE): WaitFrameEventX(event, timeout_ms, pEvent, flags, &token). When a presenter is
// attached, forwards directly to it; otherwise returns at once with a fresh non-zero token so boot tests cannot stall.
HRESULT proxy_wait_frame_event_x(DeviceProxy* self, std::uint64_t event, std::uint64_t timeout_ms,
                                 std::uint64_t flags, std::uint64_t token_word) {
    auto* token = reinterpret_cast<std::uint64_t*>(token_word);
    HRESULT hr = S_OK;
    if (call_presentation(self, [&](const PresentationHandler& p) {
            if (!p.wait_frame_event) return false;
            hr = p.wait_frame_event(p.context, static_cast<UINT>(event), static_cast<DWORD>(timeout_ms),
                                    static_cast<UINT>(flags), token);
            return true;
        }))
        return hr;
    if (!token) return E_POINTER;
#ifdef LEONIDA_VULKAN_DEVICE
    if (auto* presentation = device_presentation(self))
        return presentation->WaitFrameEventX(static_cast<UINT>(event), static_cast<DWORD>(timeout_ms),
                                             static_cast<UINT>(flags), token);
#endif
    *token = self->frame_tokens->issue();
    return S_OK;
}

// Xbox device slot 73 (public HLE): CreateCommandListX(desc, allocator, initial_state, riid, out). The console call is
// not the PC six-argument form: the type is the first word of D3D12XBOX_COMMAND_LIST_DESC and there is no node mask
// (the HLE passes 0). The second desc word is logged, not interpreted: its public layout is not established.
HRESULT proxy_create_command_list_x(DeviceProxy* self, std::uint64_t desc_word, std::uint64_t allocator_word,
                                    std::uint64_t initial_state_word, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (!desc_word || !allocator_word) return E_INVALIDARG;
    if (!riid) return E_POINTER;
    if (!out) return E_POINTER;

    std::uint32_t words[2] = {};
    std::memcpy(words, reinterpret_cast<const void*>(desc_word), sizeof(words));
    auto type = static_cast<D3D12_COMMAND_LIST_TYPE>(words[0]);
    if (type != D3D12_COMMAND_LIST_TYPE_DIRECT && type != D3D12_COMMAND_LIST_TYPE_BUNDLE &&
        type != D3D12_COMMAND_LIST_TYPE_COMPUTE && type != D3D12_COMMAND_LIST_TYPE_COPY)
        type = D3D12_COMMAND_LIST_TYPE_DIRECT;  // as the public HLE does

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 73 CreateCommandListX desc words 0x%08x 0x%08x -> node mask 0, type %d\n",
                     words[0], words[1], static_cast<int>(type));

    // The list comes from the slot 12 path (the allocator proxy unwrapped there), as the console list proxy over a PC
    // graphics list. Measured locally at a title's call sites: riid is a console command-list IID that no PC object
    // answers, so asking the PC device for it fails every list. The proxy is the console-shaped list that IID asks
    // for: an IID the proxy answers is returned as asked, and any other is answered with the proxy itself.
    void* list = nullptr;
    const HRESULT hr = proxy_create_command_list(self, 0, static_cast<std::uint64_t>(type), allocator_word,
                                                 initial_state_word, &__uuidof(ID3D12GraphicsCommandList), &list);
    if (FAILED(hr)) return hr;
    if (!list) return E_FAIL;
    auto* unknown = static_cast<IUnknown*>(list);
    if (IsEqualIID(*riid, __uuidof(ID3D12GraphicsCommandList))) {
        *out = list;
        return S_OK;
    }
    void* asked = nullptr;
    if (SUCCEEDED(unknown->QueryInterface(*riid, &asked)) && asked) {
        unknown->Release();
        *out = asked;
        return S_OK;
    }
    static std::atomic<bool> console_iid_logged{false};
    if (first_call(console_iid_logged))
        std::fprintf(stderr, "[rt] d3d12_xs: slot 73 asks for a list IID the PC does not know; answered with the "
                             "console list proxy\n");
    *out = list;
    return S_OK;
}

// Xbox device slots 70 and 71: (resource words, view words, destination), no return value. The retail title's
// shader-resource (70) and unordered-access (71) views of memory it placed itself, with no resource object
// (docs/retail_boot_chain.md, stop 14). The PC slots are CreatePlacedResource1 and CreateSamplerFeedbackUnordered-
// AccessView. The layout is VIctory's measurement for this title (crates/victory-gpu/src/descriptor.rs and
// descriptor/host.rs, "device methods 0x230 and 0x238", GPL-3.0-or-later):
//   - resource words: 14 dwords in the shape of a PC D3D12_RESOURCE_DESC: dword 0 the dimension (1 a buffer),
//     dwords 4-5 the width in bytes;
//   - view words: 16 dwords: 0-9 the PC D3D12_SHADER_RESOURCE_VIEW_DESC (slot 70) or D3D12_UNORDERED_ACCESS_VIEW_DESC
//     (slot 71), 10-11 the 64-bit address of the memory, 12-15 console fields (not read).
// A buffer view gets its V# on that address (vsharp_address gives title memory a GPU copy). A texture view gets the
// T# of the slot-133 texture placed at the address, or of an image made for that memory, filled from it with its
// console layout (XG) and filled again when the bytes change (rt::d3d12::write_placed_texture_view); what that
// cannot show gets a null descriptor. UAVs require a live, matching GPU-writable texture at the placement.
// SRVs over pending streams are filled once their pages arrive. The first calls and every 4096th are logged.
#ifdef LEONIDA_VULKAN_DEVICE
static bool accessible_range(const void* ptr, std::size_t bytes, bool writable);
#endif

struct PlacedViewCounts {
    std::atomic<std::uint64_t> calls{0}, buffers{0}, textures{0}, refused{0}, texture_views{0};
};
PlacedViewCounts g_placed_views[2];  // SRV, UAV

// The providers a shader-resource view of slot 70 asks when the placed-memory path below wrote nothing.
//
// The one measured shape (PR 7066, eugen1763, first retail call) is a raw buffer view: texture_words starts with a
// desktop D3D12_RESOURCE_DESC (buffer), view_desc with a desktop D3D12_SHADER_RESOURCE_VIEW_DESC (R32_TYPELESS,
// BUFFER, RAW flag) followed at once by a D3D12_GPU_VIRTUAL_ADDRESS, the console VA slot 133 registered. For that shape
// and a VA inside a buffer the resource provider knows, the destination gets a raw view of that range: FirstElement
// (va - resource start) / 4, NumElements the description's Width / 4, cut to the end of the resource. A 2D texture
// shape goes to the raw texture view provider. Anything else (another dimension or format, no RAW flag, a VA no buffer
// holds, no provider, unreadable words) writes nothing, with one log line saying why.
struct RawSrvShape {
    D3D12_RESOURCE_DESC resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC view;
    D3D12_GPU_VIRTUAL_ADDRESS va;
};

bool readable(std::uint64_t address, std::size_t bytes) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
    if (info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address + bytes <= end;
}

// Reads the measured shape from the two words into `out`. Returns null when it parsed, else why it is not that shape.
const char* parse_raw_srv(std::uint64_t texture_words, std::uint64_t view_desc, RawSrvShape& out) {
    if (!readable(texture_words, sizeof(D3D12_RESOURCE_DESC)) ||
        !readable(view_desc, sizeof(D3D12_SHADER_RESOURCE_VIEW_DESC) + sizeof(D3D12_GPU_VIRTUAL_ADDRESS)))
        return "the words are not readable";
    std::memcpy(&out.resource, reinterpret_cast<const void*>(texture_words), sizeof(out.resource));
    std::memcpy(&out.view, reinterpret_cast<const void*>(view_desc), sizeof(out.view));
    std::memcpy(&out.va, reinterpret_cast<const std::uint8_t*>(view_desc) + sizeof(out.view), sizeof(out.va));
    if (out.resource.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return "arg1 is not a buffer description";
    if (out.view.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) return "arg2 is not a buffer view";
    if (out.view.Format != DXGI_FORMAT_R32_TYPELESS || !(out.view.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW))
        return "arg2 is not a raw R32_TYPELESS view";
    if (!out.resource.Width || !out.va) return "no width or no address";
    return nullptr;
}

void log_slot70_null_once(const char* why, std::uint64_t texture_words, std::uint64_t view_desc) {
    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr, "[rt] d3d12_xs: slot 70 writes a null view: %s (args 0x%llx 0x%llx)\n", why,
                     static_cast<unsigned long long>(texture_words), static_cast<unsigned long long>(view_desc));
}

// True when a view was written to `destination` (the real heap's handle; `title_slot` is the handle the title gave).
bool create_srv_from_raw_x(DeviceProxy* self, std::uint64_t texture_words, std::uint64_t view_desc,
                           D3D12_CPU_DESCRIPTOR_HANDLE title_slot, SIZE_T destination) {
    RawSrvShape shape{};
    const char* why = parse_raw_srv(texture_words, view_desc, shape);
    if (shape.resource.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && self->raw_texture_view && shape.va &&
        self->raw_texture_view(self->real, shape.resource, shape.view, shape.va, {static_cast<SIZE_T>(destination)})) {
        local_write_through(self, title_slot, 1);
        return true;
    }
    if (why || !self->resource_provider) {
        log_slot70_null_once(why ? why : "no resource provider", texture_words, view_desc);
        return false;
    }
    std::uint64_t start = 0, size = 0;
    ID3D12Resource* resource = self->resource_provider(self->real, shape.va, shape.resource.Width, &start, &size);
    if (!resource || shape.va < start || shape.va - start >= size) {
        if (resource) resource->Release();
        // LOCAL: a small raw view over the title's own memory becomes a raw buffer V# through the constant-view path,
        // which resolves a title address to its GPU copy (#9137). LEONIDA_SLOT70_TITLE=0 keeps the null view.
        static const bool title_views = [] { const char* v = std::getenv("LEONIDA_SLOT70_TITLE"); return !(v && *v == '0'); }();
        std::uint64_t title_lo = 0, title_hi = 0;
        if (title_views && shape.resource.Width && shape.resource.Width <= 65536 &&
            leonida::title_va_aliases().title_memory(shape.va, title_lo, title_hi) &&
            shape.resource.Width <= title_hi - shape.va) {
            D3D12_CONSTANT_BUFFER_VIEW_DESC constants{};
            constants.BufferLocation = shape.va;
            constants.SizeInBytes = static_cast<UINT>(shape.resource.Width);
            self->real->CreateConstantBufferView(&constants, D3D12_CPU_DESCRIPTOR_HANDLE{destination});
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                std::fprintf(stderr, "[rt] d3d12_xs: local: slot 70 view over title memory (%llu bytes) written as a raw "
                                     "buffer V# on its GPU copy\n", static_cast<unsigned long long>(shape.resource.Width));
            return true;
        }
        log_slot70_null_once("no buffer holds the address", texture_words, view_desc);
        return false;
    }
    const std::uint64_t offset = shape.va - start;
    const std::uint64_t elements = std::min<std::uint64_t>(shape.resource.Width, size - offset) / 4;
    if (offset % 4 || !elements || elements > UINT32_MAX) {
        resource->Release();
        log_slot70_null_once("address or width is not 4-byte aligned or too large", texture_words, view_desc);
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_TYPELESS;
    view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    view.Shader4ComponentMapping = shape.view.Shader4ComponentMapping;
    view.Buffer.FirstElement = offset / 4;
    view.Buffer.NumElements = static_cast<UINT>(elements);
    view.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    self->real->CreateShaderResourceView(resource, &view, D3D12_CPU_DESCRIPTOR_HANDLE{destination});
    resource->Release();
    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr, "[rt] d3d12_xs: slot 70 wrote a raw buffer view (va 0x%llx, first element %llu, %u elements)\n",
                     static_cast<unsigned long long>(shape.va), static_cast<unsigned long long>(view.Buffer.FirstElement),
                     view.Buffer.NumElements);
    return true;
}

void proxy_create_placed_view_x(DeviceProxy* self, bool unordered, std::uint64_t resource_words,
                                std::uint64_t view_words, std::uint64_t destination) {
    const unsigned slot = unordered ? 71 : 70;
    note_slot(slot);
    log_xbox_route_once(slot, unordered ? "CreateUnorderedAccessView of placed memory"
                                        : "CreateShaderResourceView of placed memory",
                        resource_words, view_words, destination, 0, 0);
    if (!destination) return;
    // LOCAL: whatever this call writes to the real slot is stored at the title's address too when it returns.
    struct LocalWriteThrough {
        DeviceProxy* self;
        std::uint64_t custom;
        ~LocalWriteThrough() { local_write_through(self, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(custom)}, 1); }
    } local_write_back{self, destination};
    const D3D12_CPU_DESCRIPTOR_HANDLE dest = translate_cpu_handle(self, D3D12_CPU_DESCRIPTOR_HANDLE{destination});
    PlacedViewCounts& counts = g_placed_views[unordered ? 1 : 0];
    const std::uint64_t n = counts.calls.fetch_add(1, std::memory_order_relaxed) + 1;
    std::uint32_t resource[14] = {}, view[16] = {};
    bool read = false;
#ifdef LEONIDA_VULKAN_DEVICE
    read = resource_words && view_words &&
           accessible_range(reinterpret_cast<const void*>(resource_words), sizeof(resource), false) &&
           accessible_range(reinterpret_cast<const void*>(view_words), sizeof(view), false);
#endif
    if (read) {
        std::memcpy(resource, reinterpret_cast<const void*>(resource_words), sizeof(resource));
        std::memcpy(view, reinterpret_cast<const void*>(view_words), sizeof(view));
    }
    const std::uint64_t address = std::uint64_t(view[10]) | (std::uint64_t(view[11]) << 32);
    const std::uint64_t width = std::uint64_t(resource[4]) | (std::uint64_t(resource[5]) << 32);
    const bool buffer = read && resource[0] == std::uint32_t(D3D12_RESOURCE_DIMENSION_BUFFER);
    bool written = false, texture_written = false;
#ifdef LEONIDA_VULKAN_DEVICE
    if (buffer) written = rt::d3d12::write_placed_buffer_view(self->real, unordered, view, address, width, dest);
    // A texture view: the T# of an image filled from the title's memory at the address (TitleTexture in
    // d3d12_vulkan_device.cpp), or a null view with the reason logged there.
    if (read && !buffer)
        texture_written = rt::d3d12::write_placed_texture_view(self->real, unordered, resource, view, address, dest);
#endif
    (written ? counts.buffers : buffer ? counts.refused : texture_written ? counts.texture_views : counts.textures)
        .fetch_add(1, std::memory_order_relaxed);
    if (n <= 8 || !(n % 4096))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot %u call %llu: %s of placed memory 0x%llx (%llu bytes), dimension %u, view "
                     "format %u dimension %u -> %s (so far: %llu buffer views, %llu refused, %llu texture views, "
                     "%llu textures as null views)\n",
                     slot, static_cast<unsigned long long>(n), unordered ? "UAV" : "SRV",
                     static_cast<unsigned long long>(address), static_cast<unsigned long long>(width), resource[0],
                     view[0], view[1],
                     written           ? "a buffer V#"
                     : texture_written ? "a texture T#"
                     : !read           ? "unreadable words, null view"
                     : buffer          ? "refused, null view"
                                       : "texture, null view",
                     static_cast<unsigned long long>(counts.buffers.load()),
                     static_cast<unsigned long long>(counts.refused.load()),
                     static_cast<unsigned long long>(counts.texture_views.load()),
                     static_cast<unsigned long long>(counts.textures.load()));
    if (written || texture_written) return;
    if (!unordered &&
        create_srv_from_raw_x(self, resource_words, view_words,
                              D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(destination)}, dest.ptr))
        return;
    if (unordered) {
        // A null UAV of the view's kind: a buffer, or a 2D texture for anything else.
        D3D12_UNORDERED_ACCESS_VIEW_DESC null_view{};
        null_view.Format = DXGI_FORMAT_R32_UINT;
        null_view.ViewDimension = buffer ? D3D12_UAV_DIMENSION_BUFFER : D3D12_UAV_DIMENSION_TEXTURE2D;
        self->real->CreateUnorderedAccessView(nullptr, nullptr, &null_view, dest);
        return;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC null_view{};
    null_view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    null_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    null_view.Texture2D.MipLevels = 1;
    // The destination can be a handle inside a heap backed by the title's XMem hook (slot 14): translated as slots 17-24.
    self->real->CreateShaderResourceView(nullptr, &null_view, dest);
}

// Device slot 75: ID3D12Device9::CreateCommandQueue1(desc, creator_id, riid, out), called by slot index.
HRESULT proxy_create_command_queue1(DeviceProxy* self, std::uint64_t desc, std::uint64_t creator_id,
                                    const IID* riid, void** out) {
    note_slot(75);
    if (out) *out = nullptr;  // as slot 8
    if (self->slot_count <= 75) return refuse_beyond_device(75, self->slot_count);  // predates ID3D12Device9
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, std::uint64_t, std::uint64_t, const IID*, void**);
    const auto create = reinterpret_cast<Fn>(self->real_vtable[75]);
    if (!out) return create(self->real, desc, creator_id, riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = create(self->real, desc, creator_id, &__uuidof(ID3D12CommandQueue), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::queue>(static_cast<IUnknown*>(created), self, riid, out);
}

// Device slot 36: CreateFence(initial_value, flags, riid, out).
HRESULT proxy_create_fence(DeviceProxy* self, std::uint64_t initial_value, std::uint64_t flags, const IID* riid,
                           void** out) {
    note_slot(36);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto fence_flags = static_cast<D3D12_FENCE_FLAGS>(static_cast<std::uint32_t>(flags));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateFence(initial_value, fence_flags, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateFence(initial_value, fence_flags, __uuidof(ID3D12Fence), &created);
    if (FAILED(hr)) return hr;
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(created), self, riid, out);
}

// True when `allocator` is one whose consumers resolve allocator proxies: Leonida's own Vulkan-layer allocator,
// whose lists (CreateCommandList, Reset) find the real allocator through allocator_proxy_real_allocator. A PC
// runtime allocator does not answer the tag, and its lists would take a proxy for their own allocator.
bool resolves_allocator_proxies(IUnknown* allocator) {
    void* found = nullptr;
    if (FAILED(allocator->QueryInterface(kLeonidaCommandAllocatorTag, &found)) || !found) return false;
    static_cast<IUnknown*>(found)->Release();  // the implementation pointer starts with its IUnknown chain
    return true;
}

// Device slot 9: CreateCommandAllocator(type, riid, out). An allocator proxy where the list side resolves it (see
// resolves_allocator_proxies); any other allocator goes back unchanged, logged once, so the host runtime's lists
// never see a proxy. Null output and null IID as slot 8.
HRESULT proxy_create_command_allocator(DeviceProxy* self, std::uint64_t type, const IID* riid, void** out) {
    note_slot(9);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto list_type = static_cast<D3D12_COMMAND_LIST_TYPE>(static_cast<std::uint32_t>(type));
#pragma warning(suppress : 6387)  // the caller's null output, on purpose (see slot 8)
    if (!out) return self->real->CreateCommandAllocator(list_type, *riid, nullptr);
    void* created = nullptr;
    const HRESULT hr = self->real->CreateCommandAllocator(list_type, __uuidof(ID3D12CommandAllocator), &created);
    if (FAILED(hr)) return hr;
    if (!created) return E_FAIL;
    auto* allocator = static_cast<IUnknown*>(created);
    if (!resolves_allocator_proxies(allocator)) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: allocators of this device are not wrapped: its command lists "
                                 "cannot resolve an allocator proxy\n");
        const HRESULT asked = allocator->QueryInterface(*riid, out);
        allocator->Release();
        return asked;
    }
    return wrap_child<ChildKind::allocator>(allocator, self, riid, out);
}

// Device slot 32: OpenSharedHandle(handle, riid, out). The object is opened as asked; when it is a fence (whatever
// interface was asked for, IUnknown included) the title gets a fence proxy, anything else (heaps, resources) is
// returned unchanged.
HRESULT proxy_open_shared_handle(DeviceProxy* self, std::uint64_t handle, const IID* riid, void** out) {
    note_slot(32);
    if (out) *out = nullptr;  // as slot 8
    if (!riid) return E_POINTER;
    const auto shared = reinterpret_cast<HANDLE>(handle);
    if (!out) return self->real->OpenSharedHandle(shared, *riid, nullptr);
    void* opened = nullptr;
    HRESULT hr = self->real->OpenSharedHandle(shared, *riid, &opened);
    if (FAILED(hr) || !opened) return FAILED(hr) ? hr : E_FAIL;
    void* fence = nullptr;
    if (FAILED(static_cast<IUnknown*>(opened)->QueryInterface(__uuidof(ID3D12Fence), &fence)) || !fence) {
        *out = opened;  // not a fence
        return hr;
    }
    static_cast<IUnknown*>(opened)->Release();
    return wrap_child<ChildKind::fence>(static_cast<IUnknown*>(fence), self, riid, out);
}

// Device slots that take heap, queue or fence objects: the real runtime gets the real objects.
//   31 CreateSharedHandle(object, attributes, access, name, handle)
//   34 MakeResident(count, objects)      35 Evict(count, objects)
//   45 SetEventOnMultipleFenceCompletion(fences, values, count, flags, event)            (ID3D12Device1)
//   46 SetResidencyPriority(count, objects, priorities)                                  (ID3D12Device1)
//   50 EnqueueMakeResident(flags, count, objects, fence, value)                          (ID3D12Device3)
template <unsigned Slot>
HRESULT proxy_unwrapping_call(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                              std::uint64_t a4, std::uint64_t a5) {
    note_slot(Slot);
    if (Slot >= self->slot_count) return refuse_beyond_device(Slot, self->slot_count);  // predates the interface
    UnwrappedArray objects;
    std::uint64_t r1 = a1, r2 = a2, r3 = a3, r4 = a4;
    if constexpr (Slot == 31) {
        r1 = unwrap_object(a1);
    } else if constexpr (Slot == 34 || Slot == 35 || Slot == 46) {
        if (!objects.fill(a2, a1)) return E_OUTOFMEMORY;
        if (objects.data()) r2 = objects.data();
    } else if constexpr (Slot == 45) {
        if (!objects.fill(a1, a3)) return E_OUTOFMEMORY;
        if (objects.data()) r1 = objects.data();
    } else if constexpr (Slot == 50) {
        if (!objects.fill(a3, a2)) return E_OUTOFMEMORY;
        if (objects.data()) r3 = objects.data();
        r4 = unwrap_object(a4);
    }
    // r1-r4 differ from a1-a4 only in a position the method takes, so the title's frame has that word.
    return static_cast<HRESULT>(static_cast<std::uint32_t>(forward_device<Slot>(self, r1, r2, r3, r4, a5)));
}

// A console descriptor heap carries a size as well as an address, and that size is 32 bytes per descriptor. A PC
// desc's trailing words are whatever the caller holds next; they do not encode NumDescriptors * 32, so this
// separates the two layouts without guessing anything about the address itself.
static bool console_heap_range_plausible(const D3D12_DESCRIPTOR_HEAP_DESC* desc, std::uint64_t address,
                                         std::uint64_t size) {
    constexpr std::uint64_t kConsoleDescriptorSize = 32;
    if (address == 0 || size == 0) return false;
    if (desc->NumDescriptors == 0 || desc->NumDescriptors > (0xFFFFFFFFull / kConsoleDescriptorSize)) return false;
    if (size != static_cast<std::uint64_t>(desc->NumDescriptors) * kConsoleDescriptorSize) return false;
    return true;
}

static bool accessible_range(const void* ptr, std::size_t bytes, bool writable) {
    if (!ptr || !bytes) return false;
    const auto* p = static_cast<const std::uint8_t*>(ptr);
    std::size_t remaining = bytes;
    while (remaining) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xffu;
        if ((mbi.Protect & PAGE_GUARD) || protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) return false;
        if (writable && protect != PAGE_READWRITE && protect != PAGE_WRITECOPY &&
            protect != PAGE_EXECUTE_READWRITE && protect != PAGE_EXECUTE_WRITECOPY) return false;
        const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
        const auto addr = reinterpret_cast<std::uintptr_t>(p);
        const std::size_t avail = static_cast<std::size_t>(base + mbi.RegionSize - addr);
        if (avail >= remaining) return true;
        remaining -= avail;
        p += avail;
    }
    return true;
}


HRESULT proxy_create_descriptor_heap(DeviceProxy* self, const D3D12_DESCRIPTOR_HEAP_DESC* desc, const IID* riid,
                                     void** out) {
    note_slot(14);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as the PC device does
    if (!riid) return E_POINTER;
    // No output: a capability check, answered by the device (S_FALSE, as on the PC).
    if (!out) return self->real->CreateDescriptorHeap(desc, *riid, nullptr);

    void* created = nullptr;
    HRESULT hr = E_INVALIDARG;
    bool console_extended = false;
    // Console-extended slot 14 (issue #313): the address and size of the title's own descriptor memory follow the PC
    // description. The device behind the proxy registers the handler that interprets them
    // (set_console_descriptor_heap_handler); on the Vulkan layer only its own devices take the path (a PC device
    // behind the proxy gets the PC call).
    const ConsoleDescriptorHeapHandler handler = g_console_descriptor_heap.load(std::memory_order_acquire);
#ifdef LEONIDA_VULKAN_DEVICE
    const bool console_capable = handler && rt::d3d12::vulkan_device_info(self->real);
#else
    const bool console_capable = handler != nullptr;
#endif
    if (desc && console_capable && desc->Type <= D3D12_DESCRIPTOR_HEAP_TYPE_DSV &&
        (desc->Flags & ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(desc);
        const void* tail = bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC);
        if (accessible_range(tail, sizeof(std::uint64_t) * 2, false)) {
            std::uint64_t address = 0;
            std::uint64_t size = 0;
            std::memcpy(&address, tail, sizeof(address));
            std::memcpy(&size, bytes + sizeof(D3D12_DESCRIPTOR_HEAP_DESC) + sizeof(address), sizeof(size));
            const auto stride = self->real->GetDescriptorHandleIncrementSize(desc->Type);
            // LOCAL DIAGNOSTIC: what the title passes on slot 14, and whether the console path takes it
            if (leonida::local_diagnostics()) {
                static std::atomic<int> logged{0};
                if (logged.fetch_add(1) < 32)
                    std::fprintf(stderr, "[rt] heap-probe: slot 14 type %d num %u flags 0x%x console va 0x%llx size %llu "
                                         "(num*stride %llu, our stride %u, size/num %.2f) writable %d\n",
                                 int(desc->Type), desc->NumDescriptors, unsigned(desc->Flags),
                                 static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                                 static_cast<unsigned long long>(std::uint64_t(desc->NumDescriptors) * stride), stride,
                                 desc->NumDescriptors ? double(size) / desc->NumDescriptors : 0.0,
                                 address && size ? int(accessible_range(reinterpret_cast<const void*>(address), size, true)) : -1);
            }
            // A non-zero word is not evidence of that layout - the bytes after a PC desc are whatever the caller holds
            // next, and they are almost always readable and non-zero. The size has to agree with the descriptor count
            // (32 bytes each, the console's and this layer's stride), and the range has to be the title's writable
            // memory; otherwise the PC heap stands (a read-only or mismatched range falls back). Physical pages are
            // committed on first touch (xmem), so the range's pages are committed first, as slot 133 does: a range the
            // title mapped but has not written yet is still reserved and would read as not writable.
            // The range may also lie under a slot-133 shadow or a title window whose write watch made its pages
            // read-only (the title places a buffer over the memory it carves the heap from); those pages are the
            // title's writable memory all the same, so they are pinned writable while the range is checked.
            const bool plausible = console_heap_range_plausible(desc, address, size) && address <= UINTPTR_MAX - size;
            std::uint64_t pin = 0;
            if (plausible) {
                if (const PlacedAtVaPagesHook pages = g_placed_at_va_pages.load(std::memory_order_acquire))
                    pages(reinterpret_cast<const void*>(address), static_cast<std::size_t>(size));
                pin = leonida::kernel_writes::begin_write(reinterpret_cast<const void*>(address),
                                                          static_cast<std::size_t>(size), nullptr);
            }
            const bool writable = plausible && accessible_range(reinterpret_cast<const void*>(address), size, true);
            leonida::kernel_writes::end_write(pin, false);
            if (writable && size == std::uint64_t(desc->NumDescriptors) * stride) {
                console_extended = true;
                hr = handler(self->real, desc, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(address), size,
                             __uuidof(ID3D12DescriptorHeap), &created);
            }
        }
    }
    if (!console_extended)
        hr = self->real->CreateDescriptorHeap(desc, __uuidof(ID3D12DescriptorHeap), &created);
    if (leonida::local_diagnostics()) {  // LOCAL DIAGNOSTIC
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32)
            std::fprintf(stderr, "[rt] heap-probe: slot 14 -> %s heap, hr 0x%08lx\n", console_extended ? "CONSOLE" : "pc",
                         static_cast<unsigned long>(hr));
    }
    if (FAILED(hr) || !created) return FAILED(hr) ? hr : E_FAIL;

    ID3D12DescriptorHeap* heap = static_cast<ID3D12DescriptorHeap*>(created);
    SIZE_T custom_cpu_start = 0;
    SIZE_T custom_byte_size = 0;
    SIZE_T real_cpu_start = 0;
    SIZE_T stride = 0;
    DescriptorHeapBackingAlloc alloc_fn = nullptr;
    DescriptorHeapBackingFree free_fn = nullptr;
    {
        std::lock_guard lock(heap_alloc_mutex());
        alloc_fn = g_heap_alloc;
        free_fn = g_heap_free;
    }
    // A heap the console-extended handler made already sits at the address the title named: the hook-backed address
    // is for a heap made from a plain PC description only.
    if (!console_extended && alloc_fn && desc && desc->NumDescriptors > 0) {
        const UINT inc = self->real->GetDescriptorHandleIncrementSize(desc->Type);
        if (inc > 0 && desc->NumDescriptors <= (std::numeric_limits<SIZE_T>::max)() / inc) {
            const SIZE_T byte_size = static_cast<SIZE_T>(desc->NumDescriptors) * static_cast<SIZE_T>(inc);
            void* backing = alloc_fn(byte_size, kXmemDescriptorHeapAttributes);
            if (backing) {
                const auto addr = reinterpret_cast<SIZE_T>(backing);
                if (addr <= (std::numeric_limits<SIZE_T>::max)() - byte_size) {
                    custom_cpu_start = addr;
                    custom_byte_size = byte_size;
                    stride = inc;
                    real_cpu_start = raw_heap_get_cpu_handle(heap, *reinterpret_cast<void***>(heap)).ptr;
#ifdef LEONIDA_VULKAN_DEVICE
                    // A CPU-only heap of the Vulkan device keeps its descriptors in the backing itself: the title
                    // makes views there through the API and then moves the 32-byte records into its shader-visible
                    // heap with its own memcpy (no CopyDescriptors call), so the records must be in the memory its
                    // handles name. Without this the device wrote its own heap and the title copied the untouched
                    // backing (zeros or stale bytes; found by the fsdraw forensics on the console path).
                    if (!console_extended && !(desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) &&
                        rt::d3d12::vulkan_device_info(self->real)) {
                        void* in_backing = nullptr;
                        if (SUCCEEDED(rt::d3d12::create_console_descriptor_heap(
                                self->real, desc, static_cast<D3D12_GPU_VIRTUAL_ADDRESS>(addr), byte_size,
                                __uuidof(ID3D12DescriptorHeap), &in_backing)) &&
                            in_backing) {
                            heap->Release();
                            heap = static_cast<ID3D12DescriptorHeap*>(in_backing);
                            real_cpu_start = addr;
                            static std::atomic<bool> logged{false};
                            if (first_call(logged))
                                std::fprintf(stderr, "[rt] d3d12_xs: CPU-only descriptor heaps keep their descriptors "
                                                     "in the title's XMem backing (first: type %d, %u slots at %p)\n",
                                             int(desc->Type), desc->NumDescriptors, backing);
                        }
                    }
#endif
                } else if (free_fn) {
                    free_fn(backing, kXmemDescriptorHeapAttributes);
                }
            }
        }
    }

    hr = create_heap_proxy(heap, self, *riid, out, custom_cpu_start, custom_byte_size, real_cpu_start,
                           custom_cpu_start ? free_fn : nullptr, stride);
    if (SUCCEEDED(hr) && desc && desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV && is_heap_proxy(*out)) {
        // The null-view marks are kept by real handle (mark_null_rtv); heap_release drops those in this range.
        const UINT inc = self->real->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto* proxy = static_cast<HeapProxy*>(*out);
        proxy->rtv_real_start =
            real_cpu_start ? real_cpu_start : raw_heap_get_cpu_handle(heap, *reinterpret_cast<void***>(heap)).ptr;
        if (inc && desc->NumDescriptors <= (std::numeric_limits<SIZE_T>::max)() / inc)
            proxy->rtv_real_bytes = static_cast<SIZE_T>(desc->NumDescriptors) * inc;
    }
    if (leonida::fsdraw::enabled()) {  // LOCAL DIAGNOSTIC (fsdraw): which CPU start the title gets for this heap
        static std::atomic<int> logged{0};
        if (logged.fetch_add(1) < 32)
            std::fprintf(stderr, "[rt] fsdraw: heap type %d num %u flags 0x%x console %d: title CPU start 0x%llx (custom "
                                 "backing %s), real CPU start 0x%llx, hr 0x%08lx\n",
                         desc ? int(desc->Type) : -1, desc ? desc->NumDescriptors : 0u, desc ? unsigned(desc->Flags) : 0u,
                         int(console_extended),
                         static_cast<unsigned long long>(custom_cpu_start ? custom_cpu_start
                                                                          : raw_heap_get_cpu_handle(heap, *reinterpret_cast<void***>(heap)).ptr),
                         custom_cpu_start ? "yes" : "no",
                         static_cast<unsigned long long>(raw_heap_get_cpu_handle(heap, *reinterpret_cast<void***>(heap)).ptr),
                         static_cast<unsigned long>(hr));
    }
    if (FAILED(hr)) {
        heap->Release();
        if (custom_cpu_start && free_fn)
            free_fn(reinterpret_cast<void*>(custom_cpu_start), kXmemDescriptorHeapAttributes);
    }
    return hr;
}

// Device slots 17-24 write or copy descriptors using CPU handles. When a title supplies its own descriptor-heap base
// via __XMemAllocationHooks__, those handles fall in [custom_start, custom_end) and must be translated back to the
// underlying PC/Vulkan heap before forwarding.
void proxy_create_cbv(DeviceProxy* self, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(17);
    const auto target = translate_cpu_handle(self, dest);
    if (!self->constant_view || !self->constant_view(self->real, desc, target))
        self->real->CreateConstantBufferView(desc, target);
    local_write_through(self, dest, 1);
}

void proxy_create_srv(DeviceProxy* self, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(18);
    self->real->CreateShaderResourceView(resource, desc, translate_cpu_handle(self, dest));
    local_write_through(self, dest, 1);
}

void proxy_create_uav(DeviceProxy* self, ID3D12Resource* resource, ID3D12Resource* counter,
                      const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(19);
    self->real->CreateUnorderedAccessView(resource, counter, desc, translate_cpu_handle(self, dest));
    local_write_through(self, dest, 1);
}

void proxy_create_rtv(DeviceProxy* self, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(20);
    const D3D12_CPU_DESCRIPTOR_HANDLE real_dest = translate_cpu_handle(self, dest);
    self->real->CreateRenderTargetView(resource, desc, real_dest);
    mark_null_rtv(self, real_dest.ptr, resource == nullptr);  // for console list slot 52 (DeviceProxy::null_rtvs)
}

void proxy_create_dsv(DeviceProxy* self, ID3D12Resource* resource, const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(21);
    self->real->CreateDepthStencilView(resource, desc, translate_cpu_handle(self, dest));
}

void proxy_create_sampler(DeviceProxy* self, const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE dest) {
    note_slot(22);
    self->real->CreateSampler(desc, translate_cpu_handle(self, dest));
    local_write_through(self, dest, 1);
}

void proxy_copy_descriptors(DeviceProxy* self, UINT num_dest, const D3D12_CPU_DESCRIPTOR_HANDLE* dest_starts,
                            const UINT* dest_sizes, UINT num_src, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts,
                            const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    note_slot(23);
    if (g_custom_range_count.load(std::memory_order_acquire) == 0) {
        self->real->CopyDescriptors(num_dest, dest_starts, dest_sizes, num_src, src_starts, src_sizes, type);
        copy_null_rtv_marks(self, num_dest, dest_starts, dest_sizes, num_src, src_starts, src_sizes, type);
        return;
    }
    constexpr UINT kInline = 32;
    D3D12_CPU_DESCRIPTOR_HANDLE dest_inline[kInline];
    D3D12_CPU_DESCRIPTOR_HANDLE src_inline[kInline];
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> dest_heap;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> src_heap;
    const D3D12_CPU_DESCRIPTOR_HANDLE* dest_out = dest_starts;
    const D3D12_CPU_DESCRIPTOR_HANDLE* src_out = src_starts;
    try {
        if (dest_starts && num_dest > 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE* dst = dest_inline;
            if (num_dest > kInline) {
                dest_heap.resize(num_dest);
                dst = dest_heap.data();
            }
            for (UINT i = 0; i < num_dest; ++i) dst[i] = translate_cpu_handle(self, dest_starts[i]);
            dest_out = dst;
        }
        if (src_starts && num_src > 0) {
            D3D12_CPU_DESCRIPTOR_HANDLE* src = src_inline;
            if (num_src > kInline) {
                src_heap.resize(num_src);
                src = src_heap.data();
            }
            for (UINT i = 0; i < num_src; ++i) src[i] = translate_cpu_handle(self, src_starts[i]);
            src_out = src;
        }
    } catch (const std::bad_alloc&) {
        return;
    }
    self->real->CopyDescriptors(num_dest, dest_out, dest_sizes, num_src, src_out, src_sizes, type);
    for (UINT i = 0; dest_starts && i < num_dest; ++i)
        local_write_through(self, dest_starts[i], dest_sizes ? dest_sizes[i] : 1);
    copy_null_rtv_marks(self, num_dest, dest_out, dest_sizes, num_src, src_out, src_sizes, type);
}

void proxy_copy_descriptors_simple(DeviceProxy* self, UINT num_descriptors, D3D12_CPU_DESCRIPTOR_HANDLE dest_start,
                                   D3D12_CPU_DESCRIPTOR_HANDLE src_start, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    note_slot(24);
    const D3D12_CPU_DESCRIPTOR_HANDLE dest = translate_cpu_handle(self, dest_start);
    const D3D12_CPU_DESCRIPTOR_HANDLE src = translate_cpu_handle(self, src_start);
    self->real->CopyDescriptorsSimple(num_descriptors, dest, src, type);
    local_write_through(self, dest_start, num_descriptors);
    copy_null_rtv_marks(self, 1, &dest, &num_descriptors, 1, &src, &num_descriptors, type);
    if (leonida::fsdraw::enabled()) {  // LOCAL DIAGNOSTIC (fsdraw): where the title copies its descriptors, and what lands there
        static std::atomic<std::uint64_t> copies{0};
        const std::uint64_t n = ++copies;
        if (n <= 24 || n % 600 == 0 || n % 600 == 1) {
            std::uint32_t w[8]{};
            if (dest.ptr && num_descriptors && accessible_range(reinterpret_cast<const void*>(dest.ptr), sizeof w, false))
                std::memcpy(w, reinterpret_cast<const void*>(dest.ptr), sizeof w);  // a handle need not be CPU memory (mocks)
            std::fprintf(stderr, "[rt] fsdraw: CopyDescriptorsSimple #%llu n %u type %d dest 0x%llx -> 0x%llx src 0x%llx -> "
                                 "0x%llx first slot now [%08x %08x %08x %08x %08x %08x %08x %08x]\n",
                         static_cast<unsigned long long>(n), num_descriptors, int(type),
                         static_cast<unsigned long long>(dest_start.ptr), static_cast<unsigned long long>(dest.ptr),
                         static_cast<unsigned long long>(src_start.ptr), static_cast<unsigned long long>(src.ptr), w[0],
                         w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
}

// The Xbox slot 62 takes two more words between the description and the IID: (desc, w2, w3, riid, out). Measured
// on the package's XboxBvhBuilder.dll (its constructor passes w2 = w3 = 0 and IID_ID3D12PipelineState in the fourth
// argument), as the prior art had recorded. Their meaning is unknown; non-zero values are logged once.
HRESULT proxy_create_compute_pipeline_state(DeviceProxy* self, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
                                            std::uint64_t w2, std::uint64_t w3, const IID* riid, void** out) {
    note_slot(62);
    if (w2 || w3) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 62 extra words 0x%llx 0x%llx (ignored)\n",
                         static_cast<unsigned long long>(w2), static_cast<unsigned long long>(w3));
    }
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!riid) return E_POINTER;
    return self->real->CreateComputePipelineState(desc, *riid, out);
}

// The Xbox slot 41 is CreateCommandSignature at its PC position, (desc, root signature, riid, out), but a console
// description can hold an argument type the PC does not have. The retail title creates five one-argument signatures
// with no root signature at startup; one has type 105 (0x69) and a 12-byte stride, next to a DISPATCH and a
// DISPATCH_MESH of 12 bytes. Read statically from its call sites (counts and shapes only): its two ExecuteIndirect
// sites pass MaxCommandCount 1 and an argument offset into the same upload buffer as the DISPATCH ones, and set the
// compute state (pipeline, compute root signature, tables, constants, SRV/UAV) right before, as the DISPATCH path
// does; they record on a second command list of the title's context. So type 105 is taken as a dispatch whose
// arguments are a D3D12_DISPATCH_ARGUMENTS. HYPOTHESIS: what the console does differently for it (which command
// stream it feeds) is not known; the PC layer has one compute path. Other descriptions are forwarded unchanged.
// 105 is outside D3D12_INDIRECT_ARGUMENT_TYPE's range, so the field is read as its 32 bits (a constant expression that
// casts 105 to the enum does not compile with clang-cl).
constexpr std::uint32_t kXboxIndirectArgumentDispatch105 = 105;

std::uint32_t argument_type(const D3D12_INDIRECT_ARGUMENT_DESC& arg) {
    std::uint32_t type = 0;
    std::memcpy(&type, &arg.Type, sizeof(type));
    return type;
}

HRESULT proxy_create_command_signature(DeviceProxy* self, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                       std::uint64_t root_signature, const IID* riid, void** out) {
    note_slot(41);
    using Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_SIGNATURE_DESC*, std::uint64_t,
                                           const IID*, void**);
    const auto forward = reinterpret_cast<Fn>(self->real_vtable[41]);
    bool console = false;
    if (desc && desc->pArgumentDescs)
        for (UINT i = 0; i < desc->NumArgumentDescs && !console; ++i)
            console = argument_type(desc->pArgumentDescs[i]) == kXboxIndirectArgumentDispatch105;
    if (!console) return forward(self->real, desc, root_signature, riid, out);

    std::unique_ptr<D3D12_INDIRECT_ARGUMENT_DESC[]> args(new (std::nothrow)
                                                             D3D12_INDIRECT_ARGUMENT_DESC[desc->NumArgumentDescs]);
    if (!args) {
        if (out) *out = nullptr;
        return E_OUTOFMEMORY;
    }
    for (UINT i = 0; i < desc->NumArgumentDescs; ++i) {
        args[i] = desc->pArgumentDescs[i];
        if (argument_type(args[i]) == kXboxIndirectArgumentDispatch105)
            args[i].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: slot 41 argument type 105 (stride %u) created as DISPATCH\n",
                     desc->ByteStride);
    D3D12_COMMAND_SIGNATURE_DESC pc = *desc;
    pc.pArgumentDescs = args.get();
    return forward(self->real, &pc, root_signature, riid, out);
}

// The Xbox slot 61 is CreateSamplerX(desc, dest): a sampler written to a CPU descriptor handle, with the PC
// CreateSampler's two arguments. Measured on the retail title's boot (stop 20 in #6910's RT-5 measurements): the
// title calls it once per sampler after its descriptor heaps exist, and it goes on when the call is served by the PC
// CreateSampler. The PC slot 61 is CreateMetaCommand, which would take these words as (CommandId, NodeMask, ...).
// HYPOTHESIS: the console description begins with the PC D3D12_SAMPLER_DESC (52 bytes), which is what got the title
// past this call; whatever follows it is not known and not read. The title was not seen testing a result, so a null
// description is skipped rather than reported. A handle in the title's own heap range (#7021) is translated, as
// slot 22 does.
// The same proxy also serves host callers (the runtime's own tests drive the Vulkan device through it), for whom slot
// 61 is the PC ID3D12Device5::CreateMetaCommand. So the slot is CreateSamplerX only for a caller in an image the
// guest loader mapped (set_console_caller); any other caller, or every caller when no check is installed, gets the PC
// method. Without the check d3d12_vulkan_device_test's CreateMetaCommand call reached this handler (#7115).
std::atomic<ConsoleCaller> g_console_caller{nullptr};

bool is_console_caller(const void* address) {
    const ConsoleCaller classify = g_console_caller.load(std::memory_order_acquire);
    return classify && classify(reinterpret_cast<std::uint64_t>(address));
}

void proxy_create_sampler_x(DeviceProxy* self, const D3D12_SAMPLER_DESC* desc, std::uint64_t dest) {
    note_slot(61);
    if (!desc) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: slot 61 CreateSamplerX without a description (skipped)\n");
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE handle{};
    // A destination in the title's own descriptor range is translated to the real heap's, as device slots 17 to 24 do.
    handle = translate_cpu_handle(self, D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(dest)});
    self->real->CreateSampler(desc, handle);
}

// The Xbox slot 67 creates a compute pipeline from native GPU code: (desc, riid, out), named
// DeserializeComputePipelineStateX in amd_fidelityfx_upscaler_xbox.dll's messages (call at RVA 0x56d9). The desc
// passes the root signature at +0x00, followed by pointer/size pairs for the shader's FSPX entries (docs/fspx.md):
// kind 0 (dispatch state) at +0x08/+0x10, kind 1 at +0x18/+0x20, kind 6 (the RDNA2 program) at +0x28/+0x30, and
// kind 7 (records) at +0x38/+0x40. Translates the console description to a standard D3D12_COMPUTE_PIPELINE_STATE_DESC
// with CS bytecode pointing at kind 6 and routes it to CreateComputePipelineState on the PC device (slot 11), which
// on the Vulkan device resolves through the translator. The PC slot 67 is CreateProtectedResourceSession1.
HRESULT proxy_create_compute_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                       const IID* riid, void** out) {
    note_slot(67);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    // FSPX description layout (docs/fspx.md §How the DLL reads it):
    struct XboxNativeComputePipelineStateDesc {
        ID3D12RootSignature* root_signature;  // +0x00
        const void* kind0;                    // +0x08: dispatch state
        std::uint64_t kind0_size;             // +0x10: 28 bytes
        const void* kind1;                    // +0x18: unknown
        std::uint64_t kind1_size;             // +0x20: 20 bytes
        const void* kind6;                    // +0x28: RDNA2 program code
        std::uint64_t kind6_size;             // +0x30: program size in bytes
        const void* kind7;                    // +0x38: resource/hash records
        std::uint64_t kind7_size;             // +0x40: records size in bytes
    };
    const auto* xbox_desc = static_cast<const XboxNativeComputePipelineStateDesc*>(desc);

    D3D12_COMPUTE_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.CS.pShaderBytecode = xbox_desc->kind6;
    pc_desc.CS.BytecodeLength = static_cast<SIZE_T>(xbox_desc->kind6_size);
    // With kind 0 (the DLL always passes it: its reader refuses a payload without one), the four pairs go on as one
    // FSPX payload, so the device matches the program by its kind-6 and kind-0 keys (docs/fspx.md, "Build and run-time
    // lookup"): two programs with the same code and different dispatch state stay apart. Every pair is checked before
    // it is read (leonida::fspx_payload_from_parts); an inconsistent one refuses the call. Without kind 0 the kind-6
    // bytes go on alone, as before.
    std::vector<std::uint8_t> payload;
    if (xbox_desc->kind0 || xbox_desc->kind0_size) {
        const leonida::FspxPart parts[] = {{0, xbox_desc->kind0, xbox_desc->kind0_size},
                                           {1, xbox_desc->kind1, xbox_desc->kind1_size},
                                           {6, xbox_desc->kind6, xbox_desc->kind6_size},
                                           {7, xbox_desc->kind7, xbox_desc->kind7_size}};
        payload = leonida::fspx_payload_from_parts(parts);
        if (payload.empty()) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true, std::memory_order_relaxed))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 67 refused: a pointer/size pair is inconsistent or too large "
                             "(kind0 %llu B, kind1 %llu B, kind6 %llu B, kind7 %llu B)\n",
                             static_cast<unsigned long long>(xbox_desc->kind0_size),
                             static_cast<unsigned long long>(xbox_desc->kind1_size),
                             static_cast<unsigned long long>(xbox_desc->kind6_size),
                             static_cast<unsigned long long>(xbox_desc->kind7_size));
            return E_INVALIDARG;
        }
        pc_desc.CS.pShaderBytecode = payload.data();
        pc_desc.CS.BytecodeLength = payload.size();
    }

    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    // Every failed call is logged with its number: the DLL creates its pipelines one after the other, and only the
    // first call of a slot is announced, so a miss on a later program would otherwise read as the first call's.
    static std::atomic<std::uint64_t> calls{0};
    const std::uint64_t call = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const HRESULT hr = self->real->CreateComputePipelineState(&pc_desc, *riid, out);
    if (FAILED(hr))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 67 call %llu failed with 0x%08lx (kind 0 %llu B, kind 1 %llu B, kind 6 %llu B, "
                     "kind 7 %llu B)\n",
                     static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                     static_cast<unsigned long long>(xbox_desc->kind0_size),
                     static_cast<unsigned long long>(xbox_desc->kind1_size),
                     static_cast<unsigned long long>(xbox_desc->kind6_size),
                     static_cast<unsigned long long>(xbox_desc->kind7_size));
    else if (d3d12_trace::enabled())
        std::fprintf(stderr, "[d3d12-trace] slot 67 call %llu ok\n", static_cast<unsigned long long>(call));
    return hr;
}

// ---- Committed resources take their memory through the title's XMem hooks (RT-5 stop 17) ----
// On the console the D3D runtime allocates a committed resource's memory with XMemAlloc, so the allocation goes
// through the title's __XMemAllocationHooks__. Around its first full-screen render targets (slot 127) the retail title
// registers a per-thread matcher that bump-allocates out of a range it reserved, taking any allocation whose XALLOC
// page-size field (bits 14..15) is 2, and afterwards asserts that less than 2 MiB of the range is left. Resources
// created on the PC never allocated through the hooks, so the whole range was left and the assert fired. Each committed
// resource made by slots 76 and 127 therefore allocates its size through the hooks as a lease: the resource holds it
// (SetPrivateDataInterface) and the hooks' free runs when the resource is destroyed. The bytes are not used for the
// resource; the PC device owns its memory. The size is the console's (lease_size): for a render target of a listed
// shape, its whole SW_64KB_D tiles (console_render_target_size), never the PC size. The title's budget is sized for
// exactly those (two 3840 x 2160 RGBA8 targets are 2 x 510 tiles of a 64 MiB range), and a PC driver may answer more:
// AMD's Windows driver gives 513 x 64 KiB for that target, and leasing that overran the range (an int3 in the title's
// hook, @controldebils on #7240). Anything else leases the PC size.
// HYPOTHESIS: the attribute word, allocator ID 0x80 (eXALLOCAllocatorId_D3D) with page-size field 2 and object type 0.
// Of it the measured matcher reads only the page-size field; a title hook that matches on other fields may not take
// these leases for the console's allocations.
inline constexpr std::uint64_t kXmemCommittedResourceAttributes = (0x80ULL << 16) | (2ULL << 14);

// {6c1f3a52-8d47-4e0b-9b1e-2f7a5c3d9e61}: the private-data entry a committed resource keeps its lease under.
constexpr GUID kTitleMemoryLeaseGuid{0x6c1f3a52, 0x8d47, 0x4e0b, {0x9b, 0x1e, 0x2f, 0x7a, 0x5c, 0x3d, 0x9e, 0x61}};

// The console allocation for one clean description: a listed render-target shape's whole tiles, else the PC size.
UINT64 lease_size(ID3D12Device* real, const D3D12_RESOURCE_DESC& clean_desc) {
    if (const UINT64 tiles = console_render_target_size(clean_desc)) return tiles;
    return real->GetResourceAllocationInfo(0, 1, &clean_desc).SizeInBytes;
}

// One reference is held by the resource; the last Release gives the memory back through the hooks' free.
class TitleMemoryLease final : public IUnknown {
public:
    TitleMemoryLease(void* memory, DescriptorHeapBackingFree free_fn) : memory_(memory), free_(free_fn) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (!IsEqualIID(riid, __uuidof(IUnknown))) {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *out = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (left == 0) {
            if (free_) free_(memory_, kXmemCommittedResourceAttributes);
            delete this;
        }
        return left;
    }

private:
    std::atomic<ULONG> refs_{1};
    void* memory_;
    DescriptorHeapBackingFree free_;
};

// `created` is the object a slot 76/127 creation returned (any interface of the resource). Nothing happens without
// installed hooks, for a size the PC device refuses, or when the hooks return null; creation never fails here.
void lease_title_resource_memory(DeviceProxy* self, void* created, const D3D12_RESOURCE_DESC& clean_desc, UINT layout) {
    if (!created) return;
    DescriptorHeapBackingAlloc alloc_fn = nullptr;
    DescriptorHeapBackingFree free_fn = nullptr;
    {
        std::lock_guard lock(heap_alloc_mutex());
        alloc_fn = g_heap_alloc;
        free_fn = g_heap_free;
    }
    if (!alloc_fn) return;
    const UINT64 size = lease_size(self->real, clean_desc);
    if (size == 0 || size == UINT64_MAX || size > (std::numeric_limits<std::size_t>::max)()) return;
    void* const memory = alloc_fn(static_cast<std::size_t>(size), kXmemCommittedResourceAttributes);
    if (!memory) return;
    ID3D12Object* object = nullptr;
    auto* const lease = new (std::nothrow) TitleMemoryLease(memory, free_fn);
    if (!lease || FAILED(static_cast<IUnknown*>(created)->QueryInterface(__uuidof(ID3D12Object),
                                                                         reinterpret_cast<void**>(&object)))) {
        if (lease)
            lease->Release();  // frees the memory
        else if (free_fn)
            free_fn(memory, kXmemCommittedResourceAttributes);
        return;
    }
    const HRESULT kept = object->SetPrivateDataInterface(kTitleMemoryLeaseGuid, lease);  // the resource's reference
    if (SUCCEEDED(kept) && self->texture_lease_provider && clean_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        self->texture_lease_provider(static_cast<IUnknown*>(created), memory, static_cast<std::size_t>(size), layout);
    object->Release();
    lease->Release();  // ours; from here the resource's reference is the only one
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: committed resources lease their size through the title's XMem hooks (first: "
                     "%llu B)\n",
                     static_cast<unsigned long long>(size));
}

// The console creates its render-target and depth textures with initial state 0x6; the PC runtime refuses that state
// for a texture (reported by rafaelpssz from a retail exe integration run). A texture whose flags name the use gets
// that use's state: ALLOW_RENDER_TARGET -> RENDER_TARGET, ALLOW_DEPTH_STENCIL -> DEPTH_WRITE. Every other state and
// every buffer goes on unchanged. The description is only read; the title's words are never written.
constexpr std::uint32_t kConsoleTextureInitialState = 0x6;

std::uint64_t pc_initial_state_word(std::uint64_t desc_word, std::uint64_t state_word) {
    if (static_cast<std::uint32_t>(state_word) != kConsoleTextureInitialState || !desc_word) return state_word;
    const auto* desc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc_word);
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return state_word;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) return D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return state_word;
}

D3D12_RESOURCE_STATES pc_initial_state(const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES state) {
    return static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(
        pc_initial_state_word(reinterpret_cast<std::uint64_t>(desc), static_cast<std::uint32_t>(state))));
}

// Device slots 27 CreateCommittedResource(props, heap_flags, desc, state, clear, riid, out) and 53
// CreateCommittedResource1 (a protected session before riid): the PC call with the initial state mapped above. The PC
// method is called by its table entry with the caller's words, so a null riid or output reaches the real device as it did.
template <unsigned Slot>
std::uint64_t proxy_create_committed_resource(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                              std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                              std::uint64_t a8) {
    const std::uint64_t state = pc_initial_state_word(a3, a4);
    if constexpr (Slot == 27) {
        using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t, std::uint64_t);
        return reinterpret_cast<Fn>(self->real_vtable[27])(self->real, a1, a2, a3, state, a5, a6, a7);
    } else {
        using Fn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                          std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
        return reinterpret_cast<Fn>(self->real_vtable[53])(self->real, a1, a2, a3, state, a5, a6, a7, a8);
    }
}

// Xbox slot 57 through the factory of the device behind the proxy (set_device_native_graphics_factory), which
// takes the whole description. E_NOTIMPL (no factory, or one that cannot build this description) sends the call
// to proxy_create_graphics_pipeline_from_native_code below.
HRESULT proxy_create_native_graphics(DeviceProxy* self, const void* raw, REFIID riid, void** out) {
    namespace np = leonida::native_pso;
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!self->native_graphics) return E_NOTIMPL;
    if (!accessible_range(raw, sizeof(np::GraphicsDesc), false)) return E_INVALIDARG;
    np::GraphicsDesc desc{};
    std::memcpy(&desc, raw, sizeof(desc));
    const auto readable_pair = [](const np::Pair& pair) {
        const auto state = np::pair_state(pair);
        return state == np::PairState::absent ||
               (state == np::PairState::present && accessible_range(pair.data, static_cast<std::size_t>(pair.size), false));
    };
    if (!readable_pair(desc.state_b)) return E_INVALIDARG;
    for (const auto& stage : desc.stages)
        if (!readable_pair(stage.records)) return E_INVALIDARG;
    return self->native_graphics(self->real, desc, riid, out);
}

// Xbox slot 57 (+0x1c8): DeserializeGraphicsPipelineStateX(const XboxNativeGraphicsPipelineStateDesc* desc,
// REFIID riid, void** ppPipelineState).
struct XboxNativeGraphicsPipelineStateDesc {
    ID3D12RootSignature* root_signature;  // +0x00
    const void* state_blob;               // +0x08: 380..404 B state blob (magic 0x4005d800 / 0x4005d808 / 0x4005d810)
    std::uint64_t state_blob_size;        // +0x10
    const void* pipeline_blob;            // +0x18: 80..224 B pipeline / input-layout blob (188 B in boot PSOs 1..3)
    std::uint64_t pipeline_blob_size;     // +0x20
    const void* prog_a;                   // +0x28: native RDNA2 program #1 (or nullptr/0 if absent, as in boot PSO 3)
    std::uint64_t prog_a_size;            // +0x30
    const void* stage_a;                  // +0x38: stage blob #1 (kind-2, e.g. 208 B, or nullptr/0 if absent)
    std::uint64_t stage_a_size;           // +0x40
    const void* prog_b;                   // +0x48: native RDNA2 program #2 (e.g. 320..384 B)
    std::uint64_t prog_b_size;            // +0x50
    const void* stage_b;                  // +0x58: stage blob #2 (kind-2, e.g. 752 B)
    std::uint64_t stage_b_size;           // +0x60
};

// Measured offsets in the 380..388 B state blob across the 24,797 graphics records of the pipeline library.
// None is confirmed as a specific D3D12 state field yet; each offset is logged once with its observed u32 word.
struct MeasuredStateBlobField {
    std::uint32_t offset;
    const char* note;
};

constexpr MeasuredStateBlobField kMeasuredStateBlobFields[] = {
    {0x000, "version/type tag (0x4005d800 / 0x4005d808 / 0x4005d810)"},
    {0x004, "PSO family (4 or 6)"},
    {0x00c, "per-PSO state word"},
    {0x028, "per-PSO state word"},
    {0x02c, "constant 2"},
    {0x038, "per-PSO state word"},
    {0x03c, "small counter"},
    {0x040, "mask (0xfffe or 0)"},
    {0x044, "mask (0xfffffffe or 0xffff0000)"},
    {0x04c, "boot-PSO diff word"},
    {0x050, "small counter"},
    {0x054, "boot-PSO diff word"},
    {0x058, "boot-PSO diff word"},
    {0x05c, "boot-PSO diff word"},
    {0x064, "per-PSO state word"},
    {0x068, "boot-PSO diff word"},
    {0x06c, "boot-PSO diff word"},
    {0x070, "boot-PSO diff word"},
    {0x074, "per-PSO state word (0x060201bc or 0x80)"},
    {0x07c, "per-PSO state word (0xcc0010 / 0 / 0xcc0011)"},
    {0x084, "boot-PSO diff word"},
    {0x0a0, "per-PSO state word"},
    {0x0a8, "per-PSO state word (0x1000 or 0x8004)"},
    {0x0b8, "boot-PSO diff word"},
    {0x0f0, "per-PSO state word"},
    {0x0f4, "constant 5"},
    {0x0f8, "constant 1"},
    {0x0fc, "0x33333333 (float 0.1)"},
    {0x100, "repeated 0x00060006 slot 0"},
    {0x104, "repeated 0x00060006 slot 1"},
    {0x108, "repeated 0x00060006 slot 2"},
    {0x10c, "repeated 0x00060006 slot 3"},
    {0x110, "repeated 0x00060006 slot 4"},
    {0x114, "repeated 0x00060006 slot 5"},
    {0x118, "repeated 0x00060006 slot 6"},
    {0x11c, "repeated 0x00060006 slot 7"},
    {0x128, "boot-PSO diff word"},
    {0x144, "mask (0 or 0xffffffff)"},
    {0x150, "constant 0x40"},
    {0x158, "constant 0xffffffff"},
    {0x174, "per-PSO state word"},
    {0x17c, "constant 0x10000a00 (present when size >= 384)"},
};

std::atomic<bool> g_state_blob_field_logged[std::size(kMeasuredStateBlobFields)]{};

void log_measured_state_blob_fields(const void* blob, std::size_t blob_size) {
    if (!blob || blob_size < 4) return;
    const auto* bytes = static_cast<const std::uint8_t*>(blob);
    for (std::size_t i = 0; i < std::size(kMeasuredStateBlobFields); ++i) {
        const std::uint32_t off = kMeasuredStateBlobFields[i].offset;
        if (static_cast<std::size_t>(off) + 4 > blob_size) continue;
        bool expected = false;
        if (g_state_blob_field_logged[i].compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            std::uint32_t val = 0;
            std::memcpy(&val, bytes + off, 4);
            std::fprintf(stderr, "[rt] d3d12_xs: state blob: unidentified field at 0x%03x = 0x%08x\n",
                         static_cast<unsigned>(off), static_cast<unsigned>(val));
        }
    }
}

bool parse_xbox_pipeline_blob_input_layout_impl(const void* blob, std::size_t blob_size,
                                                std::vector<XboxPipelineInputElement>& out_elements) {
    out_elements.clear();
    if (!blob || blob_size < 0x54) return false;
    const auto* bytes = static_cast<const std::uint8_t*>(blob);
    const auto read_u32 = [&](std::size_t off, std::uint32_t& v) -> bool {
        if (off + 4 > blob_size) return false;
        std::memcpy(&v, bytes + off, 4);
        return true;
    };
    std::uint32_t magic_const = 0;
    std::uint32_t field_block_size = 0;
    std::uint32_t tag_3c = 0;
    std::uint32_t num_elements = 0;
    if (!read_u32(0x04, magic_const) || !read_u32(0x10, field_block_size) ||
        !read_u32(0x3c, tag_3c) || !read_u32(0x4c, num_elements))
        return false;
    if (num_elements == 0 || num_elements > D3D12_IA_VERTEX_INPUT_STRUCTURE_ELEMENT_COUNT) return true;
    // Measured at +0x10: 0x1c (28 bytes = 7 u32) in PSOs 1 and 2, 0 in PSO 3 where the element stride is still 28 B.
    const std::size_t stride_after_name = (field_block_size >= 16 && field_block_size <= 64) ? field_block_size : 28u;
    std::size_t pos = 0x54;
    for (std::uint32_t idx = 0; idx < num_elements && pos + 4 <= blob_size; ++idx) {
        const void* nul = std::memchr(bytes + pos, 0, blob_size - pos);
        if (!nul) break;
        const std::size_t name_len = static_cast<const std::uint8_t*>(nul) - (bytes + pos);
        if (name_len == 0) break;
        const std::size_t fields_pos = pos + name_len + 1;
        // Need at least the first 4 u32s (+0x00 slot, +0x04 DXGI_FORMAT, +0x08 SemanticIndex, +0x0c AlignedByteOffset)
        // inside blob_size; never read past blob_size even when the last 1..4 bytes of the 28-byte block are truncated.
        if (fields_pos + 16 > blob_size) break;
        std::uint32_t slot = 0, fmt = 0, sem_idx = 0, byte_off = 0;
        read_u32(fields_pos + 0x00, slot);
        read_u32(fields_pos + 0x04, fmt);
        read_u32(fields_pos + 0x08, sem_idx);
        read_u32(fields_pos + 0x0c, byte_off);
        XboxPipelineInputElement elem;
        elem.semantic_name.assign(reinterpret_cast<const char*>(bytes + pos), name_len);
        elem.semantic_index = sem_idx;
        elem.format = static_cast<DXGI_FORMAT>(fmt);
        elem.input_slot = slot < D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT ? slot : 0;
        elem.aligned_byte_offset = byte_off;
        out_elements.push_back(std::move(elem));
        pos = fields_pos + stride_after_name;
    }
    return true;
}

SRWLOCK g_fspx_stage_lock = SRWLOCK_INIT;
leonida::ShaderLibrary* g_fspx_stage_lib = nullptr;
std::wstring* g_fspx_stage_dir = nullptr;

std::wstring current_shaders_env_dir() {
    wchar_t buf[32768]{};
    const DWORD n = GetEnvironmentVariableW(L"LEONIDA_SHADERS", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return {};
    return std::wstring(buf, n);
}

std::string lookup_xbox_fspx_program_stage_impl(const void* program_bytes, std::size_t program_size) {
    if (!program_bytes || !program_size || program_size > leonida::kFspxPartLimit) return {};
    const std::span<const std::uint8_t> code{static_cast<const std::uint8_t*>(program_bytes), program_size};
    if (!leonida::fspx_bare_program(code)) return {};
    const std::string hash = leonida::sha256_hex(code);
    const std::wstring dir = current_shaders_env_dir();
    if (dir.empty()) return {};
    AcquireSRWLockExclusive(&g_fspx_stage_lock);
    if (!g_fspx_stage_lib) g_fspx_stage_lib = new (std::nothrow) leonida::ShaderLibrary();
    if (!g_fspx_stage_dir) g_fspx_stage_dir = new (std::nothrow) std::wstring();
    if (g_fspx_stage_lib && g_fspx_stage_dir && *g_fspx_stage_dir != dir) {
        *g_fspx_stage_dir = dir;
        g_fspx_stage_lib->load_fspx_only(std::filesystem::path(dir));
    }
    const leonida::ShaderEntry* entry = g_fspx_stage_lib ? g_fspx_stage_lib->find_fspx_by_code(hash) : nullptr;
    const std::string stage = entry ? entry->stage : std::string{};
    ReleaseSRWLockExclusive(&g_fspx_stage_lock);
    return stage;
}

std::atomic<std::uint64_t> g_slot57_calls{0};

HRESULT proxy_create_graphics_pipeline_from_native_code(DeviceProxy* self, const void* desc,
                                                        const IID* riid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!desc) return E_INVALIDARG;
    if (!riid) return E_POINTER;

    const auto* xbox_desc = static_cast<const XboxNativeGraphicsPipelineStateDesc*>(desc);
    const std::uint64_t call = g_slot57_calls.fetch_add(1, std::memory_order_relaxed) + 1;

    // 1. Inspect and log measured state-blob fields once per offset (no unmeasured offsets).
    if (xbox_desc->state_blob && xbox_desc->state_blob_size) {
        if (xbox_desc->state_blob_size > leonida::kFspxPartLimit) return E_INVALIDARG;
        log_measured_state_blob_fields(xbox_desc->state_blob, static_cast<std::size_t>(xbox_desc->state_blob_size));
    }

    // 2. Defensively decode input layout from the pipeline blob (+0x18 / +0x20).
    std::vector<XboxPipelineInputElement> parsed_inputs;
    if (xbox_desc->pipeline_blob && xbox_desc->pipeline_blob_size) {
        if (xbox_desc->pipeline_blob_size > leonida::kFspxPartLimit) return E_INVALIDARG;
        parse_xbox_pipeline_blob_input_layout_impl(
            xbox_desc->pipeline_blob, static_cast<std::size_t>(xbox_desc->pipeline_blob_size), parsed_inputs);
    }
    std::vector<D3D12_INPUT_ELEMENT_DESC> input_descs;
    input_descs.reserve(parsed_inputs.size());
    for (const auto& elem : parsed_inputs) {
        D3D12_INPUT_ELEMENT_DESC d{};
        d.SemanticName = elem.semantic_name.c_str();
        d.SemanticIndex = elem.semantic_index;
        d.Format = elem.format;
        d.InputSlot = elem.input_slot;
        d.AlignedByteOffset = elem.aligned_byte_offset;
        d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        d.InstanceDataStepRate = 0;
        input_descs.push_back(d);
    }

    // 3. Resolve each present RDNA2 program (+0x28/+0x30 and +0x48/+0x50) by sha256(program bytes) and assign its
    //    stage from the index entry ("vs", "ps", "ms", "gs"), never from its position in the description.
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pc_desc{};
    pc_desc.pRootSignature = xbox_desc->root_signature;
    pc_desc.InputLayout.pInputElementDescs = input_descs.empty() ? nullptr : input_descs.data();
    pc_desc.InputLayout.NumElements = static_cast<UINT>(input_descs.size());

    struct ProgPair {
        const void* data;
        std::uint64_t size;
    };
    const ProgPair pairs[2] = {{xbox_desc->prog_a, xbox_desc->prog_a_size},
                               {xbox_desc->prog_b, xbox_desc->prog_b_size}};
    for (const ProgPair& p : pairs) {
        if (!p.data && !p.size) continue;
        if (!p.data || !p.size || p.size > leonida::kFspxPartLimit) return E_INVALIDARG;
        const std::span<const std::uint8_t> code{static_cast<const std::uint8_t*>(p.data),
                                                 static_cast<std::size_t>(p.size)};
        if (!leonida::fspx_bare_program(code)) {
            static std::atomic<int> bad_prog_log{0};
            if (bad_prog_log.fetch_add(1, std::memory_order_relaxed) < 8)
                std::fprintf(stderr, "[rt] d3d12_xs: slot 57 call %llu: non-RDNA2 program blob (%llu B)\n",
                             static_cast<unsigned long long>(call), static_cast<unsigned long long>(p.size));
            return E_INVALIDARG;
        }
        const std::string stage = lookup_xbox_fspx_program_stage_impl(p.data, static_cast<std::size_t>(p.size));
        if (stage == "vs") {
            pc_desc.VS = {p.data, static_cast<SIZE_T>(p.size)};
        } else if (stage == "ps") {
            pc_desc.PS = {p.data, static_cast<SIZE_T>(p.size)};
        } else if (stage == "gs") {
            pc_desc.GS = {p.data, static_cast<SIZE_T>(p.size)};
        } else {
            static std::atomic<int> miss_log{0};
            if (miss_log.fetch_add(1, std::memory_order_relaxed) < 16) {
                const std::string hash = leonida::sha256_hex(code);
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 57 call %llu: no translated graphics stage in fspx_gfx for kind-6 "
                             "sha256 %s (%llu B)\n",
                             static_cast<unsigned long long>(call), hash.c_str(),
                             static_cast<unsigned long long>(p.size));
            }
            return E_INVALIDARG;
        }
    }

    // 4. Safe default fixed-function state (blend disabled, depth test on/LESS, cull none, triangle list, 1x sample).
    pc_desc.BlendState.AlphaToCoverageEnable = FALSE;
    pc_desc.BlendState.IndependentBlendEnable = FALSE;
    for (auto& rt : pc_desc.BlendState.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    pc_desc.SampleMask = UINT_MAX;
    pc_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pc_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pc_desc.RasterizerState.FrontCounterClockwise = FALSE;
    pc_desc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    pc_desc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    pc_desc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    pc_desc.RasterizerState.DepthClipEnable = TRUE;
    pc_desc.RasterizerState.MultisampleEnable = FALSE;
    pc_desc.RasterizerState.AntialiasedLineEnable = FALSE;
    pc_desc.RasterizerState.ForcedSampleCount = 0;
    pc_desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    pc_desc.DepthStencilState.DepthEnable = TRUE;
    pc_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pc_desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pc_desc.DepthStencilState.StencilEnable = FALSE;
    pc_desc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    pc_desc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC default_stencil_op = {
        D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    pc_desc.DepthStencilState.FrontFace = default_stencil_op;
    pc_desc.DepthStencilState.BackFace = default_stencil_op;

    pc_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    if (pc_desc.PS.pShaderBytecode && pc_desc.PS.BytecodeLength) {
        pc_desc.NumRenderTargets = 1;
        pc_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    } else {
        pc_desc.NumRenderTargets = 0;
    }
    pc_desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pc_desc.SampleDesc.Count = 1;
    pc_desc.SampleDesc.Quality = 0;
    pc_desc.NodeMask = 0;
    pc_desc.CachedPSO = {nullptr, 0};
    pc_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    const HRESULT hr = self->real->CreateGraphicsPipelineState(&pc_desc, *riid, out);
    if (FAILED(hr)) {
        static std::atomic<int> fail_log{0};
        if (fail_log.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 57 call %llu failed with 0x%08lx (state %llu B, pipe %llu B, progA %llu B, "
                         "progB %llu B)\n",
                         static_cast<unsigned long long>(call), static_cast<unsigned long>(hr),
                         static_cast<unsigned long long>(xbox_desc->state_blob_size),
                         static_cast<unsigned long long>(xbox_desc->pipeline_blob_size),
                         static_cast<unsigned long long>(xbox_desc->prog_a_size),
                         static_cast<unsigned long long>(xbox_desc->prog_b_size));
    }
    return hr;
}

bool supports_depth_uav(const DeviceProxy* self, const D3D12_RESOURCE_DESC& desc) {
    return self->depth_stencil_uav_provider && depth_uav_description(desc);
}

D3D12_RESOURCE_DESC depth_image_desc(D3D12_RESOURCE_DESC desc) {
    // Keep desktop resource flags up to RTAS and USE_TIGHT_ALIGNMENT. Xbox stores additional flags above them.
    desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>((UINT(desc.Flags) & 0x5ffu) &
                                                  ~UINT(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS));
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

D3D12_RESOURCE_STATES depth_image_state(D3D12_RESOURCE_STATES requested) {
    return requested == D3D12_RESOURCE_STATE_UNORDERED_ACCESS ? D3D12_RESOURCE_STATE_COMMON : requested;
}

// The depth provider answers E_INVALIDARG or E_NOTIMPL for a texture it has no unordered-access form of
// (Resource::enable_depth_stencil_uav). A console D16S8 target is one on a GPU with D24S8 attachments (every NVIDIA
// card): pc_format() makes it as D24_UNORM_S8_UINT, and the provider takes only R16_TYPELESS, 118 and
// R32G8X24_TYPELESS. Slots 76 and 133 then make the texture without unordered access, as they did before the provider
// (#939), and the title gets its texture instead of asserting. Any other failure is passed on as it is.
bool depth_provider_refusal(HRESULT hr) { return hr == E_INVALIDARG || hr == E_NOTIMPL; }

void note_depth_provider_refusal(std::atomic<bool>& seen, unsigned slot, DXGI_FORMAT format, HRESULT hr) {
    if (first_call(seen))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot %u: the depth provider refused a depth-stencil texture with unordered access "
                     "(format %d, hr=0x%08x); it is made without unordered access\n",
                     slot, static_cast<int>(format), static_cast<unsigned>(hr));
}

// `refused` (optional) is set when the provider refused the texture (depth_provider_refusal), not when the backing
// itself failed.
HRESULT create_depth_uav_backing(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* props, D3D12_HEAP_FLAGS flags,
                                 const D3D12_RESOURCE_DESC& native, D3D12_RESOURCE_STATES initial,
                                 const D3D12_CLEAR_VALUE* clear, ID3D12Resource** out, bool* refused = nullptr) {
    // The logical Xbox texture has a UAV. Its writable planes take ALLOW_SHADER_ATOMICS, which the PC
    // rejects on the depth-only attachment. Upload/readback heaps cannot hold a UAV on either API.
    if (props && (props->Type == D3D12_HEAP_TYPE_UPLOAD || props->Type == D3D12_HEAP_TYPE_READBACK))
        return E_INVALIDARG;
    const auto storage_flags = flags & D3D12_HEAP_FLAG_ALLOW_SHADER_ATOMICS;
    HRESULT hr = self->real->CreateCommittedResource(props, flags & ~storage_flags, &native, initial, clear,
                                                     IID_PPV_ARGS(out));
    if (FAILED(hr)) return hr;
    hr = self->depth_stencil_uav_provider(*out, storage_flags);
    if (FAILED(hr)) {
        (*out)->Release();
        *out = nullptr;
        if (refused) *refused = depth_provider_refusal(hr);
    }
    return hr;
}

HRESULT proxy_create_placed_depth(DeviceProxy* self, UINT64 address, const D3D12_RESOURCE_DESC& desc,
                                  std::uint32_t layout, const D3D12_CLEAR_VALUE* clear, REFIID riid, void** out,
                                  bool* provider_refused) {
    const auto native = depth_image_desc(desc);
    const bool valid = self->validate_depth_placement(native, layout, clear);
    D3D12_RESOURCE_ALLOCATION_INFO alloc{};
    HRESULT hr = E_INVALIDARG;
    if (valid) {
        alloc = self->real->GetResourceAllocationInfo(0, 1, &native);
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        hr = self->placed_depth_textures.create(address, native, alloc, clear, desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN, riid, out, [&](ID3D12Resource** backing) {
            return create_depth_uav_backing(self, &heap, D3D12_HEAP_FLAG_NONE, native, D3D12_RESOURCE_STATE_COMMON,
                                            clear, backing, provider_refused);
        });
    }
    // A refusal here reaches no trace line, and the title stops on an int3 right after it: log the first few. The
    // provider's own refusal is not one of them: slot 133 makes that texture without unordered access and says so.
    static std::atomic<int> refusals{0};
    if (FAILED(hr) && !*provider_refused && refusals.fetch_add(1, std::memory_order_relaxed) < 8)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 depth texture refused (hr=0x%08x, %s): format %d %llux%ux%u, Xbox layout "
                     "0x%x, layout word 0x%x, PC size %llu\n",
                     static_cast<unsigned>(hr), valid ? "placement" : "description", int(desc.Format),
                     static_cast<unsigned long long>(desc.Width), unsigned(desc.Height), unsigned(desc.DepthOrArraySize),
                     unsigned(desc.Layout), layout, static_cast<unsigned long long>(alloc.SizeInBytes));
    return hr;
}

HRESULT proxy_create_resource(DeviceProxy* self, const D3D12_HEAP_PROPERTIES* heap_properties,
                              D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC* xbox_desc,
                              D3D12_RESOURCE_STATES initial_state, const D3D12_CLEAR_VALUE* clear_value,
                              const IID* riid, void** out) {
    note_slot(76);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!xbox_desc) return E_POINTER;
    if (!riid) return E_POINTER;

    // The measured description is 56 bytes plus Xbox extension words; D3D12_RESOURCE_DESC is exactly 56 bytes.
    // The Xbox layout word is 0x101 (tiled) or 0x1 (row-major buffer), and is not a PC layout value. Committed
    // resources take ROW_MAJOR for buffers and UNKNOWN for everything else, so the word is ignored and replaced.
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // A console depth-stencil texture may allow unordered access; the PC one may not (without_depth_stencil_uav, the
    // slot 25 stand-in). Only that bit is dropped here: the other flag bits go on as they came.
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(
        without_depth_stencil_uav(pc_desc.Dimension, static_cast<std::uint32_t>(pc_desc.Flags)));

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 76 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout));
    }
    // With a provider for the unordered-access planes the texture keeps them (two resources behind one).
    if (supports_depth_uav(self, *xbox_desc)) {
        ID3D12Resource* resource = nullptr;
        bool refused = false;
        HRESULT hr = create_depth_uav_backing(self, heap_properties, heap_flags, depth_image_desc(pc_desc),
                                              depth_image_state(initial_state), clear_value, &resource, &refused);
        if (SUCCEEDED(hr)) {
            hr = resource->QueryInterface(*riid, out);
            if (SUCCEEDED(hr)) lease_title_resource_memory(self, *out, pc_desc, unsigned(xbox_desc->Layout));
            resource->Release();
            return hr;
        }
        if (!refused) return hr;
        // pc_desc has no unordered access already (without_depth_stencil_uav): the committed texture below is it.
        static std::atomic<bool> refusal_seen{false};
        note_depth_provider_refusal(refusal_seen, 76, xbox_desc->Format, hr);
    }
    const HRESULT hr = self->real->CreateCommittedResource(heap_properties, heap_flags, &pc_desc,
                                                           pc_initial_state(&pc_desc, initial_state), clear_value,
                                                           *riid, out);
    if (SUCCEEDED(hr)) lease_title_resource_memory(self, *out, pc_desc, xbox_layout);
    return hr;
}

// Device slot 26, GetCustomHeapProperties, as the retail executable calls it. Measured on 1.0.2.1 under
// gta6rt --run with a register probe in this slot's thunk: three calls right after device creation, each with the
// device proxy in RDX and not in RCX, R8 = 1 and R9 = 1, 2, 3 (node mask 1; DEFAULT, UPLOAD, READBACK). So the
// buffer for the returned D3D12_HEAP_PROPERTIES is in RCX and the device in RDX. That is how x64 passes them to a
// plain function that takes the device as its first argument and returns a struct; a PC C++ method has the device
// in RCX and the buffer in RDX. The forwarder took RCX for the proxy, read the PC vtable out of the title's stack
// buffer and faulted. The buffer's address goes back in RAX, as that convention returns it; what the title does with
// RAX is not measured.
D3D12_HEAP_PROPERTIES* proxy_get_custom_heap_properties(D3D12_HEAP_PROPERTIES* out, DeviceProxy* self,
                                                        UINT node_mask, D3D12_HEAP_TYPE heap_type) {
    note_slot(26);
    *out = self->real->GetCustomHeapProperties(node_mask, heap_type);
    return out;
}

// Device slot 133 places a resource at a virtual address: (va, desc, initial_state, clear_value, riid, out), the
// shape of the console's CreatePlacedResourceX. Measured locally at a title's call sites: riid is ID3D12Resource, va is
// the start of a range the title has just reserved for physical pages, desc is a 56-byte D3D12_RESOURCE_DESC with
// Xbox bits in Flags above the PC flags, and initial_state is all ones, which is no PC state. The PC has no call that
// puts a resource at a caller's address, so this creates a committed DEFAULT-heap resource from the cleaned
// description: the PC flags only, slot 76's layout rule, COMMON for a buffer or an all-ones state, and the clear value
// only for a render target or depth texture. The resource is not backed by the memory at va: what the title writes
// there does not reach it. Backing it needs the physical-page mapper; until then this gets the title past the call.
//
// The fourth word is read as a resource state here, which is what the retail boot ran through the story load with
// wherever this stand-in answered. The placed-at-VA handler reads the same word as a D3D12_BARRIER_LAYOUT (see
// d3d12_xs_device.h: 0x6 SHADER_RESOURCE, 0x3 UNORDERED_ACCESS, all ones UNDEFINED), and as a state 6 is
// INDEX_BUFFER | RENDER_TARGET, which a PC device refuses for a texture. LEONIDA_SLOT133_LAYOUT_WORD=1 reads it as a
// layout here too: through ID3D12Device10::CreateCommittedResource3 with the layout as given (UNDEFINED for a
// buffer), or on an older PC device through CreateCommittedResource with the legacy state of that layout.
constexpr std::uint32_t kPcResourceFlags = 0x3FF;  // D3D12_RESOURCE_FLAG_* up to USE_TIGHT_ALIGNMENT (0x200)

D3D12_RESOURCE_STATES legacy_state_for_layout(std::uint32_t layout, bool buffer) {
    if (buffer) return D3D12_RESOURCE_STATE_COMMON;
    switch (layout) {
        case 2: return D3D12_RESOURCE_STATE_RENDER_TARGET;        // D3D12_BARRIER_LAYOUT_RENDER_TARGET
        case 3: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;     // UNORDERED_ACCESS
        case 4: return D3D12_RESOURCE_STATE_DEPTH_WRITE;          // DEPTH_STENCIL_WRITE
        case 5: return D3D12_RESOURCE_STATE_DEPTH_READ;           // DEPTH_STENCIL_READ
        case 6: return D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;  // SHADER_RESOURCE
        case 7: return D3D12_RESOURCE_STATE_COPY_SOURCE;          // COPY_SOURCE
        case 8: return D3D12_RESOURCE_STATE_COPY_DEST;            // COPY_DEST
        default: return D3D12_RESOURCE_STATE_COMMON;              // UNDEFINED, COMMON and the rest
    }
}

HRESULT proxy_create_placed_resource_x(DeviceProxy* self, std::uint64_t va, const D3D12_RESOURCE_DESC* xbox_desc,
                                       std::uint32_t initial_state, const D3D12_CLEAR_VALUE* clear_value,
                                       const IID* riid, void** out) {
    note_slot(133);
    if (out) *out = nullptr;
    if (!xbox_desc) return E_INVALIDARG;
    if (!riid || !out) return E_POINTER;

    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    const bool buffer = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    pc_desc.Layout = buffer ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    static const bool layout_word = [] {
        const char* v = std::getenv("LEONIDA_SLOT133_LAYOUT_WORD");
        return v && *v == '1';
    }();
    const auto state = layout_word ? legacy_state_for_layout(initial_state, buffer)
                       : (buffer || initial_state == 0xFFFFFFFFu) ? D3D12_RESOURCE_STATE_COMMON
                                                                   : static_cast<D3D12_RESOURCE_STATES>(initial_state);
    const bool clearable =
        !buffer && (pc_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL));

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 places a resource at a VA: dimension=%d flags=0x%x -> 0x%x, state=0x%x; "
                     "created committed, not backed by the VA\n",
                     static_cast<int>(pc_desc.Dimension), xbox_flags, static_cast<unsigned>(pc_desc.Flags),
                     initial_state);
    (void)va;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                     D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    const D3D12_CLEAR_VALUE* const clear = clearable ? clear_value : nullptr;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    constexpr unsigned kDevice10Create = 76;  // ID3D12Device10::CreateCommittedResource3
    void* device10 = nullptr;
    if (layout_word && self->slot_count > kDevice10Create &&
        SUCCEEDED(self->real->QueryInterface(__uuidof(ID3D12Device10), &device10)) && device10) {
        D3D12_RESOURCE_DESC1 desc1{};
        std::memcpy(&desc1, &pc_desc, sizeof(pc_desc));  // DESC1 is DESC plus SamplerFeedbackMipRegion (zero)
        const std::uint32_t layout = buffer ? 0xFFFFFFFFu : initial_state;  // a buffer is created UNDEFINED
        using Fn = HRESULT(WINAPI*)(void*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*,
                                    std::uint32_t, const D3D12_CLEAR_VALUE*, void*, UINT, const DXGI_FORMAT*,
                                    const IID*, void**);
        const auto create = reinterpret_cast<Fn>((*reinterpret_cast<void***>(device10))[kDevice10Create]);
        const HRESULT hr = create(device10, &heap, D3D12_HEAP_FLAG_NONE, &desc1, layout, clear, nullptr, 0, nullptr,
                                  riid, out);
        static_cast<IUnknown*>(device10)->Release();
        return hr;
    }
#endif
    return self->real->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &pc_desc, state, clear, *riid, out);
}

// The Xbox routes at their configured indices. Returns false when `slot` is none of them. The built-in handlers of
// slots 70, 71 and 74 ask too, because an override that moves a route onto their slot must win over the built-in handler.
bool route_xbox_device_slot(DeviceProxy* self, unsigned slot, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                            std::uint64_t a4, std::uint64_t a5, std::uint64_t& result) {
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    if (slot == routes.frame_interval) {
        log_xbox_route_once(slot, "SetFrameIntervalX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_set_frame_interval_x(self, a1, a2, a3, a4));
    } else if (slot == routes.schedule_event) {
        log_xbox_route_once(slot, "ScheduleFrameEventX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_schedule_frame_event_x(self, a1, a2, a4));
    } else if (slot == routes.wait_event) {
        log_xbox_route_once(slot, "WaitFrameEventX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_wait_frame_event_x(self, a1, a2, a4, a5));
    } else if (slot == routes.frame_statistics) {
        HRESULT hr = S_OK;
        if (call_presentation(self, [&](const PresentationHandler& p) {
                if (!p.get_frame_statistics) return false;
                log_xbox_route_once(slot, "GetFrameStatisticsX", a1, a2, a3, a4, a5);
                hr = p.get_frame_statistics(p.context, a1, static_cast<UINT>(a2), reinterpret_cast<UINT*>(a3),
                                            reinterpret_cast<void*>(a4));
                return true;
            })) {
            result = hr_to_u64(hr);
        } else {
            // No presenter: nothing to report, as a success with a zero count. That is the answer the retail boot was
            // measured with (it ran through the story load on it); S_FALSE, the "not available yet" answer of the
            // public samples (docs/d3d12_vulkan.md), has not been through a retail run. A null count, or a count
            // above zero with no array to fill, is E_POINTER. Never the PC slot 81
            // (ID3D12Device13::OpenExistingHeapFromAddress1).
            log_xbox_route_once(slot, "GetFrameStatisticsX", a1, a2, a3, a4, a5);
            auto* count = reinterpret_cast<UINT*>(a3);
#ifdef LEONIDA_VULKAN_DEVICE
            if (auto* presentation = device_presentation(self)) {
                result = hr_to_u64(presentation->GetFrameStatisticsX(
                    a1, static_cast<UINT>(a2), count, reinterpret_cast<leonida::FrameStatistic*>(a4)));
                return true;
            }
#endif
            if (!count || (*count > 0 && !a4)) {
                result = hr_to_u64(E_POINTER);
            } else {
                *count = 0;
                result = hr_to_u64(S_OK);
            }
        }
    } else if (slot == routes.queue_x) {
        log_xbox_route_once(slot, "CreateCommandQueueX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_create_command_queue_xbox(self, reinterpret_cast<const std::uint32_t*>(a1),
                                                           reinterpret_cast<const IID*>(a2),
                                                           reinterpret_cast<void**>(a3)));
    } else if (slot == routes.list_x) {
        log_xbox_route_once(slot, "CreateCommandListX", a1, a2, a3, a4, a5);
        result = hr_to_u64(proxy_create_command_list_x(self, a1, a2, a3, reinterpret_cast<const IID*>(a4),
                                                       reinterpret_cast<void**>(a5)));
    } else {
        return false;
    }
    return true;
}

// Xbox device slot 127 (RT-5 stop 15, measured by Pipodi in #6982): the argument list of ID3D12Device10::
// CreateCommittedResource3, (heap properties, heap flags, desc1, initial layout, clear value, protected session,
// castable format count, castable formats, riid, out), with the Xbox layout word in desc1.Layout and Xbox bits above
// the PC flags in desc1.Flags. The slot is past the PC device's vtable, so the generic thunk could only refuse it. The
// PC device creates the resource through its own CreateCommittedResource3 (PC slot 76) with the layout word replaced
// as slot 76 does and the flags limited to the PC bits. The initial layout, clear value, session and castable list go
// on as the title passed them; 0xFFFFFFFF is D3D12_BARRIER_LAYOUT_UNDEFINED, the value the console passes for the
// state of slot 133 as well. The words take the positions the thunk received them in (see the top of the file).
constexpr unsigned kCreateCommittedResource3X = 127;
constexpr unsigned kPcCreateCommittedResource3 = 76;  // ID3D12Device10, the PC device's 77th method
// Slot 127 keeps its own mask (d3d12_xs_desc_clean.h has kPcResourceFlagsMask without this bit).
// D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT: a PC bit the Windows SDK 10.0.26100 header does not name yet; the PC runtime
// accepts it for committed resources (D3D12 tight alignment spec), so a title's request must reach it.
constexpr std::uint32_t kPcResourceFlagUseTightAlignment = 0x400;
constexpr std::uint32_t kPcResourceFlagsMaskSlot127 =
    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE |
    D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS |
    D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
    D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE | kPcResourceFlagUseTightAlignment;

HRESULT proxy_create_committed_resource3_x(DeviceProxy* self, std::uint64_t heap_word, std::uint64_t heap_flags,
                                           std::uint64_t desc_word, std::uint64_t layout_word,
                                           std::uint64_t clear_word, std::uint64_t session_word,
                                           std::uint64_t castable_count, std::uint64_t castable_formats,
                                           std::uint64_t riid_word, std::uint64_t out_word) {
    note_slot(kCreateCommittedResource3X);
    auto* const out = reinterpret_cast<void**>(out_word);
    if (out) *out = nullptr;  // _COM_Outptr_: NULL on every failure, as slots 14/62/76
    const auto* const xbox_desc = reinterpret_cast<const D3D12_RESOURCE_DESC1*>(desc_word);
    const auto* const riid = reinterpret_cast<const IID*>(riid_word);
    if (!out || !heap_word || !xbox_desc || !riid) return E_POINTER;
    if (self->slot_count <= kPcCreateCommittedResource3) {  // the PC device predates ID3D12Device10
        static std::atomic<bool> warned{false};
        if (first_call(warned))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: device slot %u (%s) needs ID3D12Device10 and the PC device has %u slots; "
                         "returning E_NOTIMPL\n",
                         kCreateCommittedResource3X, slot_name(kCreateCommittedResource3X), self->slot_count);
        return E_NOTIMPL;
    }

    D3D12_RESOURCE_DESC1 pc_desc = *xbox_desc;
    const auto xbox_layout = static_cast<std::uint32_t>(pc_desc.Layout);
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlagsMaskSlot127);

    static std::atomic<bool> logged{false};
    if (first_call(logged))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 127 resource: dimension=%d xbox layout=0x%x -> PC layout=0x%x, xbox flags "
                     "0x%x -> PC flags 0x%x\n",
                     static_cast<int>(pc_desc.Dimension), xbox_layout, static_cast<unsigned>(pc_desc.Layout),
                     xbox_flags, static_cast<unsigned>(pc_desc.Flags));

    using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                const D3D12_RESOURCE_DESC1*, std::uint32_t, const D3D12_CLEAR_VALUE*,
                                ID3D12ProtectedResourceSession*, std::uint32_t, const DXGI_FORMAT*, const IID*,
                                void**);
    // The ID3D12Device10 interface pointer, not the base device's: COM only promises that QueryInterface on the
    // original pointer yields that interface, and a different pointer has its own vtable. Released after the call.
    void* device10 = nullptr;
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (FAILED(self->real->QueryInterface(__uuidof(ID3D12Device10), &device10))) device10 = nullptr;
#endif
    if (!device10) return E_NOTIMPL;
    const auto create = reinterpret_cast<Fn>((*reinterpret_cast<void***>(device10))[kPcCreateCommittedResource3]);
    const HRESULT hr =
        create(static_cast<ID3D12Device*>(device10), reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(heap_word),
               static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(heap_flags)), &pc_desc,
               static_cast<std::uint32_t>(layout_word), reinterpret_cast<const D3D12_CLEAR_VALUE*>(clear_word),
               reinterpret_cast<ID3D12ProtectedResourceSession*>(session_word),
               static_cast<std::uint32_t>(castable_count), reinterpret_cast<const DXGI_FORMAT*>(castable_formats), riid,
               out);
    static_cast<IUnknown*>(device10)->Release();
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC plain{};  // D3D12_RESOURCE_DESC1 without SamplerFeedbackMipRegion
        plain.Dimension = pc_desc.Dimension;
        plain.Alignment = pc_desc.Alignment;
        plain.Width = pc_desc.Width;
        plain.Height = pc_desc.Height;
        plain.DepthOrArraySize = pc_desc.DepthOrArraySize;
        plain.MipLevels = pc_desc.MipLevels;
        plain.Format = pc_desc.Format;
        plain.SampleDesc = pc_desc.SampleDesc;
        plain.Layout = pc_desc.Layout;
        plain.Flags = pc_desc.Flags;
        lease_title_resource_memory(self, *out, plain, xbox_layout);
    }
    return hr;
}

// Device slot 133 is the console's CreatePlacedResourceX: (va, desc, state, clear, riid, out), measured on the retail
// executable (docs/retail_boot_chain.md, section 3.4). `va` is the start of a physical-page range the title has just
// mapped, `desc` a 56-byte description with Xbox bits above the PC resource flags and an Xbox layout word, and
// `state` is 0xFFFFFFFF, which is no PC state. The resource is a buffer on those pages, made by the handler the
// device behind the proxy registered (set_placed_at_va_handler), with the PC flags kept and the layout made
// ROW_MAJOR, as slot 76 does.
std::atomic<PlacedAtVaHandler> g_placed_at_va{nullptr};

HRESULT proxy_create_buffer_at_va(DeviceProxy* self, void* va, const D3D12_RESOURCE_DESC* xbox_desc,
                                  std::uint32_t state, const D3D12_CLEAR_VALUE* clear, const IID* riid, void** out) {
    if (out) *out = nullptr;
    // Without a handler (a PC device behind the proxy, or none registered) the slot refuses like any slot past the
    // PC vtable, whatever its arguments.
    const PlacedAtVaHandler handler = g_placed_at_va.load(std::memory_order_acquire);
    if (!handler) return E_NOTIMPL;
    if (!out || !xbox_desc || !riid) return E_POINTER;
    // An integer mask: a D3D12_RESOURCE_FLAGS constant past the SDK's last enumerator is not a constant expression
    // for clang-cl with SDK 10.0.26100 (found in review). The cast is applied to the masked value at run time.
    // The PC flags up to RAYTRACING_ACCELERATION_STRUCTURE (0x100) and USE_TIGHT_ALIGNMENT (0x400); 0x200 is
    // dropped, as committed_resource_refusal says the host refuses it (found in review).
    constexpr std::uint32_t kPcResourceFlags = 0x5ff;
    D3D12_RESOURCE_DESC pc_desc = *xbox_desc;
    const auto xbox_flags = static_cast<std::uint32_t>(pc_desc.Flags);
    pc_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(xbox_flags & kPcResourceFlags);
    // The Xbox layout word (0x101 tiled, 0x1 row-major buffer) is no PC layout: ROW_MAJOR for a buffer and UNKNOWN
    // for everything else, as slot 76 does.
    pc_desc.Layout = pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                         : D3D12_TEXTURE_LAYOUT_UNKNOWN;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 133 CreatePlacedResourceX: va=%p dimension=%d width=%llu xbox flags=0x%x "
                     "state=0x%x -> a buffer on the title's pages\n",
                     va, static_cast<int>(pc_desc.Dimension), static_cast<unsigned long long>(pc_desc.Width),
                     xbox_flags, state);
    }
    // The buffer's pages are committed before the handler walks them (set_placed_at_va_pages_hook).
    const PlacedAtVaPagesHook pages = g_placed_at_va_pages.load(std::memory_order_acquire);
    if (pages && pc_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && pc_desc.Width &&
        !pages(va, static_cast<std::size_t>(pc_desc.Width))) {
        static std::atomic<bool> refused{false};
        if (!refused.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr, "[rt] d3d12_xs: slot 133: committing the pages of va=%p width=%llu failed (error %lu)\n",
                         va, static_cast<unsigned long long>(pc_desc.Width), GetLastError());
        }
    }
    const bool clearable = pc_desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    return handler(self->real, va, pc_desc, state, *riid, out, clearable ? clear : nullptr);
}

// Slot 133 with both of its paths. The buffer on the title's own pages (proxy_create_buffer_at_va) is tried first: it
// is the only one whose memory is the memory at `va`. Where it cannot be made (no handler registered, a PC device
// behind the proxy, a texture, no VK_EXT_external_memory_host, or a range the import refuses), the committed DEFAULT
// stand-in (proxy_create_placed_resource_x) answers, so the title still gets a resource and does not assert; that
// one is not backed by the VA and its first-call log line says so.
HRESULT proxy_create_placed_resource_slot133(DeviceProxy* self, std::uint64_t va,
                                             const D3D12_RESOURCE_DESC* xbox_desc, std::uint32_t state,
                                             const D3D12_CLEAR_VALUE* clear_value, const IID* riid, void** out) {
    // A depth-stencil texture with unordered access is the depth provider's when one is set.
    if (out && xbox_desc && riid && self->depth_stencil_uav_provider && self->validate_depth_placement &&
        depth_uav_description(*xbox_desc)) {
        note_slot(133);
        *out = nullptr;
        bool refused = false;
        const HRESULT hr = proxy_create_placed_depth(self, va, *xbox_desc, state, clear_value, *riid, out, &refused);
        if (!refused) return hr;
        // The paths below make it without unordered access, as before the provider (depth_provider_refusal).
        *out = nullptr;
        static std::atomic<bool> refusal_seen{false};
        note_depth_provider_refusal(refusal_seen, 133, xbox_desc->Format, hr);
    }
    if (out && xbox_desc && riid) {
        const HRESULT hr = proxy_create_buffer_at_va(self, reinterpret_cast<void*>(va), xbox_desc, state, clear_value,
                                                     riid, out);
        if (SUCCEEDED(hr)) {
            note_slot(133);
            return hr;
        }
        *out = nullptr;
        static std::atomic<bool> logged{false};
        if (hr != E_NOTIMPL && !logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 133: no buffer on the title's pages at %p (hr=0x%08x); creating the "
                         "committed stand-in\n",
                         reinterpret_cast<void*>(va), static_cast<unsigned>(hr));
    }
    return proxy_create_placed_resource_x(self, va, xbox_desc, state, clear_value, riid, out);
}

// Slot 74: the retail title calls it once with (pointer, u32, pointer) (kapitche, issue 7231). The PC slot 74 is
// ID3D12Device9::ShaderCacheControl(kinds, control), which reads the first word as a flags value and returns
// E_INVALIDARG, and the title then stops on an int3. The words are not measured: nothing is written through the
// pointers, the PC device is not called, and the call succeeds. Only for the title's calls (set_console_caller, as
// slot 61): a host caller of ID3D12Device9::ShaderCacheControl through the proxy gets the PC method.
std::uint64_t proxy_slot74(std::uint64_t a1, std::uint64_t a2, std::uint64_t a3) {
    static std::atomic<bool> logged{false};
    bool expected = false;
    if (logged.compare_exchange_strong(expected, true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: slot 74 (pointer, u32, pointer) returns S_OK; words 0x%llx 0x%llx 0x%llx\n",
                     static_cast<unsigned long long>(a1), static_cast<unsigned long long>(a2),
                     static_cast<unsigned long long>(a3));
    return hr_to_u64(S_OK);
}

// Keeps a copy of the stream a PSO was created from, on the PSO, so slot 55 can derive from it. A stream the module
// does not model is logged once: the PSO is still returned, and slot 55 refuses it as a base.
void keep_pso_stream(IUnknown* pso, const D3D12_PIPELINE_STATE_STREAM_DESC& desc) {
    ID3D12Object* object = nullptr;
    if (FAILED(pso->QueryInterface(__uuidof(ID3D12Object), reinterpret_cast<void**>(&object))) || !object) return;
    auto record = PsoStreamRecord::capture(desc, used_console_stream);
    static std::atomic<bool> logged{false};
    if (!record || !attach_pso_record(object, std::move(record))) {
        if (!logged.exchange(true, std::memory_order_relaxed))
            std::fprintf(stderr, "[rt] d3d12_xs: a pipeline state stream is not kept for derivation (unmodelled subobject or allocation failure); slot 55 refuses that PSO as a base\n");
    }
    object->Release();
}

// The PC CreatePipelineState (PC slot 47) that slots 86 and 55 share: on success the stream is kept on the PSO.
HRESULT create_pso_from_stream(DeviceProxy* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, const IID* riid,
                               void** out, std::unique_ptr<PsoStreamRecord> derived = {}) {
    if (!riid) return E_POINTER;
    if (self->prepare_stream && desc && out && IsEqualIID(*riid, __uuidof(ID3D12PipelineState)))
        return self->prepare_stream(self->real, *desc, *riid, out, std::move(derived));
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_PIPELINE_STATE_STREAM_DESC*, const IID*, void**);
    used_console_stream = false;
    d3d12_trace::stream_refusal = nullptr;
    const HRESULT hr = reinterpret_cast<Fn>(self->real_vtable[47])(self->real, desc, riid, out);
    static const bool audit = [] {
        char value[2]{};
        return GetEnvironmentVariableA("LEONIDA_PSO_STREAM_AUDIT", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    if (audit && desc && (hr == E_NOTIMPL || hr == E_FAIL)) {
        try {
            audit_pso_stream(*desc, hr, used_console_stream);
        } catch (const std::bad_alloc&) {}
    }
    if (SUCCEEDED(hr) && out && *out && desc) keep_pso_stream(static_cast<IUnknown*>(*out), *desc);
    return hr;
}

// Xbox slot 86: CreatePipelineState(desc, riid, out), forwarded to PC ID3D12Device2::CreatePipelineState (PC slot 47).
HRESULT proxy_create_pipeline_state(DeviceProxy* self, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, const IID* riid,
                                    void** out) {
    if (out) *out = nullptr;
    if (self->slot_count <= 47) return refuse_beyond_device(86, self->slot_count);  // predates ID3D12Device2
    return create_pso_from_stream(self, desc, riid, out);
}

// Logs a slot 55 refusal once per reason, and returns the HRESULT.
HRESULT refuse_derived(unsigned reason, const char* why, HRESULT hr) {
    static std::atomic<bool> logged[4]{};
    if (!logged[reason].exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr, "[rt] d3d12_xs: derived pipeline state (device slot 55) refused: %s\n", why);
    return hr;
}

// Xbox slot 55: CreateDerivedPipelineState(pBasePSO, n, pRecords, riid, ppv). The base must be a PSO that slot 86 or
// slot 55 created with a kept stream; n is 1..4 records of 0x14c bytes (d3d12_xs_pso_stream.h). The patched stream
// goes through the slot 86 path. Every refusal leaves *ppv null and returns a failure, never a fake success.
HRESULT proxy_create_derived_pso(DeviceProxy* self, ID3D12PipelineState* base, std::uint32_t count,
                                 const unsigned char* records, const IID* riid, void** out) {
    note_slot(55);
    if (out) *out = nullptr;
    if (self->slot_count <= 47) return refuse_beyond_device(55, self->slot_count);  // predates ID3D12Device2
    if (!out || !riid || !records) return E_POINTER;
    const PsoStreamRecord* base_record = base ? find_pso_record(base) : nullptr;
    if (!base_record) return refuse_derived(0, "base PSO has no kept stream", E_FAIL);
    if (!count || count > kDerivedMaxRecords) return E_INVALIDARG;
    if (!accessible_range(records, std::size_t(count) * kDerivedRecordSize, false)) return E_INVALIDARG;
    std::vector<unsigned char> patched;
    switch (derive_stream(*base_record, records, count, patched)) {
        case DeriveStatus::ok: break;
        case DeriveStatus::unknown_tag: return refuse_derived(1, "record tag not known", E_INVALIDARG);
        case DeriveStatus::missing_state: return refuse_derived(2, "base has no subobject for a record", E_INVALIDARG);
        case DeriveStatus::malformed: return refuse_derived(3, "record count or base stream malformed", E_INVALIDARG);
    }
    auto record = PsoStreamRecord::derived(*base_record, std::move(patched));
    const auto desc = record->desc();
    return create_pso_from_stream(self, &desc, riid, out, std::move(record));
}

// Xbox slot 101: CreateStateObject(desc, riid, out), forwarded to PC ID3D12Device5::CreateStateObject (PC slot 62,
// displaced on Xbox by compute-PSO slot 62).
HRESULT proxy_create_state_object(DeviceProxy* self, const D3D12_STATE_OBJECT_DESC* desc, const IID* riid, void** out) {
    if (out) *out = nullptr;
    if (self->slot_count <= 62) return refuse_beyond_device(101, self->slot_count);  // predates ID3D12Device5
    if (!riid) return E_POINTER;
    using Fn = HRESULT(WINAPI*)(ID3D12Device*, const D3D12_STATE_OBJECT_DESC*, const IID*, void**);
    return reinterpret_cast<Fn>(self->real_vtable[62])(self->real, desc, riid, out);
}

// Xbox slot 108: GetResourceAllocationInfo2, forwarded to PC ID3D12Device8::GetResourceAllocationInfo2 (PC slot 68).
// The retail executable calls struct-returning device methods with the C ABI (return buffer in RCX, device proxy in
// RDX); a PC C++ caller passes the proxy in RCX and the return buffer in RDX. Both orders are accepted. The title's
// descriptor builder populates 56-byte D3D12_RESOURCE_DESC entries, so the input array is indexed with a 56-byte
// stride (sizeof(D3D12_RESOURCE_DESC)), each entry is expanded into a zeroed D3D12_RESOURCE_DESC1 with
// SamplerFeedbackMipRegion cleared, and Xbox layout words (e.g. 0x101, 0x105) and console flag bits above the PC
// D3D12_RESOURCE_FLAGS are normalized on the copy before the PC call.
void apply_console_placement(const D3D12_RESOURCE_DESC* descs, UINT count, D3D12_RESOURCE_ALLOCATION_INFO& info,
                             D3D12_RESOURCE_ALLOCATION_INFO1* each,
                             const std::function<D3D12_RESOURCE_ALLOCATION_INFO(UINT)>& pc_one, unsigned slot,
                             const D3D12_RESOURCE_DESC* title_descs);

std::uint64_t proxy_get_resource_allocation_info2(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2,
                                                  std::uint64_t a3, std::uint64_t a4, std::uint64_t a5) {
    DeviceProxy* proxy = nullptr;
    D3D12_RESOURCE_ALLOCATION_INFO* ret = nullptr;
    if (is_device_proxy(self)) {
        proxy = self;
        ret = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(a1);
    } else if (is_device_proxy(reinterpret_cast<const void*>(a1))) {
        proxy = reinterpret_cast<DeviceProxy*>(a1);
        ret = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(self);
    }
    if (!ret) return 0;
    if (!proxy) {
        *ret = {UINT64_MAX, 0};
        return reinterpret_cast<std::uint64_t>(ret);
    }
    if (proxy->slot_count <= 68) {
        refuse_beyond_device(108, proxy->slot_count);
        *ret = {UINT64_MAX, 0};
        return reinterpret_cast<std::uint64_t>(ret);
    }
    const auto visible_mask = static_cast<UINT>(a2);
    const auto count = static_cast<UINT>(a3);
    const auto* raw_descs = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a4);
    auto* info1 = reinterpret_cast<D3D12_RESOURCE_ALLOCATION_INFO1*>(a5);
    if (count > 0 && !raw_descs) {  // a count with no descriptor array: refuse like the other bad-input paths
        *ret = {UINT64_MAX, 0};
        return reinterpret_cast<std::uint64_t>(ret);
    }

    // The title sizes a texture here when it creates it and through slot 25 when it budgets it (its transient pool
    // check), so both must give one answer: the same cleaning (pc_resource_desc: console flags, a depth target's
    // unordered access) and the same console sizes (apply_console_placement) as slot 25.
    std::vector<D3D12_RESOURCE_DESC1> cleaned;
    std::vector<D3D12_RESOURCE_DESC> pc;
    std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> local;
    const D3D12_RESOURCE_DESC1* descs_to_pass = nullptr;
    if (raw_descs && count > 0) {
        try {
            cleaned.resize(count);
            pc.resize(count);
            if (!info1) {
                local.resize(count);
                info1 = local.data();
            }
        } catch (...) {
            *ret = {UINT64_MAX, 0};
            return reinterpret_cast<std::uint64_t>(ret);
        }
        for (UINT i = 0; i < count; ++i) {
            D3D12_RESOURCE_DESC1 d{};
            pc[i] = pc_resource_desc(raw_descs[i]);
            std::memcpy(&d, &pc[i], sizeof(D3D12_RESOURCE_DESC));
            d.SamplerFeedbackMipRegion = {};
            if (static_cast<std::uint32_t>(d.Layout) >
                static_cast<std::uint32_t>(D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)) {
                d.Layout = d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12_TEXTURE_LAYOUT_ROW_MAJOR
                                                                          : D3D12_TEXTURE_LAYOUT_UNKNOWN;
            }
            constexpr std::uint32_t kPcResourceFlagsMask = 0x1FFu;
            d.Flags = static_cast<D3D12_RESOURCE_FLAGS>(static_cast<std::uint32_t>(d.Flags) & kPcResourceFlagsMask);
            cleaned[i] = d;
        }
        descs_to_pass = cleaned.data();
    }

    using Fn = D3D12_RESOURCE_ALLOCATION_INFO*(WINAPI*)(
        ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC1*,
        D3D12_RESOURCE_ALLOCATION_INFO1*);
    reinterpret_cast<Fn>(proxy->real_vtable[68])(proxy->real, ret, visible_mask, count, descs_to_pass, info1);
    if (info1 && !pc.empty())
        apply_console_placement(pc.data(), count, *ret, info1, [&](UINT i) {
            return D3D12_RESOURCE_ALLOCATION_INFO{info1[i].SizeInBytes, info1[i].Alignment};
        }, 108, raw_descs);
    return reinterpret_cast<std::uint64_t>(ret);
}

template <unsigned Slot>
std::uint64_t WINAPI proxy_slot(DeviceProxy* self, std::uint64_t a1, std::uint64_t a2, std::uint64_t a3,
                                std::uint64_t a4, std::uint64_t a5, std::uint64_t a6, std::uint64_t a7,
                                std::uint64_t a8, std::uint64_t a9, std::uint64_t a10, std::uint64_t a11,
                                std::uint64_t a12, std::uint64_t a13, std::uint64_t a14) {
    if constexpr (Slot == 0) {
        return hr_to_u64(proxy_query_interface(self, reinterpret_cast<const IID*>(a1),
                                               reinterpret_cast<void**>(a2)));
    } else if constexpr (Slot == 1) {
        return proxy_add_ref(self);
    } else if constexpr (Slot == 2) {
        return proxy_release(self);
    } else if constexpr (Slot == 8) {
        return hr_to_u64(proxy_create_command_queue(self, reinterpret_cast<const D3D12_COMMAND_QUEUE_DESC*>(a1),
                                                    reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 9) {
        return hr_to_u64(proxy_create_command_allocator(self, a1, reinterpret_cast<const IID*>(a2),
                                                        reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 12) {
        return hr_to_u64(proxy_create_command_list(self, a1, a2, a3, a4, reinterpret_cast<const IID*>(a5),
                                                   reinterpret_cast<void**>(a6)));
    } else if constexpr (Slot == 13) {
        const bool console = is_console_caller(_ReturnAddress());
        struct RestoreFeatureQuery {
            bool previous;
            ~RestoreFeatureQuery() { g_console_feature_query = previous; }
        } restore{std::exchange(g_console_feature_query, console)};
        const auto feature = static_cast<D3D12_FEATURE>(a1);
        if (feature == D3D12_FEATURE_FORMAT_INFO && a3 == sizeof(D3D12_FEATURE_DATA_FORMAT_INFO) &&
            console && local_accessible(a2, sizeof(D3D12_FEATURE_DATA_FORMAT_INFO), true)) {
            auto* info = reinterpret_cast<D3D12_FEATURE_DATA_FORMAT_INFO*>(a2);
            const auto format = static_cast<unsigned>(info->Format);
            // Xbox D16/S8 and its depth and stencil aliases occupy slots reserved by desktop DXGI.
            if (format >= 118 && format <= 120) {
                info->PlaneCount = 2;
                return hr_to_u64(S_OK);
            }
        }
        return hr_to_u64(self->real->CheckFeatureSupport(feature, reinterpret_cast<void*>(a2), static_cast<UINT>(a3)));
    } else if constexpr (Slot == 36) {
        return hr_to_u64(proxy_create_fence(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 32) {
        return hr_to_u64(proxy_open_shared_handle(self, a1, reinterpret_cast<const IID*>(a2),
                                                  reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 31 || Slot == 34 || Slot == 35 || Slot == 45 || Slot == 46 || Slot == 50) {
        return hr_to_u64(proxy_unwrapping_call<Slot>(self, a1, a2, a3, a4, a5));
    } else if constexpr (Slot == 74) {
        std::uint64_t routed = 0;
        if (route_xbox_device_slot(self, Slot, a1, a2, a3, a4, a5, routed)) return routed;  // an override moved a route here
        note_slot(Slot);
        if (is_console_caller(_ReturnAddress())) return proxy_slot74(a1, a2, a3);
        // A host caller: the PC slot 74 (ID3D12Device9::ShaderCacheControl), forwarded like any other PC method, as
        // slot 61 does. With no classifier installed every caller is a host one.
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    } else if constexpr (Slot == 75) {
        return hr_to_u64(proxy_create_command_queue1(self, a1, a2, reinterpret_cast<const IID*>(a3),
                                                     reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 14) {
        return hr_to_u64(proxy_create_descriptor_heap(
            self, reinterpret_cast<const D3D12_DESCRIPTOR_HEAP_DESC*>(a1),
            reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 17) {
        proxy_create_cbv(self, reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(a1),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)});
        return 0;
    } else if constexpr (Slot == 18) {
        proxy_create_srv(self, reinterpret_cast<ID3D12Resource*>(a1),
                         reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(a2),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        return 0;
    } else if constexpr (Slot == 19) {
        proxy_create_uav(self, reinterpret_cast<ID3D12Resource*>(a1), reinterpret_cast<ID3D12Resource*>(a2),
                         reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(a3),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a4)});
        return 0;
    } else if constexpr (Slot == 20) {
        proxy_create_rtv(self, reinterpret_cast<ID3D12Resource*>(a1),
                         reinterpret_cast<const D3D12_RENDER_TARGET_VIEW_DESC*>(a2),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        return 0;
    } else if constexpr (Slot == 21) {
        proxy_create_dsv(self, reinterpret_cast<ID3D12Resource*>(a1),
                         reinterpret_cast<const D3D12_DEPTH_STENCIL_VIEW_DESC*>(a2),
                         D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)});
        return 0;
    } else if constexpr (Slot == 22) {
        proxy_create_sampler(self, reinterpret_cast<const D3D12_SAMPLER_DESC*>(a1),
                             D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)});
        return 0;
    } else if constexpr (Slot == 23) {
        proxy_copy_descriptors(self, static_cast<UINT>(a1), reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a2),
                               reinterpret_cast<const UINT*>(a3), static_cast<UINT>(a4),
                               reinterpret_cast<const D3D12_CPU_DESCRIPTOR_HANDLE*>(a5),
                               reinterpret_cast<const UINT*>(a6),
                               static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(static_cast<UINT>(a7)));
        return 0;
    } else if constexpr (Slot == 24) {
        proxy_copy_descriptors_simple(self, static_cast<UINT>(a1),
                                      D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a2)},
                                      D3D12_CPU_DESCRIPTOR_HANDLE{static_cast<SIZE_T>(a3)},
                                      static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(static_cast<UINT>(a4)));
        return 0;
    } else if constexpr (Slot == 57) {
        // Xbox slot 57 is DeserializeGraphicsPipelineStateX(desc, riid, out), whereas PC slot 57 is
        // ID3D12Device4::CreateProtectedResourceSession(desc, riid, out). Both pass `riid` in `a2`; when `riid`
        // is IID_ID3D12PipelineState (or `a2` is null so the slot-57 null-IID path is tested), route to graphics
        // PSO deserialization. When `riid` is IID_ID3D12ProtectedResourceSession*, forward to the PC device.
        const IID* riid = reinterpret_cast<const IID*>(a2);
        if (!riid || *riid == __uuidof(ID3D12PipelineState)) {
            note_slot(57);
            log_xbox_route_once(57, "DeserializeGraphicsPipelineStateX", a1, a2, a3, a4, a5);
            HRESULT hr = E_NOTIMPL;
            if (riid && self->native_graphics)
                hr = proxy_create_native_graphics(self, reinterpret_cast<const void*>(a1), *riid,
                                                  reinterpret_cast<void**>(a3));
            if (hr == E_NOTIMPL)
                hr = proxy_create_graphics_pipeline_from_native_code(
                    self, reinterpret_cast<const void*>(a1), riid, reinterpret_cast<void**>(a3));
            return hr_to_u64(d3d12_trace::result("XboxID3D12Device::CreateNativeGraphicsPipelineState", hr));
        }
        note_slot(57);
        if (57 >= self->slot_count) return hr_to_u64(refuse_beyond_device(57, self->slot_count));
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[57]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    } else if constexpr (Slot == 62) {
        return hr_to_u64(proxy_create_compute_pipeline_state(
            self, reinterpret_cast<const D3D12_COMPUTE_PIPELINE_STATE_DESC*>(a1), a2, a3,
            reinterpret_cast<const IID*>(a4), reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == 41) {
        return hr_to_u64(proxy_create_command_signature(
            self, reinterpret_cast<const D3D12_COMMAND_SIGNATURE_DESC*>(a1), a2, reinterpret_cast<const IID*>(a3),
            reinterpret_cast<void**>(a4)));
    } else if constexpr (Slot == 61) {
        if (is_console_caller(_ReturnAddress())) {
            proxy_create_sampler_x(self, reinterpret_cast<const D3D12_SAMPLER_DESC*>(a1), a2);
            return 0;
        }
        // A host caller: the PC slot 61 (ID3D12Device5::CreateMetaCommand), forwarded like any other PC method.
        note_slot(Slot);
        if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
        using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                 std::uint64_t, std::uint64_t, std::uint64_t);
        auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
        return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
    } else if constexpr (Slot == 67) {
        return hr_to_u64(proxy_create_compute_pipeline_from_native_code(
            self, reinterpret_cast<const void*>(a1), reinterpret_cast<const IID*>(a2),
            reinterpret_cast<void**>(a3)));
    } else if constexpr (Slot == 133) {
        const auto* desc = reinterpret_cast<const D3D12_RESOURCE_DESC*>(a2);
        auto** out = reinterpret_cast<void**>(a6);
        const HRESULT hr = proxy_create_placed_resource_slot133(
            self, a1, desc, static_cast<std::uint32_t>(a3), reinterpret_cast<const D3D12_CLEAR_VALUE*>(a4),
            reinterpret_cast<const IID*>(a5), out);
        if (SUCCEEDED(hr) && desc) {
            if (a1 && out && *out && self->texture_lease_provider && desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                (unsigned(desc->Layout)==1u || unsigned(desc->Layout)==0x100u))
                self->texture_lease_provider(static_cast<IUnknown*>(*out),reinterpret_cast<void*>(a1),0,unsigned(desc->Layout));
        }
        return hr_to_u64(hr);
    } else if constexpr (Slot == 70) {
        std::uint64_t routed = 0;
        if (route_xbox_device_slot(self, Slot, a1, a2, a3, a4, a5, routed)) return routed;  // an override moved a route here
        proxy_create_placed_view_x(self, false, a1, a2, a3);
        return 0;
    } else if constexpr (Slot == 71) {
        std::uint64_t routed = 0;
        if (route_xbox_device_slot(self, Slot, a1, a2, a3, a4, a5, routed)) return routed;  // an override moved a route here
        proxy_create_placed_view_x(self, true, a1, a2, a3);
        return 0;
    } else if constexpr (Slot == 76) {
        return hr_to_u64(proxy_create_resource(
            self, reinterpret_cast<const D3D12_HEAP_PROPERTIES*>(a1),
            static_cast<D3D12_HEAP_FLAGS>(static_cast<std::uint32_t>(a2)),
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(a3),
            static_cast<D3D12_RESOURCE_STATES>(static_cast<std::uint32_t>(a4)),
            reinterpret_cast<const D3D12_CLEAR_VALUE*>(a5), reinterpret_cast<const IID*>(a6),
            reinterpret_cast<void**>(a7)));
    } else if constexpr (Slot == 55) {
        return hr_to_u64(proxy_create_derived_pso(
            self, reinterpret_cast<ID3D12PipelineState*>(a1), static_cast<std::uint32_t>(a2),
            reinterpret_cast<const unsigned char*>(a3), reinterpret_cast<const IID*>(a4),
            reinterpret_cast<void**>(a5)));
    } else if constexpr (Slot == kCreateCommittedResource3X) {
        return hr_to_u64(proxy_create_committed_resource3_x(self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10));
    } else {
        if constexpr (Slot == 26) {
            // The console form has the device in RDX. A PC C++ caller has the proxy in RCX and its return buffer in
            // RDX; that call goes to the forwarder below, as before.
            if (is_device_proxy(reinterpret_cast<const void*>(a1)))
                return reinterpret_cast<std::uint64_t>(proxy_get_custom_heap_properties(
                    reinterpret_cast<D3D12_HEAP_PROPERTIES*>(self), reinterpret_cast<DeviceProxy*>(a1),
                    static_cast<UINT>(a2), static_cast<D3D12_HEAP_TYPE>(static_cast<std::uint32_t>(a3))));
        }
        note_slot(Slot);
        // Xbox device slots, before the PC vtable is indexed: the public HLE's 78/79/80 would otherwise reach
        // CreateReservedResource2/CreateSampler2 or fall past an 80-slot table, and 48/73 would reach unrelated PC
        // methods. The check is before the slot-count refusal because slot 80 is outside a PC ID3D12Device11 table.
        std::uint64_t routed = 0;
        if (route_xbox_device_slot(self, Slot, a1, a2, a3, a4, a5, routed)) return routed;
        if constexpr (Slot == 86) {
            return hr_to_u64(proxy_create_pipeline_state(
                self, reinterpret_cast<const D3D12_PIPELINE_STATE_STREAM_DESC*>(a1),
                reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
        } else if constexpr (Slot == 101) {
            return hr_to_u64(proxy_create_state_object(
                self, reinterpret_cast<const D3D12_STATE_OBJECT_DESC*>(a1),
                reinterpret_cast<const IID*>(a2), reinterpret_cast<void**>(a3)));
        } else if constexpr (Slot == 108) {
            return proxy_get_resource_allocation_info2(self, a1, a2, a3, a4, a5);
        } else {
            if (Slot >= self->slot_count) return hr_to_u64(refuse_beyond_device(Slot, self->slot_count));
            if constexpr (Slot == 27 || Slot == 53) {
                return proxy_create_committed_resource<Slot>(self, a1, a2, a3, a4, a5, a6, a7, a8);
            } else {
                // The PC vtable entry, called with the PC device as `this`. Integer/pointer arguments, the hidden
                // struct-return pointer and any stack arguments keep their positions; XMM registers are untouched.
                using ForwardFn = std::uint64_t(WINAPI*)(void*, std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                         std::uint64_t, std::uint64_t, std::uint64_t);
                auto fn = reinterpret_cast<ForwardFn>(self->real_vtable[Slot]);
                return fn(self->real, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
            }
        }
    }
}

template <std::size_t... I>
void fill_vtable(void** vtable, std::index_sequence<I...>) {
    ((vtable[I] = reinterpret_cast<void*>(&proxy_slot<static_cast<unsigned>(I)>) ), ...);
}

// ---- Struct-returning device methods: the console title calls them with the C ABI ----
//
// The PC vtable (MSVC C++) takes `this` in RCX and, for a struct return, the hidden return pointer in RDX. The console
// title does not. Measured with the retail game_scarlett_master.exe 1.0.2.1 under gta6rt --run: its first
// GetCustomHeapProperties call (device slot 26) is `rcx = return buffer on the caller's stack, rdx = the device,
// r8 = node mask 1, r9 = heap type` (DEFAULT, then UPLOAD), and it reads the result from that buffer. That is the C
// convention, where a struct wider than 8 bytes comes back through a hidden *first* argument (an 8-byte one comes back
// in RAX, which is what the heap proxy's slots 9/10 already measured). The generic forwarder took the return buffer
// for the device and crashed on its vtable.
//
// The runtime's own code and tests still call the proxy through d3d12.h, in the PC form, so these thunks accept both,
// as the heap proxy's 9/10 do: whichever of the first two arguments is the device proxy (its first word is the proxy
// vtable) is `this`, and the other is the return buffer. A return buffer never starts with that vtable pointer: the
// C-form caller hands over uninitialised memory to be written, and the PC-form caller is this runtime. The buffer is
// returned in RAX, as both conventions require.
//   slot 25 GetResourceAllocationInfo -> D3D12_RESOURCE_ALLOCATION_INFO (16 bytes): same rule as slot 26 (inferred,
//           no call site measured yet)
//   slot 26 GetCustomHeapProperties   -> D3D12_HEAP_PROPERTIES (20 bytes): measured; it has its own thunk above
//           (proxy_get_custom_heap_properties), not this one
// Not changed: slot 43 GetAdapterLuid (an 8-byte LUID; by the same rule the title would expect it in RAX, but no call
// site is measured yet, so it still forwards in the PC form).
void** get_vtable();

struct StructReturnCall {
    DeviceProxy* self;
    void* ret;
    bool console_abi;
};

StructReturnCall struct_return_call(std::uint64_t a0, std::uint64_t a1) {
    void** const vtable = get_vtable();
    if (a0 && *reinterpret_cast<void***>(a0) == vtable)  // PC form: this, return buffer
        return {reinterpret_cast<DeviceProxy*>(a0), reinterpret_cast<void*>(a1), false};
    return {reinterpret_cast<DeviceProxy*>(a1), reinterpret_cast<void*>(a0), true};  // C form: return buffer, this
}

// The console size of a list of descriptions (d3d12_xs_alloc_size.h): the PC device's answer with every console render
// target laid out at its console size, each at its own alignment. `info` is the PC answer and is replaced only when
// the console layout is larger. `each` is the per-description array of slot 56, or null. `title_descs` is the
// title's own array when `descs` is its PC form (the layout words the PC form drops), else null.
void apply_console_placement(const D3D12_RESOURCE_DESC* descs, UINT count, D3D12_RESOURCE_ALLOCATION_INFO& info,
                             D3D12_RESOURCE_ALLOCATION_INFO1* each,
                             const std::function<D3D12_RESOURCE_ALLOCATION_INFO(UINT)>& pc_one, unsigned slot,
                             const D3D12_RESOURCE_DESC* title_descs) {
    if (info.SizeInBytes == UINT64_MAX) return;
    D3D12_RESOURCE_ALLOCATION_INFO console{};
    if (!console_placement(descs, count, pc_one, console, each, title_descs)) return;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed))
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot %u: render targets sized as console tiles, %llu B for %llu B on %u "
                     "description(s)\n",
                     slot, static_cast<unsigned long long>(console.SizeInBytes),
                     static_cast<unsigned long long>(info.SizeInBytes), static_cast<unsigned>(count));
    // The first few textures the title writes with the CPU through its lock that took their console layout's size
    // (console_unordered_access_size).
    static std::atomic<int> uav_logged{0};
    for (UINT i = 0; descs && i < count && uav_logged.load(std::memory_order_relaxed) < 8; ++i) {
        const D3D12_RESOURCE_DESC& d = descs[i];
        const UINT layout = static_cast<UINT>((title_descs ? title_descs : descs)[i].Layout);
        const UINT64 written = console_unordered_access_size(d, layout);
        if (!written || uav_logged.fetch_add(1, std::memory_order_relaxed) >= 8) continue;
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot %u: unordered-access texture dimension %d %llux%ux%u mips %u format %d "
                     "layout 0x%x takes its console layout's %llu B (list %llu B, PC %llu B)\n",
                     slot, static_cast<int>(d.Dimension), static_cast<unsigned long long>(d.Width), d.Height,
                     unsigned(d.DepthOrArraySize), unsigned(d.MipLevels), static_cast<int>(d.Format), layout,
                     static_cast<unsigned long long>(written), static_cast<unsigned long long>(console.SizeInBytes),
                     static_cast<unsigned long long>(info.SizeInBytes));
    }
    // A list with a depth-stencil target may come out smaller than the PC answer (console_depth_stencil_size).
    bool depth = false;
    for (UINT i = 0; descs && i < count && !depth; ++i) {
        UINT64 xg_size = 0, xg_alignment = 0;
        depth = console_depth_stencil_size(descs[i]) != 0 || console_texture_layout(descs[i], xg_size, xg_alignment);
    }
    if (depth || console.SizeInBytes >= info.SizeInBytes) info = console;
}

void* WINAPI proxy_get_resource_allocation_info(std::uint64_t a0, std::uint64_t a1, std::uint64_t visible_mask,
                                                std::uint64_t count, const D3D12_RESOURCE_DESC* descs) {
    note_slot(25);
    const StructReturnCall call = struct_return_call(a0, a1);
    ID3D12Device* const real = call.self->real;
    // Normalize console descriptors in a copy. PC calls can also pass the Xbox tiled layout explicitly. Otherwise
    // their flags and layout reach the host unchanged so the host can validate them.
    const UINT desc_count = static_cast<UINT>(count);
    const D3D12_RESOURCE_DESC* const title_descs = descs;
    std::unique_ptr<D3D12_RESOURCE_DESC[]> clean;
    if (descs && desc_count) {
        clean.reset(new (std::nothrow) D3D12_RESOURCE_DESC[desc_count]);
        if (!clean) {
            *static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(call.ret) = {UINT64_MAX, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
            return call.ret;
        }
        for (UINT i = 0; i < desc_count; ++i) {
            const auto& desc = descs[i];
            clean[i] = call.console_abi || desc.Layout == kXboxTiledResourceLayout ? pc_resource_desc(desc) : desc;
            static std::atomic<bool> uav_dropped{false};
            if (((static_cast<std::uint32_t>(desc.Flags) & ~static_cast<std::uint32_t>(clean[i].Flags)) &
                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
                first_call(uav_dropped))
                std::fprintf(stderr,
                             "[rt] d3d12_xs: slot 25: depth-stencil texture (format %d) sized without unordered "
                             "access, which the PC refuses with it (stand-in)\n",
                             static_cast<int>(desc.Format));
        }
        descs = clean.get();
    }
    const UINT mask = static_cast<UINT>(visible_mask);
    D3D12_RESOURCE_ALLOCATION_INFO info = real->GetResourceAllocationInfo(mask, desc_count, descs);
    // The title asserts that the size fits in 32 bits, so a description the PC refuses (UINT64_MAX) stops the boot.
    // Each refused description is logged once per (dimension, format, flags) so the next stand-in can be found.
    if (info.SizeInBytes == UINT64_MAX && descs) {
        static std::mutex mutex;
        static std::set<std::uint64_t> seen;
        for (UINT i = 0; i < desc_count; ++i) {
            const D3D12_RESOURCE_DESC& d = descs[i];
            const std::uint64_t key = (std::uint64_t(d.Dimension) << 56) | (std::uint64_t(d.Format) << 40) |
                                      static_cast<std::uint32_t>(d.Flags);
            {
                const std::lock_guard lock(mutex);
                if (!seen.insert(key).second) continue;
            }
            std::fprintf(stderr,
                         "[rt] d3d12_xs: slot 25: the PC refuses to size a description: dimension=%d format=%d "
                         "%llux%ux%u mips=%u samples=%u/%u layout=0x%x flags=0x%x (console flags 0x%x)\n",
                         static_cast<int>(d.Dimension), static_cast<int>(d.Format),
                         static_cast<unsigned long long>(d.Width), d.Height, d.DepthOrArraySize, d.MipLevels,
                         d.SampleDesc.Count, d.SampleDesc.Quality, static_cast<unsigned>(d.Layout),
                         static_cast<unsigned>(d.Flags), static_cast<unsigned>(title_descs[i].Flags));
        }
    }
    const UINT64 pc_size = info.SizeInBytes;
    // The console tile sizes are the title's: a call in the console form gets them. A PC-form caller (the runtime's
    // own code and tests, which compare with the PC device) gets the PC device's answer unchanged.
    if (call.console_abi)
        apply_console_placement(descs, desc_count, info, nullptr,
                                [&](UINT i) { return real->GetResourceAllocationInfo(mask, 1, &descs[i]); }, 25, title_descs);
    // LEONIDA_LOG_ALLOCINFO=1: every call, with the calling thread (the title sums a batch of these against a fixed
    // pool and asserts when the batch does not fit).
    static const bool log_all = [] {
        wchar_t value[2]{};
        return GetEnvironmentVariableW(L"LEONIDA_LOG_ALLOCINFO", value, 2) == 1 && value[0] == L'1';
    }();
    if (log_all && descs && desc_count)
        std::fprintf(stderr,
                     "[rt] d3d12_xs: slot 25 tid %lx n %u: dim %d fmt %d %llux%ux%u mips %u samples %u flags 0x%x -> %llu B "
                     "(pc %llu) align %llu\n",
                     static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned>(desc_count),
                     static_cast<int>(descs[0].Dimension), static_cast<int>(descs[0].Format),
                     static_cast<unsigned long long>(descs[0].Width), static_cast<unsigned>(descs[0].Height),
                     static_cast<unsigned>(descs[0].DepthOrArraySize), static_cast<unsigned>(descs[0].MipLevels),
                     static_cast<unsigned>(descs[0].SampleDesc.Count), static_cast<unsigned>(descs[0].Flags),
                     static_cast<unsigned long long>(info.SizeInBytes), static_cast<unsigned long long>(pc_size),
                     static_cast<unsigned long long>(info.Alignment));
    *static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(call.ret) = info;
    return call.ret;
}

// Slot 56, ID3D12Device4::GetResourceAllocationInfo1: the same answer, and the per-description offsets follow the
// padded sizes, so a title that places its resources at those offsets does not overlap them. The call is forwarded
// as it came; the console layout then replaces the total and the offsets.
void* WINAPI proxy_get_resource_allocation_info1(std::uint64_t a0, std::uint64_t a1, std::uint64_t visible_mask,
                                                 std::uint64_t count, const D3D12_RESOURCE_DESC* descs,
                                                 D3D12_RESOURCE_ALLOCATION_INFO1* each) {
    const StructReturnCall call = struct_return_call(a0, a1);
    DeviceProxy* const self = call.self;
    note_slot(56);
    if (56 >= self->slot_count) {
        return reinterpret_cast<void*>(hr_to_u64(refuse_beyond_device(56, self->slot_count)));
    }
    using Fn = void*(WINAPI*)(void*, void*, UINT, UINT, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_ALLOCATION_INFO1*);
    const UINT n = static_cast<UINT>(count);
    // The per-resource array is optional (_Out_writes_opt_). The console total must not depend on whether the
    // caller asked for it, so without one the host's per-resource answers go to a local array (found by Alpha325).
    std::vector<D3D12_RESOURCE_ALLOCATION_INFO1> local;
    if (!each && descs && n) {
        try {
            local.resize(n);
            each = local.data();
        } catch (const std::bad_alloc&) {
            each = nullptr;  // the host total stands
        }
    }
    void* const returned = reinterpret_cast<Fn>(self->real_vtable[56])(
        self->real, call.ret, static_cast<UINT>(visible_mask), n, descs, each);
    if (each && call.console_abi) {  // console sizes for the console form only, as slot 25
        auto& info = *static_cast<D3D12_RESOURCE_ALLOCATION_INFO*>(call.ret);
        apply_console_placement(descs, n, info, each, [&](UINT i) {
            return D3D12_RESOURCE_ALLOCATION_INFO{each[i].SizeInBytes, each[i].Alignment};
        }, 56, nullptr);
    }
    return returned;
}

void** get_vtable() {
    static void** vtable = [] {
        static void* storage[kMaxSlots];
        fill_vtable(storage, std::make_index_sequence<kMaxSlots>{});
        storage[25] = reinterpret_cast<void*>(&proxy_get_resource_allocation_info);
        storage[56] = reinterpret_cast<void*>(&proxy_get_resource_allocation_info1);
        return storage;
    }();
    return vtable;
}

bool supports(ID3D12Device* real, REFIID iid) {
    void* found = nullptr;
    const HRESULT hr = real->QueryInterface(iid, &found);
    if (FAILED(hr) || !found) return false;
    static_cast<IUnknown*>(found)->Release();
    return true;
}

// Highest device interface the PC runtime exposes; its cumulative slot count (from d3d12.h) bounds forwarding.
// Windows SDK 10.0.26100 defines ID3D12Device through ID3D12Device14 only; there is no Device15/Device16 header.
unsigned detect_slot_count(ID3D12Device* real) {
#ifdef __ID3D12Device14_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device14))) return 83;
#endif
#ifdef __ID3D12Device13_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device13))) return 82;
#endif
#ifdef __ID3D12Device12_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device12))) return 81;
#endif
#ifdef __ID3D12Device11_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device11))) return 80;
#endif
#ifdef __ID3D12Device10_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device10))) return 79;
#endif
#ifdef __ID3D12Device9_INTERFACE_DEFINED__
    if (supports(real, __uuidof(ID3D12Device9))) return 76;
#endif
    if (supports(real, __uuidof(ID3D12Device8))) return 73;
    if (supports(real, __uuidof(ID3D12Device7))) return 68;
    if (supports(real, __uuidof(ID3D12Device6))) return 66;
    if (supports(real, __uuidof(ID3D12Device5))) return 65;
    if (supports(real, __uuidof(ID3D12Device4))) return 57;
    if (supports(real, __uuidof(ID3D12Device3))) return 51;
    if (supports(real, __uuidof(ID3D12Device2))) return 48;
    if (supports(real, __uuidof(ID3D12Device1))) return 47;
    return 44;
}

}  // namespace

IUnknown* real_object_behind_proxy(const void* object) { return proxy_real_object(object); }

D3D_FEATURE_LEVEL max_feature_level(ID3D12Device* device) {
    if (!device) return D3D_FEATURE_LEVEL(0);
    static const D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                                D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{UINT(std::size(kLevels)), kLevels, D3D_FEATURE_LEVEL(0)};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels, sizeof(levels))))
        return D3D_FEATURE_LEVEL(0);
    return levels.MaxSupportedFeatureLevel;
}

HRESULT create_host_device(HostCreateDeviceFn create, IUnknown* adapter, ID3D12Device** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!create) return E_NOTIMPL;
    ID3D12Device* device = nullptr;
    const HRESULT hr = create(adapter, kHostDeviceFloor, __uuidof(ID3D12Device), reinterpret_cast<void**>(&device));
    if (FAILED(hr) || !device) {
        if (device) device->Release();
        return FAILED(hr) ? hr : E_FAIL;
    }
    const D3D_FEATURE_LEVEL level = max_feature_level(device);
    std::fprintf(stderr, "[rt] d3d12_xs: host device created at minimum feature level 11_0; highest level %u_%u\n",
                 unsigned(level) >> 12, (unsigned(level) >> 8) & 0xf);
    *out = device;
    return S_OK;
}

HRESULT create_device_proxy(ID3D12Device* real, REFIID riid, void** out, bool console_lists) {
    if (!real) return E_POINTER;
    if (!out) return E_POINTER;
    *out = nullptr;

    DeviceProxy* proxy = new (std::nothrow) DeviceProxy;
    if (!proxy) return E_OUTOFMEMORY;
    proxy->vtable = get_vtable();
    proxy->real = real;
    proxy->real_vtable = *reinterpret_cast<void***>(real);
    proxy->slot_count = detect_slot_count(real);
    proxy->console_lists = console_lists;
    proxy->refs.store(1, std::memory_order_relaxed);
    proxy->dxgi_vtable = get_dxgi_vtable();
    proxy->adapter_provider = nullptr;
    proxy->gpu_thread_priority.store(0, std::memory_order_relaxed);
    proxy->max_frame_latency.store(3, std::memory_order_relaxed);

    const unsigned slot_count = proxy->slot_count;
    HRESULT hr = proxy_query_interface(proxy, &riid, out);
    if (FAILED(hr)) {
        // No reference escaped, and the caller still owns its reference to `real`.
        delete proxy;
        return hr;
    }
    // The source consults the process-wide heap registry; it holds no pointer to this device.
    leonida::title_va_aliases().set_descriptor_source(nullptr, pull_title_descriptors);
    if (!d3d12_trace::enabled())
        std::fprintf(stderr, "[rt] d3d12_xs: device proxy %p wraps PC device %p (%u PC vtable slots)\n",
                     static_cast<void*>(proxy), static_cast<void*>(real), slot_count);
    proxy_release(proxy);  // drop the creation reference; `out` holds the caller's reference
    return S_OK;
}

bool is_device_proxy(const void* device) {
    return device && *reinterpret_cast<void* const*>(device) == reinterpret_cast<void*>(get_vtable());
}

void* device_proxy_from_dxgi_device(void* dxgi_device) {
    if (!dxgi_device || *static_cast<void***>(dxgi_device) != get_dxgi_vtable()) return nullptr;
    return dxgi_owner(dxgi_device);
}

void* device_proxy_primary(void* device) {
    if (is_device_proxy(device)) return device;
    return device_proxy_from_dxgi_device(device);
}

bool set_device_adapter_provider(void* device, DxgiAdapterProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        // Not installing one silently would leave the host's adapter in its place.
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_adapter_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->adapter_provider = provider;
    return true;
}

void set_constant_view_provider(void* device, ConsoleConstantView provider) {
    if (auto* proxy=static_cast<DeviceProxy*>(device_proxy_primary(device))) proxy->constant_view=provider;
}
void set_raw_texture_view_provider(void* device, RawTextureViewProvider provider) {
    if (auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device))) proxy->raw_texture_view = provider;
}
void set_texture_lease_provider(void* device, TextureLeaseProvider provider) {
    if (is_device_proxy(device)) static_cast<DeviceProxy*>(device)->texture_lease_provider = provider;
}
void set_descriptor_mapping_provider(void* device, DescriptorMappingProvider provider) {
    if (auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device))) proxy->descriptor_mapping = provider;
}

bool set_device_resource_provider(void* device, ResourceByVaProvider provider) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) {
        std::fprintf(stderr, "[rt] d3d12_xs: set_device_resource_provider: %p is not a device proxy; no provider set\n",
                     device);
        return false;
    }
    proxy->resource_provider = provider;
    return true;
}

void set_native_stream_factory(void* device, NativeStreamFactory factory) {
    if (auto* proxy=static_cast<DeviceProxy*>(device_proxy_primary(device)))proxy->prepare_stream=factory;
}
void set_device_native_graphics_factory(void* device, NativeGraphicsFactory factory) {
    if (auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device))) proxy->native_graphics = factory;
}

bool set_depth_stencil_uav_provider(void* device, DepthStencilUavProvider provider, ValidateDepthPlacement validate) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) return false;
    proxy->depth_stencil_uav_provider = provider;
    proxy->validate_depth_placement = validate;
    return true;
}

bool is_heap_proxy(const void* heap) {
    return heap && *reinterpret_cast<void* const*>(heap) == reinterpret_cast<void*>(get_heap_vtable());
}

ID3D12DescriptorHeap* heap_proxy_real_heap(const void* heap) {
    if (!is_heap_proxy(heap)) return nullptr;
    return static_cast<const HeapProxy*>(heap)->real;
}

void set_descriptor_heap_allocator(DescriptorHeapBackingAlloc alloc, DescriptorHeapBackingFree free) {
    std::lock_guard lock(heap_alloc_mutex());
    g_heap_alloc = alloc;
    g_heap_free = free;
    g_heap_alloc_explicit = (alloc != nullptr || free != nullptr);
}

void set_default_descriptor_heap_allocator(DescriptorHeapBackingAlloc alloc, DescriptorHeapBackingFree free) {
    std::lock_guard lock(heap_alloc_mutex());
    if (g_heap_alloc_explicit) return;
    g_heap_alloc = alloc;
    g_heap_free = free;
}

D3D12_CPU_DESCRIPTOR_HANDLE translate_cpu_descriptor_handle(const void* device, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const auto* proxy = is_device_proxy(device) ? static_cast<const DeviceProxy*>(device) : nullptr;
    return translate_cpu_handle(proxy, handle);
}

unsigned device_proxy_slot_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    return static_cast<const DeviceProxy*>(device)->slot_count;
}

void set_placed_at_va_handler(PlacedAtVaHandler handler) {
    g_placed_at_va.store(handler, std::memory_order_release);
}

bool console_feature_query() noexcept { return g_console_feature_query; }

void set_texture_page_provider(PinnedTexturePages hook) { g_pin_texture_pages.store(hook,std::memory_order_release); }
std::shared_ptr<std::uint8_t> pin_texture_pages(const void* address, std::size_t size) {
    const auto hook=g_pin_texture_pages.load(std::memory_order_acquire);
    return hook ? hook(address,size) : std::shared_ptr<std::uint8_t>{};
}

void set_placed_at_va_pages_hook(PlacedAtVaPagesHook hook) {
    g_placed_at_va_pages.store(hook, std::memory_order_release);
}

void set_copy_page_mappings_hook(CopyPageMappingsHook hook) {
    g_copy_page_mappings.store(hook, std::memory_order_release);
}

void set_console_descriptor_heap_handler(ConsoleDescriptorHeapHandler handler) {
    g_console_descriptor_heap.store(handler, std::memory_order_release);
}

void set_reference_settler(ReferenceSettler settle) {
    g_reference_settler.store(settle, std::memory_order_release);
}

ID3D12Device* device_proxy_real_device(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->real;
}

bool set_presentation_handler(void* device, const PresentationHandler* handler) {
    auto* proxy = static_cast<DeviceProxy*>(device_proxy_primary(device));
    if (!proxy) return false;
    AcquireSRWLockExclusive(&proxy->presentation_lock);
    proxy->external_presentation_mode = true;
    proxy->presentation = handler ? *handler : PresentationHandler{};
    ReleaseSRWLockExclusive(&proxy->presentation_lock);
    return true;
}

std::shared_ptr<leonida::FrameTokenSource> device_frame_tokens(const void* device) {
    if (!is_device_proxy(device)) return nullptr;
    return static_cast<const DeviceProxy*>(device)->frame_tokens;
}

bool device_slot_overridden(unsigned slot) {
    if (slot <= 2 || slot == 8 || slot == 9 || slot == 12 || slot == 13 || slot == 14 || (slot >= 17 && slot <= 27) || slot == 31 ||
        slot == 32 || slot == 34 || slot == 35 || slot == 36 || slot == 41 || slot == 45 || slot == 46 || slot == 50 ||
        slot == 53 || slot == 55 || slot == 56 || slot == 57 || slot == 61 || slot == 62 || slot == 67 || slot == 70 || slot == 74 || slot == 75 ||
        slot == 76 ||
        slot == 86 || slot == 101 || slot == 108 || slot == kCreateCommittedResource3X || slot == 133)
        return true;
    const XboxDeviceSlotRoutes& routes = xbox_device_slot_routes();
    return slot == routes.frame_interval || slot == routes.schedule_event || slot == routes.wait_event ||
           slot == routes.frame_statistics || slot == routes.queue_x || slot == routes.list_x;
}

bool is_queue_proxy(const void* queue) { return is_child_proxy<ChildKind::queue>(queue); }

bool is_list_proxy(const void* list) { return is_list_proxy_impl(list); }

ID3D12CommandList* list_proxy_real_list(const void* list) {
    if (!is_list_proxy_impl(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

const char* list_tail_slot_name(unsigned slot, unsigned char console_layout) {
    return list_tail_name(slot, console_layout);
}

// Every counter of list_values::Counters, by name: the stats and their reset stay in step.
#define LEONIDA_LIST_VALUE_COUNTERS(X)                                                                         \
    X(writes_recorded) X(writes_landed) X(writes_failed) X(waits_recorded) X(waits_met_at_submit)            \
    X(waits_relayed) X(gates_held) X(gates_met) X(gates_timed_out) X(cuts) X(cuts_deferred) X(cut_failures)  \
    X(gates_short) X(demotions) X(late_met) X(bundle_ops) X(releases) X(release_us) X(release_us_max)        \
    X(pass_us_max) X(release_stalls) X(remaps_now) X(remaps_deferred) X(remaps_failed) X(remap_wait_us_max)    \
    X(stamps_gpu) X(stamps_cpu) X(labels_handed) X(writes_arena) X(writes_title_pages)

ListValueStats list_value_stats() {
    const list_values::Counters& c = list_values::counters();
    ListValueStats stats{};
#define LEONIDA_GET(name) stats.name = c.name.load(std::memory_order_relaxed);
    LEONIDA_LIST_VALUE_COUNTERS(LEONIDA_GET)
#undef LEONIDA_GET
    return stats;
}

void reset_list_value_stats() {
    list_values::Counters& c = list_values::counters();
#define LEONIDA_RESET(name) c.name.store(0, std::memory_order_relaxed);
    LEONIDA_LIST_VALUE_COUNTERS(LEONIDA_RESET)
#undef LEONIDA_RESET
}
#undef LEONIDA_LIST_VALUE_COUNTERS

void set_list_value_wait_timeout_ms(unsigned ms) { list_values::Pump::get().set_timeout_ms(ms); }

void set_list_values_enabled(bool on) { list_values::set_enabled(on); }

void stop_list_values(unsigned ms) { list_values::Pump::get().stop(ms); }

bool list_values_idle(unsigned ms) { return list_values::Pump::get().idle(ms); }

unsigned list_proxy_part_count(const void* list) {
    if (!is_list_proxy_impl(list)) return 0;
    return static_cast<unsigned>(static_cast<const ListProxy*>(list)->parts.size()) + 1;
}
bool is_fence_proxy(const void* fence) { return is_child_proxy<ChildKind::fence>(fence); }
bool is_allocator_proxy(const void* allocator) { return is_child_proxy<ChildKind::allocator>(allocator); }

ID3D12CommandAllocator* allocator_proxy_real_allocator(const void* allocator) {
    if (!is_allocator_proxy(allocator)) return nullptr;
    return reinterpret_cast<ID3D12CommandAllocator*>(static_cast<const ChildProxy*>(allocator)->real);
}

ID3D12CommandQueue* queue_proxy_real_queue(const void* queue) {
    if (!is_queue_proxy(queue)) return nullptr;
    return reinterpret_cast<ID3D12CommandQueue*>(static_cast<const ChildProxy*>(queue)->real);
}

ID3D12Fence* fence_proxy_real_fence(const void* fence) {
    if (!is_fence_proxy(fence)) return nullptr;
    return reinterpret_cast<ID3D12Fence*>(static_cast<const ChildProxy*>(fence)->real);
}

unsigned queue_proxy_slot_count(const void* queue) {
    return is_queue_proxy(queue) ? static_cast<const ChildProxy*>(queue)->slot_count : 0;
}

unsigned fence_proxy_slot_count(const void* fence) {
    return is_fence_proxy(fence) ? static_cast<const ChildProxy*>(fence)->slot_count : 0;
}

unsigned queue_proxy_table_size() { return kQueueSlots; }
unsigned fence_proxy_table_size() { return kFenceSlots; }

bool queue_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == kQueueSignal || slot == kQueueWait ||
           slot == kQueueExecuteCommandListsX;
}
unsigned allocator_proxy_table_size() { return kAllocatorSlots; }
unsigned allocator_proxy_slot_count(const void* allocator) {
    return is_allocator_proxy(allocator) ? static_cast<const ChildProxy*>(allocator)->slot_count : 0;
}

std::uintptr_t allocator_proxy_callback(const void* allocator) {
    return is_allocator_proxy(allocator)
               ? static_cast<const ChildProxy*>(allocator)->allocator_callback.load(std::memory_order_relaxed)
               : 0;
}

std::uintptr_t allocator_proxy_callback_context(const void* allocator) {
    return is_allocator_proxy(allocator)
               ? static_cast<const ChildProxy*>(allocator)->allocator_callback_context.load(std::memory_order_relaxed)
               : 0;
}

unsigned long command_object_proxy_ref_count(const void* object) {
    if (!is_queue_proxy(object) && !is_fence_proxy(object) && !is_allocator_proxy(object)) return 0;
    return static_cast<const ChildProxy*>(object)->refs.load(std::memory_order_relaxed);
}

std::uint64_t queue_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool queue_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::queue>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t fence_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool fence_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::fence>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t queue_table_overruns() {
    return ChildStats<ChildKind::queue>::overruns.load(std::memory_order_relaxed);
}

std::uint64_t fence_table_overruns() {
    return ChildStats<ChildKind::fence>::overruns.load(std::memory_order_relaxed);
}

bool command_object_tables_guarded() {
    get_child_vtable<ChildKind::queue>();
    get_child_vtable<ChildKind::fence>();
    get_child_vtable<ChildKind::allocator>();
    return ChildStats<ChildKind::queue>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::fence>::table_end.load(std::memory_order_acquire) &&
           ChildStats<ChildKind::allocator>::table_end.load(std::memory_order_acquire);
}

std::uint64_t allocator_slot_hits(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    start_hit_counts();
    return slot < S::kSlots ? S::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool allocator_slot_first_call_seen(unsigned slot) {
    using S = ChildStats<ChildKind::allocator>;
    return slot < S::kSlots && S::first[slot].load(std::memory_order_relaxed);
}

std::uint64_t allocator_table_overruns() {
    return ChildStats<ChildKind::allocator>::overruns.load(std::memory_order_relaxed);
}

bool heap_slot_overridden(unsigned slot) {
    return slot <= 2 || slot == 7 || slot == 9 || slot == 10;
}

void set_console_caller(ConsoleCaller classify) {
    g_console_caller.store(classify, std::memory_order_release);
}

std::uint64_t device_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kMaxSlots ? g_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool device_slot_first_call_seen(unsigned slot) {
    return slot < kMaxSlots && g_first[slot].load(std::memory_order_relaxed);
}

unsigned long device_proxy_ref_count(const void* device) {
    if (!is_device_proxy(device)) return 0;
    if (const ReferenceSettler settle = g_reference_settler.load(std::memory_order_acquire)) settle();
    return static_cast<const DeviceProxy*>(device)->refs.load(std::memory_order_relaxed);
}

std::uint64_t heap_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kHeapSlots ? g_heap_hits[slot].load(std::memory_order_relaxed) : 0;
}

bool heap_slot_first_call_seen(unsigned slot) {
    return slot < kHeapSlots && g_heap_first[slot].load(std::memory_order_relaxed);
}

bool is_command_list_proxy(const void* list) {
    return list && *reinterpret_cast<void* const*>(list) == reinterpret_cast<void*>(get_list_vtable());
}

ID3D12GraphicsCommandList* command_list_proxy_real_list(const void* list) {
    if (!is_command_list_proxy(list)) return nullptr;
    return static_cast<const ListProxy*>(list)->real;
}

unsigned command_list_proxy_table_size() { return kListSlots; }

std::uint64_t list_slot_hits(unsigned slot) {
    start_hit_counts();
    return slot < kListSlots ? ListStats::hits[slot].load(std::memory_order_relaxed) : 0;
}

bool list_slot_first_call_seen(unsigned slot) {
    return slot < kListSlots && ListStats::first[slot].load(std::memory_order_relaxed);
}

void reset_device_proxy_stats() {
    start_hit_counts();
    for (unsigned slot = 0; slot < kMaxSlots; ++slot) {
        g_hits[slot].store(0, std::memory_order_relaxed);
        g_first[slot].store(false, std::memory_order_relaxed);
        g_beyond_warned[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kHeapSlots; ++slot) {
        g_heap_hits[slot].store(0, std::memory_order_relaxed);
        g_heap_first[slot].store(false, std::memory_order_relaxed);
    }
    for (unsigned slot = 0; slot < kListSlots; ++slot) {
        ListStats::hits[slot].store(0, std::memory_order_relaxed);
        ListStats::first[slot].store(false, std::memory_order_relaxed);
    }
    reset_child_stats<ChildKind::queue>();
    reset_child_stats<ChildKind::fence>();
    reset_child_stats<ChildKind::allocator>();
}

bool parse_xbox_pipeline_blob_input_layout(const void* blob, std::size_t blob_size,
                                           std::vector<XboxPipelineInputElement>& out_elements) {
    return parse_xbox_pipeline_blob_input_layout_impl(blob, blob_size, out_elements);
}

std::string lookup_xbox_fspx_program_stage(const void* program_bytes, std::size_t program_size) {
    return lookup_xbox_fspx_program_stage_impl(program_bytes, program_size);
}

}  // namespace rt::d3d12_xs
