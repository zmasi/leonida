// D3D12 COM objects over the Vulkan backend (src/vulkan_backend.h): command allocator, graphics command list,
// command queue and fence. Their vtables are the Windows SDK's (ID3D12GraphicsCommandList8, ID3D12CommandQueue,
// ID3D12CommandAllocator, ID3D12Fence1); every method not implemented here is a generated stub that reports its
// name once (tools/gen_d3d12_stubs.py). The Xbox-specific slots measured for the device do not apply to these
// interfaces as far as is known; docs/d3d12_vulkan.md section 3.1 lists what is still unverified.
//
// Contracts with the rest of the layer (the device proxy that will hand these objects out):
// - a resource's ID is its backing pointer value (identical Xbox depth placements share storage and layouts);
// - so is an ID3D12QueryHeap's leonida::QueryHeapId, registered with its VkQueryPool;
// - the CPU handle of an RTV or DSV points at a leonida::TargetView in the device's descriptor heap.
#pragma once

#include <cstdlib>

#include <windows.h>
#include <d3d12.h>
#include <array>
#include <map>
#include <memory>
#include <vector>

#include "d3d12_xs_device.h"
#include "../src/gfx10_descriptors.h"
#include "../src/vulkan_backend.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "../src/root_signature.h"
#include "../src/subgroup_size.h"
#include "../src/vulkan_memory_budget.h"

namespace leonida {
class VulkanPresenter;
struct PresentPlanes;
}

namespace leonida::label { struct Write; }
namespace rt::d3d12 {
bool record_gpu_label(ID3D12GraphicsCommandList* list, const leonida::label::Write& write);

// How a queue answers GetClockCalibration. With `calibrated` (vkGetCalibratedTimestampsKHR or its EXT alias, on a
// device whose time domains include DEVICE and QUERY_PERFORMANCE_COUNTER) both clocks are read in one call. Without
// it, the queue writes a timestamp query and reads QueryPerformanceCounter around it; that needs `get_device_proc`
// and a queue family with timestamp_valid_bits > 0. With neither, GetClockCalibration fails.
struct QueueClock {
    PFN_vkGetCalibratedTimestampsKHR calibrated = nullptr;
    PFN_vkGetDeviceProcAddr get_device_proc = nullptr;
    std::uint32_t timestamp_valid_bits = 0;
};

// `device` (optional) is what the object's GetDevice returns; the object holds a reference to it.
HRESULT create_command_queue(leonida::VulkanContext& context, VkQueue queue, std::uint32_t queue_family,
                             const D3D12_COMMAND_QUEUE_DESC& desc, REFIID riid, void** out,
                             IUnknown* device = nullptr, const QueueClock& clock = {},
                             std::shared_ptr<void> queue_lease = {});
HRESULT create_command_allocator(D3D12_COMMAND_LIST_TYPE type, REFIID riid, void** out, IUnknown* device = nullptr);
HRESULT create_command_list(D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator, REFIID riid,
                            void** out, IUnknown* device = nullptr, ID3D12PipelineState* initial_state = nullptr);
HRESULT create_fence(leonida::VulkanContext& context, UINT64 initial_value, D3D12_FENCE_FLAGS flags, REFIID riid,
                     void** out, IUnknown* device = nullptr);
HRESULT create_command_signature(leonida::IndirectKind kind, UINT byte_stride, UINT packed_size, REFIID riid, void** out,
                                 IUnknown* device, std::span<const D3D12_INDIRECT_ARGUMENT_DESC> prefix);
bool command_signature_info(ID3D12CommandSignature* signature, leonida::IndirectKind& kind, UINT& byte_stride,
                            bool& terminal_only);

// ID3D12Device1::SetEventOnMultipleFenceCompletion on fences from create_fence: sets `event` once every fence
// (D3D12_MULTIPLE_FENCE_WAIT_FLAG_ALL) or any fence (_ANY) has reached its value; a null event blocks until then.
HRESULT set_event_on_multiple_fence_completion(leonida::VulkanContext& context, ID3D12Fence* const* fences,
                                               const UINT64* values, UINT count,
                                               D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event,
                                               IUnknown* device = nullptr);

std::shared_ptr<const leonida::TileHeapAllocation> tile_heap(ID3D12Heap* heap, const leonida::VulkanContext& context,
                                                           bool image, std::uint32_t memory_types);
bool is_sparse_image(ID3D12Resource* resource);

// Colour image used for unordered accesses to an Xbox single-plane depth surface.
struct XboxDepthStorage {
    leonida::ResourceId colour = 0;
    UINT mips = 0;
    UINT layers = 0;
};
XboxDepthStorage xbox_depth_storage(ID3D12Resource* resource);

// The Vulkan objects a D3D12 device runs on. Vulkan 1.3 with dynamicRendering, synchronization2, timelineSemaphore
// and (when separate_depth_stencil_layouts is set) separateDepthStencilLayouts enabled.
struct VulkanDeviceInfo {
    PFN_vkGetInstanceProcAddr get_instance_proc = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;  // graphics queue used by staging and legacy single-queue callers
    std::uint32_t queue_family = 0;
    // Distinct handles from queue_family, when the creator requested more than one queue.
    // Empty for caller-owned devices that only supplied queue above.
    std::vector<VkQueue> command_queues;
    std::uint32_t compute_family = UINT32_MAX;
    std::uint32_t copy_family = UINT32_MAX;
    std::vector<VkQueue> compute_queues;
    std::vector<VkQueue> copy_queues;
    bool pipeline_creation_feedback = false;
    // Vulkan 1.3 pipelineCreationCacheControl, enabled: FAIL_ON_PIPELINE_COMPILE_REQUIRED asks a cache without a compile.
    // Implies core VkPipelineCreationFeedback, which then needs no extension.
    bool pipeline_cache_control = false;
    bool device_fault_diagnostics = false;
    bool device_fault_info = false;
    bool device_checkpoints = false;
    bool device_diagnostics_config = false;
    bool device_markers = false;  // VK_AMD_buffer_marker enabled for LEONIDA_DEVICE_MARKERS=1
    bool separate_depth_stencil_layouts = true;
    bool depth_range_unrestricted = false;  // VK_EXT_depth_range_unrestricted enabled on this device.
    std::uint32_t max_push_constants_size = 0;
    bool push_constants_need_fallback = false;
    // Top location for relocated f16 halves (0: read from device limits; non-zero: override for testing low-limit devices).
    std::uint32_t f16_half_top_location = 0;
    // maxBufferSize and maxMemoryAllocationSize, which cap the GPU address arena (src/arena_size.h). 0: read from
    // the physical device when the D3D12 device is created; a test sets smaller ones to simulate a device.
    VkDeviceSize max_buffer_size = 0;
    VkDeviceSize max_memory_allocation_size = 0;
    // The subgroup sizes the device offers and whether subgroupSizeControl / computeFullSubgroups are enabled on it
    // (src/subgroup_size.h). Default: nothing, so pipelines request no size.
    leonida::SubgroupSizeCaps subgroup;
    // Enabled: runtimeDescriptorArray, the dynamic indexing of sampled- and storage-image arrays, and update after
    // bind for sampled images, storage images and storage buffers. The device then keeps the bindless set 0 of
    // the translated shaders (docs section 3.4), and texture views and samplers are put in it as they are
    // created. bindless_partially_bound: descriptorBindingPartiallyBound is enabled as well.
    bool bindless = false;
    bool bindless_partially_bound = false;
    // samplerAnisotropy, samplerMirrorClampToEdge and samplerFilterMinmax enabled; without them such samplers are
    // approximated.
    bool sampler_anisotropy = false;
    bool sampler_mirror_clamp_to_edge = false;
    bool sampler_filter_minmax = false;
    // VK_KHR_calibrated_timestamps or VK_EXT_calibrated_timestamps is enabled on `device` (QueueClock).
    bool calibrated_timestamps = false;
    // VK_EXT_conditional_rendering (with its conditionalRendering feature) is enabled: SetPredication works, and
    // buffers are created with VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT. Without it a predicated list is
    // rejected at ExecuteCommandLists.
    bool conditional_rendering = false;
    bool storage_buffer_8bit = false; // storageBuffer8BitAccess enabled: byte-granular predicated buffer copies.
    // VK_EXT_conservative_rasterization is enabled on the logical device. Tier 1/2 depend on the physical
    // precision/overestimation properties; Tier 2 also requires post-snap degenerate triangles.
    bool conservative_rasterization = false;
    // Opt in only for shader libraries whose InnerCoverage input contract uses FullyCoveredEXT. The device
    // still checks physical Tier 2 and fullyCoveredFragmentShaderInputVariable properties. The standalone
    // device leaves this false until the Xbox PS input mapping is confirmed (synthetic GLSL is not that ABI).
    bool inner_coverage_abi = false;
    // VK_EXT_host_image_copy and its hostImageCopy feature are enabled on `device`: WriteToSubresource and
    // ReadFromSubresource on textures in CPU-visible heaps copy on the CPU instead of through a staging submit.
    bool host_image_copy = false;
    // Tests only: every host-transfer texture is treated as one the driver gave no memory type (a GTX 1050 Ti,
    // NVIDIA 582.78), so it is made again without host-transfer usage and copies on the staging path on any GPU.
    bool force_host_transfer_retry = false;
    // VK_KHR_shader_fma and its shaderFmaFloat32 feature are enabled on `device`: translated shaders built with
    // `rdna2spirv.py --fma khr` (OpFmaKHR, capability FMAKHR in the index) can be created. Without it only the
    // default `--fma glsl` shaders can.
    bool shader_fma = false;
    bool shader_subgroup_clock = false;
    bool shader_device_clock = false;
    // The caller enabled textureCompressionBC on this logical device.
    bool texture_compression_bc = false;
    // Optional device features the caller enabled. Without occlusionQueryPrecise, D3D12_QUERY_TYPE_OCCLUSION counts
    // are only zero or non-zero; without pipelineStatisticsQuery, pipeline-statistics query heaps are refused.
    bool occlusion_query_precise = false;
    bool prepare_on_bind = false;
    bool cold_graphics_compile = false;  // Driver supports a cache-only create and reports when compilation is pending.
    // pipelineCreationCacheControl is on for the heavy pipeline cache (src/heavy_pipeline_cache.h): a pipeline the
    // offline cache does not have is created against the cache of pipeline_cache.heavy.bin, written back as it grows.
    bool heavy_pipeline_cache = false;
    bool pipeline_statistics_query = false;
    bool presentation = false;  // VK_KHR_surface + VK_KHR_win32_surface on the instance, VK_KHR_swapchain on the device
    // VK_KHR_swapchain_maintenance1 (or VK_EXT_swapchain_maintenance1) and its swapchainMaintenance1 feature are
    // enabled on the device, with VK_KHR_get_surface_capabilities2 and the matching *_surface_maintenance1 on the
    // instance (only meaningful with `presentation`): a vsync swapchain can switch to IMMEDIATE or FIFO_RELAXED per
    // present, which PresentX's ImmediateThresholdPercent uses.
    bool swapchain_maintenance1 = false;
    // VK_EXT_swapchain_colorspace is enabled on the instance (with `presentation`): the surface can list its HDR10
    // (ST.2084) formats, which a PresentX plane in HDR10 uses. Without it HDR10 planes are tone mapped to SDR.
    bool swapchain_colorspace = false;
    // VK_EXT_image_view_min_lod and its minLod feature are enabled on the logical device.
    bool image_view_min_lod = false;
    // VK_EXT_robustness2 with nullDescriptor is enabled on `device`: set 0's storage-array fallbacks are null
    // descriptors, so stores and atomics through a null UAV are dropped (src/vulkan_bindless.h). Without it they are
    // 1x1 images of their own, apart from the sampled fallbacks the null SRVs read.
    bool null_descriptor = false;
    // VK_KHR_vertex_attribute_divisor or VK_EXT_vertex_attribute_divisor with vertexAttributeInstanceRateDivisor.
    bool vertex_attribute_divisor = false;
    // Pixel-shader extensions translated shaders can declare, each enabled with its feature where the GPU has it:
    // VK_KHR_fragment_shader_barycentric (BaryCoordKHR / PerVertexKHR reads), VK_EXT_fragment_shader_interlock
    // (fragmentShaderPixelInterlock: POPS shaders run in PixelInterlockOrderedEXT) and VK_KHR_maintenance8 (texel
    // offsets outside gathers: the index's needs_maintenance8).
    bool fragment_shader_barycentric = false;
    bool fragment_shader_interlock = false;
    bool maintenance8 = false;
    // VK_KHR_maintenance5, enabled where its VK_FORMAT_A8_UNORM_KHR can be sampled and is a colour attachment the GPU
    // can blend: A8_UNORM render targets are then stored in that format (other A8_UNORM textures stay R8_UNORM).
    bool a8_render_target = false;
    bool maintenance10 = false;  // VK_KHR_maintenance10 enabled: query depth/stencil copy support per format
    // The instance enabled VK_EXT_debug_utils: SetMarker/BeginEvent/EndEvent become debug labels. Otherwise they
    // do nothing.
    bool debug_utils = false;
    // Vulkan 1.2 bufferDeviceAddress is enabled: buffers and shader-visible heaps then live in the GPU address
    // arena and have console VAs (docs section 3.3), and GetGPUVirtualAddress returns those addresses; ray tracing
    // needs it too. Without it, GetGPUVirtualAddress returns 0 and pipelines cannot be created.
    bool buffer_device_address = false;
    // Optional VK_KHR_acceleration_structure + VK_KHR_deferred_host_operations + VK_KHR_ray_query are enabled:
    // GetRaytracingAccelerationStructurePrebuildInfo and BuildRaytracingAccelerationStructure (BLAS + TLAS) work.
    bool ray_tracing = false;
    // robustImageAccess is enabled on `device`, so typed 1D-family and 3D UAV loads are range checked.
    bool robust_image_access = false;
    // VK_EXT_image_sliced_view_of_3d (imageSlicedViewOf3D) is also enabled for partial 3D UAVs.
    bool sliced_3d_uav = false;
    // BuildRaytracingAccelerationStructure writes the console's XDXR BVH4 records into the destination buffer
    // (src/xdxr_build.h), the layout the game's translated traversal shaders read, instead of building a
    // VkAccelerationStructureKHR; needs buffer_device_address and the GPU address arena. The standalone device sets it
    // unless LEONIDA_RT_AS=khr (docs/d3d12_vulkan.md 4.8); with it set, ray_tracing is not enabled.
    bool xdxr_acceleration_structures = false;
    // VK_EXT_mesh_shader with its meshShader feature is enabled on `device` (taskShader is not needed: the game's
    // shaders have no amplification stage). Mesh-shader PSOs and DispatchMesh work, and CheckFeatureSupport reports
    // D3D12_MESH_SHADER_TIER_1; without it mesh PSOs are refused and the tier is NOT_SUPPORTED.
    bool mesh_shader = false;
    // VK_EXT_mesh_shader's meshShaderQueries feature is enabled on `device`, so the clipping invocations of a mesh
    // segment come from a mesh-primitives query (D3D-MESH-CLIPPING-COUNTER). Without it they read 0.
    bool mesh_shader_queries = false;
    // VK_EXT_transform_feedback with its transformFeedback feature is enabled on `device` (D3D-302): an
    // SO_STATISTICS heap is a real VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT pool, begun and ended indexed on the
    // stream. StreamOutput PSOs are still refused and SOSetTargets is still not implemented.
    bool transform_feedback = false;
    // VK_EXT_transform_feedback's geometryStreams feature is offered by the device. The device-enable chain requests
    // it when present so the pipeline cache key matches vk_pipeline_check; nothing rasterizes a stream other than 0.
    bool transform_feedback_geometry_streams = false;
    // VK_EXT_transform_feedback's transformFeedbackQueries property (D3D-302): the device can begin/end a
    // VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT query. Without it an SO_STATISTICS heap is made without a pool and
    // every SO_STATISTICS query resolves zero, even when the transformFeedback feature is enabled.
    bool transform_feedback_queries = false;
    // The device's maxTransformFeedbackStreams property: an SO_STATISTICS_STREAMn query runs only for n below it.
    // Streams from this count up (to the D3D12 maximum of four) run nothing and resolve zero.
    std::uint32_t max_transform_feedback_streams = 0;
    // The core features enabled on `device`. Graphics PSOs that need independentBlend, fillModeNonSolid,
    // depthBiasClamp or dualSrcBlend are refused unless it is set here.
    VkPhysicalDeviceFeatures features{};
    // Vulkan 1.2 shaderOutputLayer / shaderOutputViewportIndex enabled on `device`: a translated vertex shader that
    // writes the layer or viewport declares ShaderLayer / ShaderViewportIndex (the index's `features`).
    bool shader_output_layer = false;
    bool shader_output_viewport_index = false;
    // The translator's output folder for pipeline creation. An empty value uses LEONIDA_SHADERS or the packaged path.
    std::string shader_dir;
    // VK_EXT_custom_border_color (with customBorderColors and customBorderColorWithoutFormat) is enabled on
    // `device`: samplers with custom border colours pass them through to Vulkan. Without it, custom border
    // colours fall back to transparent black.
    bool custom_border_color = false;
    bool custom_border_color_without_format = false;
    // VK_EXT_external_memory_host is enabled on `device` when this is non-zero (its
    // minImportedHostPointerAlignment): OpenExistingHeapFromAddress backs a heap with the caller's own committed
    // memory. Zero: existing heaps stay unsupported.
    VkDeviceSize host_pointer_alignment = 0;
    // Must describe the feature enabled on this logical device, not merely advertised by the GPU.
    bool draw_indirect_first_instance = false;
    bool multi_draw_indirect = false;
    bool draw_indirect_count_enabled = false;
    // Both VK_KHR_maintenance5 and robustBufferAccess2 (with core robustBufferAccess) are enabled.
    bool robust_index_view = false;
    // VK_EXT_memory_budget is enabled on `device`: vulkan_vram_budget reads the GPU memory in use and the budget
    // from it (the title HUD's VRAM figure). Without it that figure is not available.
    bool memory_budget = false;
    std::uint32_t max_draw_indirect_count = 1;
};

// A Vulkan-backed ID3D12Device (PC ABI, up to ID3D12Device11) on caller-owned Vulkan objects, which must outlive
// the device and every object it created. Wrap it in rt::d3d12_xs::create_device_proxy for the Xbox ABI.
HRESULT create_vulkan_device(const VulkanDeviceInfo& info, REFIID riid, void** out);

// The folder of translated shaders a device loads when VulkanDeviceInfo::shader_dir is empty: LEONIDA_SHADERS, else
// the path make_game recorded in shader_path.txt next to the executable (`config` names another file, for tests).
// Empty when neither names one, or when the path cannot be converted (a shader_path.txt that is not UTF-8); the reason
// is in `*error` then. Never throws.
std::filesystem::path configured_shader_directory(std::string* error, const std::filesystem::path& config = {});

// The same, on a Vulkan instance and device of its own (destroyed with the D3D12 device). Picks the first discrete
// GPU with the features above. LEONIDA_VK_VALIDATION=1 enables VK_LAYER_KHRONOS_validation.
//
// The queue test settings (LEONIDA_VK_MAX_QUEUES_PER_FAMILY, LEONIDA_VK_GRAPHICS_FAMILY_ONLY; docs/testing.md) give
// the device fewer Vulkan queues than the GPU has, so the tests run the path where D3D12 queues share one. Only the
// tests apply them: the game's device (d3d12_xs, LEONIDA_D3D12=vulkan) passes `ignore`, so a variable left set does
// not serialize the game's queues; it says once that it ignores them.
enum class QueueTestSettings { apply, ignore };
HRESULT create_vulkan_device_standalone(REFIID riid, void** out,
                                        QueueTestSettings queue_test_settings = QueueTestSettings::apply);

// Tells a device from create_vulkan_device* that `outer` (the Xbox-ABI proxy that owns it) is what its children's
// GetDevice must return. No reference is taken: the outer object owns the device. `outer` may be the proxy's
// IDXGIDevice1 pointer (what create_device_proxy returns for that IID); the proxy itself becomes the outer. For a
// proxy it also makes the proxy's IDXGIDevice1::GetAdapter return this layer's adapter (d3d12_xs_device.h).
void set_device_outer(ID3D12Device* device, IUnknown* outer);

// Console Xbox CreateDescriptorHeap takes a 16-byte PC D3D12_DESCRIPTOR_HEAP_DESC followed by address/size at
// +0x10/+0x18 (issue #313 comment 7107265058). This helper creates a heap with that console ABI contract on a
// device from create_vulkan_device*; it is intended for the Xbox device-proxy slot 14 path, not normal PC callers.
HRESULT create_console_descriptor_heap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                                       D3D12_GPU_VIRTUAL_ADDRESS console_address, std::uint64_t console_size,
                                       REFIID riid, void** out);

// Calls `fn` on the device's fence-wait thread once `fence` (a fence of such a device) reaches `value` (at once on that
// thread if it has). Released or device lost, `fn` still runs once: it must not use the fence then. False for a fence
// of another kind. For the console fence's slot-12 mirror (d3d12_xs_device.cpp).
bool fence_callback_at(ID3D12Fence* fence, std::uint64_t value, std::function<void()> fn);

// The VkImage behind a texture created by such a device, or VK_NULL_HANDLE. For tests and the swapchain.
VkImage vulkan_image(ID3D12Resource* resource);
// Resolves the resource that a UAV clear writes. Xbox depth/stencil UAVs name one of two color images by T#.
leonida::ResourceId uav_image_resource(ID3D12Resource* resource, std::uint64_t address);
// The VkBuffer behind a buffer created by such a device, or VK_NULL_HANDLE. For tests.
VkBuffer vulkan_buffer(ID3D12Resource* resource);
// What a device from create_vulkan_device* was created with (enabled features included), or null. For tests.
const VulkanDeviceInfo* vulkan_device_info(ID3D12Device* device);
// The GPU memory a device from create_vulkan_device* (or its Xbox-ABI proxy) uses and may use, from
// VK_EXT_memory_budget, read now (src/vulkan_memory_budget.h). Not available without the extension or the device.
// Any thread: the title HUD calls it from its own, at most once per refresh.
leonida::VramBudget vulkan_vram_budget(ID3D12Device* device);
// The device's pipeline workers (src/pipeline_workers.h) and what they have compiled; zeros without any. For tests.
leonida::PipelineWorkers::Stats pipeline_worker_stats(ID3D12Device* device);
// True when a slot-133 buffer is a shadow copied from the title's pages, false when they were imported. For tests.
bool title_shadow_buffer(ID3D12Resource* resource);
// Slot-133 imports (VK_EXT_external_memory_host) whose memory `device` holds: an import declined after its memory
// was allocated is freed before create_buffer_at_host_address returns. For tests.
std::uint64_t title_host_imports(ID3D12Device* device);
// VirtualQuery calls made by the title-buffer shadows in their latest refresh. For tests.
std::uint64_t title_shadow_refresh_queries(ID3D12Device* device);
// The device's title windows now: how many and their bytes, how many were retired with their pages, and how many of
// those still wait for the GPU before their memory is freed. For tests.
struct TitleWindows {
    std::uint64_t count = 0, bytes = 0, retired = 0, waiting = 0;
};
TitleWindows title_windows(ID3D12Device* device);
// The trackers behind the title-buffer shadows and title windows (copies whose pages overlap or touch share one).
// For tests.
std::uint64_t title_shadow_trackers(ID3D12Device* device);
// What the title-buffer shadows' latest refresh did, summed over their trackers: GetWriteWatch calls, write-watched
// pages found written, the write faults taken since they were made, and the bytes of covered pages write-watched and
// hot (asked about rather than guarded). For tests.
struct TitleShadowWatch {
    std::uint64_t asks = 0, written = 0, faults = 0, watched_bytes = 0, hot_bytes = 0;
};
TitleShadowWatch title_shadow_watch(ID3D12Device* device);
// True when `device` has VK_KHR_acceleration_structure + VK_KHR_ray_query enabled.
bool device_supports_ray_tracing(ID3D12Device* device);

// Xbox device slot 133 (the console's CreatePlacedResourceX): a buffer on the caller's own pages at `address`, which
// becomes its console VA. The pages are imported with VK_EXT_external_memory_host, so what the CPU writes there is
// what the GPU reads. A buffer description only; E_NOTIMPL on a device that is not the Vulkan layer, without the
// extension, or for a texture.
HRESULT create_buffer_at_host_address(ID3D12Device* device, void* address, const D3D12_RESOURCE_DESC& desc,
                                      std::uint32_t layout, REFIID riid, void** out, const D3D12_CLEAR_VALUE* clear = nullptr);
// The VkAccelerationStructureKHR registered for `va` on `device`, or VK_NULL_HANDLE. For tests.
VkAccelerationStructureKHR vulkan_acceleration_structure(ID3D12Device* device, D3D12_GPU_VIRTUAL_ADDRESS va);

// The property flags of the Vulkan memory type behind a resource created by such a device, or 0. For tests.
VkMemoryPropertyFlags vulkan_memory_flags(ID3D12Resource* resource);

// Whether the requested colour format has an integer numeric class.
bool color_resolve_format_is_integer(DXGI_FORMAT format);

// Whether both resources use the requested typed, non-integer colour format for a regional average resolve.
bool color_resolve_format_matches(DXGI_FORMAT format, ID3D12Resource* dst, ID3D12Resource* src);

// Whether a resolve of one depth or stencil plane can be replayed: both resources are depth-stencil textures of
// `format` (a depth format, or a plane's shader-resource format such as R24_UNORM_X8_TYPELESS), the source
// multisampled and the destination not, the same plane on both sides, MIN or MAX (or AVERAGE of the depth plane by
// its own format), the destination offset the source rectangle's corner (or 0, 0 without one), and the Vulkan device
// has the mode.
bool depth_stencil_resolve_supported(DXGI_FORMAT format, ID3D12Resource* dst, UINT dst_subresource, UINT x, UINT y,
                                     ID3D12Resource* src, UINT src_subresource, const D3D12_RECT* rect,
                                     D3D12_RESOLVE_MODE mode);

// The maxPushConstantsSize in bytes of the Vulkan physical device behind `device`, or 0 if not a Vulkan device.
std::uint32_t device_max_push_constants_size(ID3D12Device* device);

// True when `device`'s Vulkan physical device has maxPushConstantsSize < 144 bytes, requiring
// the uniform-buffer fallback for the 16-byte tail (heap/code base).
bool device_push_constants_need_fallback(ID3D12Device* device);

// True when `device` puts upload-heap buffers in DEVICE_LOCAL | HOST_VISIBLE memory: its GPU has Resizable BAR and
// LEONIDA_UPLOAD_SYSMEM is not set (D3D-96).
bool device_upload_in_vram(ID3D12Device* device);

// True when the device can map reserved buffers onto its D3D12 heaps.
bool device_sparse_buffer_tiles(ID3D12Device* device);

// The size in bytes of the device's GPU address arena, or 0 without one. For tests.
std::uint64_t device_arena_size(ID3D12Device* device);
// Copies `bytes` of the device's GPU address arena at console VA `va` (what shaders read there: a heap's GPU copy,
// the null slots after it) into `out`. False without an arena or when the range is not inside it. For tests.
bool read_arena(ID3D12Device* device, std::uint64_t va, void* out, std::size_t bytes);

// The blocks of device memory that textures share (src/image_memory.h): how many, their bytes, and the textures in
// them. For tests.
struct ImageMemoryStats {
    std::uint64_t blocks = 0;
    std::uint64_t block_bytes = 0;
    std::uint64_t textures = 0;
};
ImageMemoryStats image_memory_stats(ID3D12Device* device);

// The bindless set 0 a device with VulkanDeviceInfo::bindless keeps (vulkan_bindless.h), and its layout: what a
// pipeline whose shader declares set-0 bindings is created with and binds. Null without such a set. The set is
// brought up to date at every ExecuteCommandLists.
VkDescriptorSet bindless_set(ID3D12Device* device);
// What the bindless set did without waiting (D3D-314): table generations made because a flush grew the table or
// changed live entries, the retired ones not freed yet, and vkDeviceWaitIdle calls made for it (always 0 unless
// LEONIDA_BINDLESS_IDLE_WAIT=1 restores the old wait, to compare frame times). All 0 without a bindless set.
struct BindlessStats {
    std::uint64_t generations_made = 0;
    std::uint64_t retired_pending = 0;
    std::uint64_t device_idle_waits = 0;
};
BindlessStats bindless_stats(ID3D12Device* device);
VkDescriptorSetLayout bindless_set_layout(ID3D12Device* device);
// The bindless index the descriptor hash table holds for the 8 key dwords of a T# (or the 4 of an S# followed by
// 0xFFFFFFFF x4), 0 (the fallback) when it has none or `device` has no such set. For tests.
std::uint32_t bindless_hash_lookup(ID3D12Device* device, const std::uint32_t (&key)[8]);
// The free indices of that set's 2D image and sampler arrays: how many more distinct 2D T#s and S#s
// CreateShaderResourceView and CreateSampler can register before they log that the array is full. Zeros when
// `device` has no such set. For tests.
struct BindlessRoom {
    std::uint32_t images_2d = 0;
    std::uint32_t samplers = 0;
};
BindlessRoom bindless_room(ID3D12Device* device);

// What a T# the title wrote itself into a console descriptor heap came to (bind_title_image).
enum class TitleImage { known, bound, unknown, refused, not_tsharp };

// Statistics on unbound T# descriptors refused during console descriptor heap binding.
struct UnboundTsharpReasonStats {
    std::size_t distinct_slots = 0;
    std::uint64_t total_refusals = 0;
};

struct UnboundTsharpStats {
    std::map<std::string, UnboundTsharpReasonStats> by_reason;
    std::size_t total_distinct_slots = 0;
    std::uint64_t total_refusals = 0;
};

// Returns statistics on unbound T# descriptor refusals across console descriptor heaps.
UnboundTsharpStats unbound_tsharp_stats();
// Resets unbound T# refusal statistics (for testing).
void reset_unbound_tsharp_stats();
// Records an unbound T# refusal (for testing).
void note_refused_tsharp_for_test(std::uint64_t heap_va, std::size_t offset,
                                  const std::array<std::uint32_t, 8>& words, const char* why);

// Simulates console descriptor heap T# binding for a synthetic descriptor against registered resources.
// Returns TitleImage::bound on success, TitleImage::refused on rule violation (with *why set to the refusal string),
// or TitleImage::unknown / TitleImage::not_tsharp.
TitleImage test_bind_title_image(ID3D12Device* device, const std::array<std::uint32_t, 8>& words,
                                 const char** why = nullptr);

// Returns the internal image address registered for a resource in the device's image table.
std::uint64_t resource_image_address(ID3D12Resource* resource);
// Gives each S# a sampler in that set and a key in its hash table, as CreateSampler does, without a heap slot: for
// the literal S#s a shader builds from immediates (`literal_samplers` in the shader index). An S# that has them
// already is left as it is. E_INVALIDARG when `device` is not such a device; E_FAIL when one of them could not be
// registered (no bindless set, or its sampler array is full), the others are registered all the same.
HRESULT register_samplers(ID3D12Device* device, std::span<const leonida::gfx10::SamplerWords> samplers);
// True when GetClockCalibration on `queue` reads calibrated timestamps, false when it submits a timestamp query
// (or `queue` is not one of this module's queues). For tests.
bool queue_uses_calibrated_timestamps(ID3D12CommandQueue* queue);

// Creates a Vulkan-backed IDXGIFactory4 bound to `device`'s VulkanContext, instance, physical device, and queue.
// Its CreateSwapChainForHwnd registers swapchain backbuffers directly with the device's VulkanContext.
HRESULT create_dxgi_factory(ID3D12Device* device, REFIID riid, void** out);

// SDRWhiteLevel of DISPLAYCONFIG_SDR_WHITE_LEVEL (1000 = 80 nits) to nits.
float sdr_white_nits_from_level(unsigned long level);

// Puts back buffers of a device from create_vulkan_device* into a window (leonida::VulkanPresenter), for the
// presentation front end the game uses (DXGI or PresentX, still open). Holds a reference to the device.
class WindowPresenter {
public:
    // `device` is kept alive while the presenter lives; null when the owner guarantees that itself.
    WindowPresenter(IUnknown* device, std::unique_ptr<leonida::VulkanPresenter> presenter);
    ~WindowPresenter();
    WindowPresenter(const WindowPresenter&) = delete;
    WindowPresenter& operator=(const WindowPresenter&) = delete;

    // `back_buffer` is a colour texture in D3D12's PRESENT state. sync_interval 0 presents without vsync.
    // S_OK also covers a skipped present while the window is minimised.
    // `src_rect` (when not null) limits the blit to that part of the texture, which is stretched to the window.
    // `tear` (with sync_interval > 0): present in the tearing mode when the swapchain has one (tear_supported()), so a
    // frame that missed its vblank is shown at once; last_present_tore() tells whether that happened.
    HRESULT present(ID3D12Resource* back_buffer, UINT sync_interval, const D3D12_RECT* src_rect = nullptr,
                    bool tear = false);
    // A black frame, nothing read (PresentX with no planes).
    HRESULT present_blank(UINT sync_interval);
    // Show the shell image through startup clears, then hand off to the first non-black output.
    bool present_splash(ID3D12Resource* image);
    // One or two planes composed (VulkanPresenter::present_planes); `planes` names the textures by ResourceId, so the
    // caller fills it with resource_id(). DXGI_ERROR_UNSUPPORTED for a source the compose pass cannot read.
    HRESULT present_planes(const leonida::PresentPlanes& planes, UINT sync_interval, bool tear = false);
    // The swapchain holds HDR10 (ST.2084) values; and a test hook (VulkanPresenter::force_hdr_encoding).
    bool hdr_active() const;
    void force_hdr_encoding(bool on);
    std::uint64_t composed_presents() const;  // presents that went through the compose pass
    // Nits of SDR white the user set in Windows for the output the window is on (DISPLAYCONFIG_SDR_WHITE_LEVEL), or 0 when
    // it cannot be read (no window, no display config, not Windows). Only meaningful while hdr_active().
    float display_sdr_white_nits() const;
    void set_window(HWND window) { window_ = window; }
    bool tear_supported() const;
    bool last_present_tore() const;
    std::uint64_t tear_presents() const;
    // Screenshot support for tests: every presented frame is also copied to host memory (call before presenting).
    bool enable_capture();
    // See VulkanPresenter::set_abort: polled while a present waits for a swapchain image.
    void set_abort(std::function<bool()> abort);
    // See VulkanPresenter::read_back: false after a present skipped while the window is minimised, too.
    bool read_back(std::vector<std::uint8_t>& pixels, VkExtent2D* extent, VkFormat* format);
    VkExtent2D extent() const;
    std::uint64_t presented() const;

private:
    IUnknown* device_;
    HWND window_ = nullptr;
    std::unique_ptr<leonida::VulkanPresenter> presenter_;
};

// Whether `device` (from create_vulkan_device*) was created with VK_EXT_swapchain_colorspace on its instance.
bool device_has_swapchain_colorspace(ID3D12Device* device);

// Null when `device` is not a Vulkan device created with presentation, or the window cannot be presented to.
// The presenter holds a reference on the device, or on the Xbox device proxy that is its outer identity, so it may
// outlive the caller's reference. `hold_device` false takes none: for a presenter that the device proxy owns itself
// and destroys before its device (the presentation gta6rt's proxy builds, d3d12_xs_device.cpp), where a reference
// to the proxy would keep the proxy, the presenter and its window alive for good.
std::unique_ptr<WindowPresenter> create_window_presenter(ID3D12Device* device, HWND window, bool hold_device = true);

// Where each root parameter's arguments go in the 32 user-data dwords the translated shaders read (docs section
// 3.5). Built when a Vulkan device creates the root signature. A signature can be bound at either bind point, so it
// carries both layouts:
//   - graphics: rt::rootsig::pack_stage(Stage::Pixel), the console layout of vertex, pixel and geometry shaders
//     (from s8, one dword per table), which the game's own shaders read: VS_Blit/PS_BlitColour and the Scaleform
//     VS load their table's first descriptor from s[8:9] with s9 forced to 0. A vertex, pixel or geometry shader
//     reads SGPR i from push dword i, so root dword k is push dword 8 + k. A signature that would spill there
//     (more than 24 dwords) keeps rt::rootsig::pack() (from s0, two dwords per table), logged: the graphics spill
//     array is not modelled. A mesh shader's root arguments start at s16; the translator loads its s16.. from push
//     dwords 8.. ("root_window" in index.json), so the same block serves both stages of a mesh pipeline. Its window
//     is 16 dwords: mesh_ok says whether Stage::Mesh places the signature too, and a mesh draw with one it refuses
//     invalidates the list (D3D-448-GRAPHICS);
//   - compute: rt::rootsig::pack_stage(Stage::Compute), the measured console layout (from s4, one dword per table).
// A layout that refuses the signature is marked unusable; binding the signature there invalidates the list.
// LEONIDA_GRAPHICS_STAGE_LAYOUT=0 keeps rt::rootsig::pack() (from s0, two dwords per address) for every graphics
// signature instead of the console layout of the vertex, pixel and geometry stages described above.
// (The switch between the two packings is from the patch graphics-root-layout of
// Sigtryggur-Mani-Gudmundsson/graphics-runtime-patches at d9a0bc1b, author smg as "Codex", GPL-3.0.)
inline bool local_graphics_stage_layout() {
    static const bool on = [] { const char* v = std::getenv("LEONIDA_GRAPHICS_STAGE_LAYOUT"); return !(v && *v == '0'); }();
    return on;
}

struct RootArgumentLayout {
    struct Parameter {
        rt::rootsig::ParamType type;
        std::uint32_t user_offset;     // graphics: first push dword; unused for a parameter with no dwords
        std::uint32_t constant_count;  // constants only
        std::uint32_t compute_offset = rt::rootsig::kNoSgpr;  // compute: first user SGPR
        // A table: the slots its ranges declare, for the scan of title-written V#s before a submission
        // (src/title_va_alias.h) and the submission-time heap refresh. Views are capped at 1024 slots (an unbounded
        // range counts as 1024) and samplers at the shader-visible sampler heap limit. 0: not a table.
        std::uint32_t table_descriptors = 0;
    };
    std::vector<Parameter> parameters;
    std::uint32_t flags = 0;  // D3D12_ROOT_SIGNATURE_FLAGS from the serialized blob
    // The graphics layout is the console's (pack_stage(Stage::Pixel)): a table takes one dword. False: pack()'s, two.
    bool graphics_console = false;
    bool graphics_ok = true;
    // The console graphics layout also fits a mesh shader's 16-dword window (s16..s31 = push dwords 8..23). False
    // for a signature on pack()'s s0 layout, which a mesh shader translated with the root window cannot read.
    bool mesh_ok = false;
    bool compute_ok = true;
    // One past the last user-data dword the explicit root arguments take at each bind point, where the console puts
    // the directly indexed heaps (resource heap, then sampler heap). kNoSgpr: no such dword (the compute arguments
    // spill, or graphics keeps pack()'s s0 layout).
    std::uint32_t compute_arguments_end = rt::rootsig::kNoSgpr;
    std::uint32_t graphics_arguments_end = rt::rootsig::kNoSgpr;
};

// For the command list: the argument layout of a root signature from a Vulkan device, and the id it records for a
// compute pipeline state (0: not a compute PSO of a Vulkan device).
const RootArgumentLayout* root_signature_layout(ID3D12RootSignature* signature);
std::uintptr_t compute_pipeline_id(ID3D12PipelineState* pipeline);
std::uintptr_t graphics_pipeline_id(ID3D12PipelineState* pipeline);
// Record time, for LEONIDA_ASYNC_PIPELINES=1: starts the pipeline's build on the compile worker before the list is
// submitted, without waiting (PipelineState::queue_build). `id` is what compute_pipeline_id or graphics_pipeline_id gave (0:
// nothing). Compute: at SetPipelineState; graphics: at each draw, with the render-target slots it binds (bit per slot)
// and whether it binds a depth buffer.
void request_pipeline_build(std::uintptr_t id, std::uint32_t bound_targets, bool depth_bound);
// A LEONIDA_STUB_PIPELINES compute or mesh stub that `device` (a command list's attached device; null: any) created,
// without a pipeline and so without an id above: its dispatches or mesh draws record nothing. none for every other
// PSO, including a stub of another device and a PSO of another implementation, whose draws stay recording errors.
enum class StubPipelineKind : std::uint8_t { none, compute, mesh };
StubPipelineKind stub_pipeline_kind(ID3D12PipelineState* pipeline, IUnknown* device);
// What each stage of a Vulkan device's PSO asked for (src/subgroup_size.h), in creation order; null for another PSO.
const std::vector<leonida::SubgroupRequest>* pipeline_subgroup_requests(ID3D12PipelineState* pipeline);
// The linked SPIR-V stage modules for a lazy pipeline (in recipe stages order); false when not a lazy pipeline.
bool pipeline_stage_words(ID3D12PipelineState* pipeline, std::vector<std::vector<std::uint32_t>>& words,
                          std::string& why);
// PSOs of any Vulkan device not yet deleted, counting one whose deletion waits for its running build (tests).
long live_pipeline_states();

// Unwraps an Xbox-ABI heap proxy if needed and inspects one of this module's descriptor heaps; returns its owning
// device identity pointer, or nullptr when `heap` is null or belongs to another implementation.
IUnknown* descriptor_heap_info(ID3D12DescriptorHeap* heap, D3D12_DESCRIPTOR_HEAP_DESC& desc,
                               D3D12_GPU_VIRTUAL_ADDRESS& gpu_base);

// Xbox device slots 70/71: a buffer view of the title's own memory, with no resource object. `view` is a PC
// D3D12_SHADER_RESOURCE_VIEW_DESC (or, `unordered`, D3D12_UNORDERED_ACCESS_VIEW_DESC) of a buffer, `address` the
// memory's console VA and `width` its bytes. Writes the V# into `destination` and returns true; false when `device`
// is not this module's or the layer cannot write that view (the slot is left as it was).
bool write_placed_buffer_view(ID3D12Device* device, bool unordered, const void* view, std::uint64_t address,
                              std::uint64_t width, D3D12_CPU_DESCRIPTOR_HANDLE destination);

// Xbox placed texture SRV (slot 70) or UAV (slot 71). The 56-byte console resource description includes the
// XG layout at +44. The PC SRV/UAV description and placement address select the view. A GPU-writable
// image must already exist for a UAV. False leaves the destination descriptor zeroed.
bool write_placed_texture_view(ID3D12Device* device, bool unordered, const std::uint32_t* resource_words,
                               const std::uint32_t* view_words, std::uint64_t address,
                               D3D12_CPU_DESCRIPTOR_HANDLE destination);

// The VA shaders read a descriptor table at: for one inside a console descriptor heap (Xbox slot 14) whose arena copy
// lies elsewhere, the matching VA in that copy; any other VA unchanged.
D3D12_GPU_VIRTUAL_ADDRESS console_descriptor_table_va(D3D12_GPU_VIRTUAL_ADDRESS va);
// The console descriptor heap holding `va` (its console VA and bytes); false when none with a copy holds it.
bool console_descriptor_heap_range(D3D12_GPU_VIRTUAL_ADDRESS va, std::uint64_t& console_va, std::uint64_t& bytes);

// XDXR acceleration-structure commands (docs/d3d12_vulkan.md 4.8) of a list of `device`: every address the command
// writes or reads, as the title named it, becomes the VA shaders reach that memory by (src/title_va_alias.h: a
// slot-133 buffer's import or shadow, a window of the title's memory, a geometry copy), checked to hold every byte the
// builder touches there. False (logged by the device) when an address has no such copy: the command must not be
// recorded, since heap_base + a title address is no allocation. True and unchanged when `device` is not a Vulkan
// device of this module or builds KHR structures.
bool resolve_acceleration_structure_build(IUnknown* device, leonida::BuildAccelerationStructure& build);
bool resolve_acceleration_structure_copy(IUnknown* device, leonida::CopyAccelerationStructure& copy);
bool resolve_acceleration_structure_postbuild(IUnknown* device, leonida::AccelerationStructurePostbuild& info);

// Reads the shader-visible descriptor heap GPU base addresses currently bound on `list` (0 when unbound).
// Returns false when `list` is not one of this module's command lists.
bool bound_descriptor_heap_bases(ID3D12GraphicsCommandList* list, D3D12_GPU_VIRTUAL_ADDRESS& cbv_srv_uav,
                                 D3D12_GPU_VIRTUAL_ADDRESS& sampler);

// The size of every descriptor slot (GetDescriptorHandleIncrementSize), for handle ranges.
inline constexpr UINT descriptor_size = 32;

// ExecuteCommandLists returns void; this is what the last call on the queue did.
leonida::ReplayResult last_execute_result(ID3D12CommandQueue* queue);

// True when the current thread or process token has SeIncreaseBasePriorityPrivilege enabled (or the test override
// forces it), which D3D12 requires for D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME.
bool has_global_realtime_privilege();
// Test override for has_global_realtime_privilege(): -1 queries the token (default), 0 forces denied, 1 forces granted.
void set_global_realtime_privilege_override_for_test(int state);

// The front/back stencil references last recorded by ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef on
// `list`. The values are kept for the future pipeline path, which is what would apply them. False when `list` is
// not one of this module's command lists.
bool front_back_stencil_ref(ID3D12GraphicsCommandList8* list, UINT& front, UINT& back);

// Makes `list` hold a reference to `object` until its next Reset or its release, as it holds the pipeline states it
// names: for an object only the recording uses, such as a shim's own scratch buffer. A list is Reset once it has
// been executed (or a held batch, src/queue_holds.h, keeps it pinned until it has run), and a resource released
// after its submission is destroyed through the device's deferred queue once that submission has retired, so the
// object outlives every GPU use the recording makes of it. False when `list` is not one of this module's command
// lists or `object` is null.
bool hold_until_reset(ID3D12GraphicsCommandList* list, IUnknown* object);

// A copy of the commands `list` has recorded so far, for tests of what a bundle puts into its parent. False when
// `list` is not one of this module's command lists.
bool recorded_commands(ID3D12GraphicsCommandList* list, std::vector<leonida::Command>& commands);

inline leonida::ResourceId resource_id(const ID3D12Resource* resource) {
    return reinterpret_cast<leonida::ResourceId>(rt::d3d12_xs::texture_backing(resource));
}

// A query heap's ID in the command-list IR and the VulkanContext: its pointer value, as for resources.
inline leonida::QueryHeapId query_heap_id(const ID3D12QueryHeap* heap) {
    return reinterpret_cast<leonida::QueryHeapId>(heap);
}

}  // namespace rt::d3d12
