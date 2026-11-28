// Capability probe for RT-TITLE-HOST-REGION: import one committed host region as a buffer-device-addressable
// Vulkan buffer with VK_EXT_external_memory_host. The optional argument is the region size in MiB (default 185).
// A successful import is checked in both directions: GPU reads CPU writes, and GPU writes are visible to the CPU.
//
// --strict (for an adapter known to be capable): the stack must answer that the host handle type is importable for
// this usage, and VK_EXT_external_memory_host and bufferDeviceAddress must be there, or the test fails (1) instead of
// skipping (77). The query is answered before any buffer exists, so a failing strict run creates, imports and binds
// nothing. Run it small: `vk_host_region_import_test 1 --strict`. A layer between the program and the driver that
// rewrites the handle type (GE-Proton11-14's win32u, see docs/testing.md) fails it at the query.
//
// Exit codes: 0 when the region imported and both directions matched; 77 (the ctest SKIP_RETURN_CODE) when this
// machine cannot import - the driver reports the usage or the handle type as not importable, refuses the host
// pointer, has no memory left to commit it, or has no Vulkan device - and 1 when the probe contradicts itself
// (a coherence mismatch, VK_ERROR_DEVICE_LOST, a driver answer that breaks its own contract, or a validation
// error). Every 77 prints one "SKIP vk_host_region_import: ..." line that says why. No shaders or game data.
#include "vk_test_device.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifndef VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME
#define VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME "VK_EXT_external_memory_host"
#endif

namespace {

constexpr std::uint64_t kMiB = 1ull << 20;
constexpr VkDeviceSize kSampleBytes = 4096;
constexpr int kSkip = 77;  // the SKIP_RETURN_CODE of runtime/CMakeLists.txt
// What the title's one arena buffer is used for: the translated draws and dispatches bind it as vertex, index,
// indirect, uniform and storage memory, and the title computes its GPU pointers from its device address.
constexpr VkBufferUsageFlags kImportUsage =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

struct Region {
    void* address = nullptr;
    ~Region() {
        if (address) VirtualFree(address, 0, MEM_RELEASE);
    }
};

struct Imported {
    vktest::Device& vk;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~Imported() {
        if (buffer) vk.fn<PFN_vkDestroyBuffer>("vkDestroyBuffer")(vk.device, buffer, nullptr);
        if (memory) vk.fn<PFN_vkFreeMemory>("vkFreeMemory")(vk.device, memory, nullptr);
    }
};

// A VkResult that says this machine cannot do the import, rather than that the import is wrong: the driver has no
// memory to commit it, or will not import this handle. VK_ERROR_DEVICE_LOST is deliberately not one of them: a
// lost device is a failure, and a skip would hide it.
bool capability_refusal(VkResult result) {
    return result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY ||
           result == VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

// VirtualAlloc refusing the commit because the machine has no room for it, as opposed to failing for any reason.
bool host_capacity_error(DWORD error) {
    return error == ERROR_NOT_ENOUGH_MEMORY || error == ERROR_COMMITMENT_LIMIT || error == ERROR_WORKING_SET_QUOTA;
}

void* allocate_region(std::size_t size, std::size_t alignment, DWORD* error) {
    *error = 0;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    if (alignment <= info.dwAllocationGranularity) {
        if (void* address = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) return address;
        *error = GetLastError();
        return nullptr;
    }

    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = alignment;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    const auto alloc2 = reinterpret_cast<decltype(&VirtualAlloc2)>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
    if (!alloc2) return nullptr;
    void* address =
        alloc2(GetCurrentProcess(), nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
    if (!address) *error = GetLastError();
    return address;
}

int fail(const char* message) {
    std::fprintf(stderr, "host-region import: %s\n", message);
    return 1;
}

// A driver call that failed: 77 with a SKIP line when the machine cannot import, 1 when the probe is at fault.
// `detail` is printed after the result, and may be empty.
int result_outcome(std::uint64_t mib, const char* what, VkResult result, const char* detail) {
    if (!capability_refusal(result)) {
        std::fprintf(stderr, "host-region import: %s failed (VkResult %d)%s%s\n", what, static_cast<int>(result),
                     *detail ? " " : "", detail);
        return 1;
    }
    std::printf("SKIP vk_host_region_import: size=%llu MiB result=%s vkResult=%d%s%s\n",
                static_cast<unsigned long long>(mib), what, static_cast<int>(result), *detail ? " " : "", detail);
    return kSkip;
}

int run(vktest::Device& vk, std::uint64_t mib, bool strict) {
    if (!mib || mib > (std::numeric_limits<std::size_t>::max)() / kMiB) return fail("invalid region size");
    const auto size = static_cast<VkDeviceSize>(mib * kMiB);
    const auto alignment = static_cast<std::size_t>(vk.host_pointer_alignment);
    if (!alignment || (alignment & (alignment - 1))) return fail("invalid imported-host-pointer alignment");

    // What the driver says about this exact usage and handle type before anything is allocated: a buffer it will
    // not import is a capability result, and the answer also says whether the memory must be dedicated.
    VkPhysicalDeviceExternalBufferInfo external_info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO};
    external_info.usage = kImportUsage;
    external_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkExternalBufferProperties external_properties{VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES};
    vk.instance_fn<PFN_vkGetPhysicalDeviceExternalBufferProperties>("vkGetPhysicalDeviceExternalBufferProperties")(
        vk.physical, &external_info, &external_properties);
    const VkExternalMemoryProperties& external = external_properties.externalMemoryProperties;
    if (!(external.compatibleHandleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT) ||
        !(external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) {
        if (strict) {
            std::fprintf(stderr,
                         "host-region import: --strict: the stack does not answer that the host handle type is "
                         "importable (handleTypes=0x%x features=0x%x); nothing was created or bound\n",
                         external.compatibleHandleTypes, external.externalMemoryFeatures);
            return 1;
        }
        std::printf("SKIP vk_host_region_import: size=%llu MiB result=not-importable handleTypes=0x%x features=0x%x\n",
                    static_cast<unsigned long long>(mib), external.compatibleHandleTypes,
                    external.externalMemoryFeatures);
        return kSkip;
    }
    const bool dedicated_only = (external.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) != 0;

    DWORD allocation_error = 0;
    Region region;
    region.address = allocate_region(static_cast<std::size_t>(size), alignment, &allocation_error);
    if (!region.address) {
        if (host_capacity_error(allocation_error)) {
            std::printf("SKIP vk_host_region_import: size=%llu MiB result=host-allocation-refused win32=%lu\n",
                        static_cast<unsigned long long>(mib), static_cast<unsigned long>(allocation_error));
            return kSkip;
        }
        std::fprintf(stderr, "host-region import: VirtualAlloc of %llu bytes at %llu-byte alignment failed (Win32 %lu)\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(alignment),
                     static_cast<unsigned long>(allocation_error));
        return 1;
    }
    if (reinterpret_cast<std::uintptr_t>(region.address) & (alignment - 1))
        return fail("host region is not aligned to minImportedHostPointerAlignment");

    Imported imported{vk};
    VkExternalMemoryBufferCreateInfo external_buffer{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external_buffer.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &external_buffer};
    buffer_info.size = size;
    buffer_info.usage = kImportUsage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult result =
        vk.fn<PFN_vkCreateBuffer>("vkCreateBuffer")(vk.device, &buffer_info, nullptr, &imported.buffer);
    if (result != VK_SUCCESS) return result_outcome(mib, "buffer-refused", result, "");

    VkMemoryRequirements requirements{};
    vk.fn<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(vk.device, imported.buffer,
                                                                               &requirements);
    if (requirements.size > size) {
        std::printf("SKIP vk_host_region_import: size=%llu MiB result=requirements-beyond-region requirements=%llu "
                    "bytes\n",
                    static_cast<unsigned long long>(mib), static_cast<unsigned long long>(requirements.size));
        return kSkip;
    }

    const auto host_properties = vk.fn<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT");
    if (!host_properties) return fail("vkGetMemoryHostPointerPropertiesEXT is not available");
    VkMemoryHostPointerPropertiesEXT host{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    result = host_properties(vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, region.address,
                             &host);
    if (result != VK_SUCCESS) return result_outcome(mib, "host-pointer-refused", result, "");
    if (!host.memoryTypeBits) {
        std::printf("SKIP vk_host_region_import: size=%llu MiB result=no-memory-type memoryTypeBits=0\n",
                    static_cast<unsigned long long>(mib));
        return kSkip;
    }

    VkPhysicalDeviceMemoryProperties memory_properties{};
    vk.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(vk.physical,
                                                                                                      &memory_properties);
    std::uint32_t memory_type = std::numeric_limits<std::uint32_t>::max();
    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (!(host.memoryTypeBits & requirements.memoryTypeBits & (1u << i))) continue;
        const auto flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memory_type = i;
            break;
        }
    }
    if (memory_type == std::numeric_limits<std::uint32_t>::max()) {
        std::printf("SKIP vk_host_region_import: size=%llu MiB result=unsupported no host-visible coherent type\n",
                    static_cast<unsigned long long>(mib));
        return kSkip;
    }

    // Dedicated only where the driver said so for this handle type (Vulkan requires the binding then).
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.buffer = imported.buffer;
    VkImportMemoryHostPointerInfoEXT host_import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    host_import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    host_import.pHostPointer = region.address;
    host_import.pNext = dedicated_only ? &dedicated : nullptr;
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &host_import};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type;
    result = vk.fn<PFN_vkAllocateMemory>("vkAllocateMemory")(vk.device, &allocate, nullptr, &imported.memory);
    if (result != VK_SUCCESS) {
        char detail[192] = {};
        std::snprintf(detail, sizeof detail,
                      "alignment=%llu requirements=%llu memoryTypeBits=0x%x type=%u dedicated=%s",
                      static_cast<unsigned long long>(requirements.alignment),
                      static_cast<unsigned long long>(requirements.size), host.memoryTypeBits, memory_type,
                      dedicated_only ? "yes" : "no");
        return result_outcome(mib, "import-refused", result, detail);
    }
    result = vk.fn<PFN_vkBindBufferMemory>("vkBindBufferMemory")(vk.device, imported.buffer, imported.memory, 0);
    if (result != VK_SUCCESS) return result_outcome(mib, "bind-refused", result, "");

    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = imported.buffer;
    const auto address = vk.fn<PFN_vkGetBufferDeviceAddress>("vkGetBufferDeviceAddress")(vk.device, &address_info);
    if (!address) return fail("imported buffer has no device address");

    auto* bytes = static_cast<std::uint8_t*>(region.address);
    for (VkDeviceSize i = 0; i < kSampleBytes; ++i) {
        bytes[i] = static_cast<std::uint8_t>(i * 131u + 7u);
        bytes[size - kSampleBytes + i] = static_cast<std::uint8_t>(i * 197u + 11u);
    }

    void* readback_map = nullptr;
    const VkBuffer readback = vk.buffer(2 * kSampleBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback_map);
    if (!readback || !readback_map) return fail("readback buffer creation failed");
    const auto copy = vk.fn<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    const auto pipeline_barrier = vk.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    const bool copied = vk.run([&](VkCommandBuffer command) {
        VkBufferCopy regions[2]{};
        regions[0].srcOffset = 0;
        regions[0].dstOffset = 0;
        regions[0].size = kSampleBytes;
        regions[1].srcOffset = size - kSampleBytes;
        regions[1].dstOffset = kSampleBytes;
        regions[1].size = kSampleBytes;
        copy(command, imported.buffer, readback, 2, regions);
        // The CPU reads the copy's destination next: its writes are only available to the host after a
        // transfer -> host barrier (without it, synchronization validation reports the read as a hazard).
        VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0,
                         nullptr, 0, nullptr);
        return true;
    });
    if (!copied) return fail("GPU copy from imported memory failed");
    const auto* readback_bytes = static_cast<const std::uint8_t*>(readback_map);
    if (std::memcmp(readback_bytes, bytes, kSampleBytes) != 0 ||
        std::memcmp(readback_bytes + kSampleBytes, bytes + size - kSampleBytes, kSampleBytes) != 0)
        return fail("GPU did not read the CPU's writes");

    const auto fill = vk.fn<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    const bool filled = vk.run([&](VkCommandBuffer command) {
        fill(command, imported.buffer, 0, kSampleBytes, 0xa5a5a5a5u);
        fill(command, imported.buffer, size - kSampleBytes, kSampleBytes, 0x5a5a5a5au);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        pipeline_barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
        return true;
    });
    if (!filled) return fail("GPU fill of imported memory failed");
    for (VkDeviceSize i = 0; i < kSampleBytes; ++i) {
        if (bytes[i] != 0xa5 || bytes[size - kSampleBytes + i] != 0x5a)
            return fail("CPU did not see the GPU's writes");
    }

    std::printf("host-region import: size=%llu MiB result=imported alignment=%llu requirements=%llu "
                "memoryTypeBits=0x%x type=%u dedicated=%s bda=0x%llx handleTypes=0x%x import-features=0x%x "
                "cpu-gpu-coherence=ok\n",
                static_cast<unsigned long long>(mib), static_cast<unsigned long long>(requirements.alignment),
                static_cast<unsigned long long>(requirements.size), host.memoryTypeBits, memory_type,
                dedicated_only ? "yes" : "no", static_cast<unsigned long long>(address),
                external.compatibleHandleTypes, external.externalMemoryFeatures);
    return 0;
}

// True when LEONIDA_VK_VALIDATION asks for the Khronos layer and it is not installed. vk_test_device.h then fails
// the device, which is right for a test that asked for validation by hand; this probe's CTest variant runs on
// every machine, so it reports a missing layer as a skip instead of a failure.
bool validation_layer_missing() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: read once, as vk_test_device.h does
#endif
    const char* env = std::getenv("LEONIDA_VK_VALIDATION");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (!env || std::strcmp(env, "0") == 0) return false;
    const auto instance_proc = load_vulkan();
    if (!instance_proc) return false;  // initialize() reports the missing loader as unavailable
    const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(
        instance_proc(nullptr, "vkEnumerateInstanceLayerProperties"));
    if (!enumerate) return false;
    std::vector<VkLayerProperties> layers;
    if (vktest::enumerate_all(layers, enumerate) != VK_SUCCESS) return false;
    for (const auto& layer : layers)
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (validation_layer_missing()) {
        std::printf("SKIP vk_host_region_import: LEONIDA_VK_VALIDATION is set but VK_LAYER_KHRONOS_validation is not "
                    "installed\n");
        return kSkip;
    }
    std::uint64_t mib = 185;
    int args = argc - 1;  // [MiB] [--strict]
    const bool strict = args > 0 && std::strcmp(argv[args], "--strict") == 0;
    if (strict) --args;
    char* end = nullptr;
    if (args == 1) mib = std::strtoull(argv[1], &end, 10);
    if (args > 1 || (args == 1 && (!end || *end))) {
        std::fprintf(stderr, "usage: vk_host_region_import_test [MiB] [--strict]\n");
        return 1;
    }
    vktest::Device vk;
    vk.request_buffer_device_address = true;
    const auto status = vk.initialize();
    if (status == vktest::DeviceStatus::unavailable) {
        std::printf("SKIP vk_host_region_import: no Vulkan 1.3 device\n");
        return kSkip;
    }
    if (status != vktest::DeviceStatus::ready) return 1;
    if (!vk.host_pointer_alignment || !vk.buffer_device_address) {
        if (strict) {
            std::fprintf(stderr, "host-region import: --strict: VK_EXT_external_memory_host or bufferDeviceAddress is "
                                 "not available\n");
            return 1;
        }
        std::printf("SKIP vk_host_region_import: VK_EXT_external_memory_host or bufferDeviceAddress is not "
                    "available\n");
        return kSkip;
    }
    const int result = run(vk, mib, strict);
    if (vk.validation_errors()) {
        std::fprintf(stderr, "%u validation errors\n", vk.validation_errors());
        return 1;
    }
    return result;
}
