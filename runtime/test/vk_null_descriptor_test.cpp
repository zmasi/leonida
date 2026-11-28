// Null descriptors of every kind through the bindless set 0 (src/vulkan_bindless.h), at the Vulkan level.
//
// On the console a null descriptor (a zero T#, or a T# whose image the layer refused or never saw) reads zeros and
// drops stores and atomics. Translated shaders find their image by hashing the T# (a miss is index 0) and read it
// from the binding of the instruction's own dimension, so index 0 of each binding must be a fallback of that
// binding's kind, and nothing written through a null UAV may show in a later read through a null SRV.
// test/glsl/cs_null_descriptors.comp looks up a zero T#, T#s of a 3D, a cube and a 1D image that no image answers,
// a zero S#, and the T# of one real 2D image, then:
//   pass 0: stores through the storage fallback of every dimension and component class, and adds to both r32ui
//           fallbacks (2D and 3D);
//   pass 1: reads every sampled fallback (fetched and sampled, the cube through the 2D-array binding at layer 3) and
//           every storage fallback, and adds to the r32ui fallbacks again.
// Every key but the real one must find index 0, every sampled read must be zero after the stores, and the real image
// must read its clear colour (so the reads are not vacuous). Twice: with BindlessFeatures::null_descriptor (where the
// device has VK_EXT_robustness2 nullDescriptor), where the stores and atomics are dropped (storage reads and atomic
// results zero too); and without it, where the storage fallbacks are images of their own that keep the stores,
// apart from the sampled ones.
// Exit 77 without a Vulkan device with the bindless features, or without the shader (no glslangValidator).
// LEONIDA_VK_VALIDATION=1 runs under VK_LAYER_KHRONOS_validation; a validation error fails the test.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "vk_test_device.h"
#include "shader_fixture_path.h"
#include "../src/vulkan_bindless.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#define EXPECT(condition)                                                    \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);     \
            return 1;                                                        \
        }                                                                    \
    } while (0)

// Everything but main needs the shader glslangValidator builds; without it CMake leaves the path undefined and
// main reports the test unsupported, so none of this may name it.
#ifdef LEONIDA_TEST_NULL_DESCRIPTORS_SPV
namespace {

using namespace leonida;

constexpr std::uint32_t result_words = 128;

// T# word 3 bits [31:28] hold the image TYPE (gfx10_descriptors.h): 8 1D, 9 2D, 10 3D, 11 cube.
DescriptorKey tsharp_of_type(std::uint32_t type, std::uint32_t salt) {
    return {0x00001000u + salt, 0x00000000u, 0x00000000u, (type << 28) | 0x00000fac, 0, 0, 0, 0};
}

std::vector<std::uint32_t> load_spirv(const char* path) {
    std::ifstream file(vktest::shader_fixture_path(path), std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    std::vector<std::uint32_t> words(bytes.size() / 4);
    if (!words.empty()) std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

// One run of the scenario on a new set with `null_descriptor`; the result words in `out`.
int run(vktest::Device& d, const VkPhysicalDeviceMemoryProperties& memory, bool null_descriptor,
        std::array<std::uint32_t, result_words>& out) {
    const VkDevice dev = d.device;
    BindlessFeatures features;
    features.update_after_bind = true;
    features.partially_bound = d.partially_bound;
    features.null_descriptor = null_descriptor;
    BindlessSet set(d.device_proc, dev, memory, features);
    EXPECT(set.ok());

    // The real 2D image: 1x1 RGBA8, cleared to a colour in the command buffer below.
    const VkImage image = d.image(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT);
    EXPECT(image);
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkCreateImageView>("vkCreateImageView")(dev, &view_info, nullptr, &view) == VK_SUCCESS);
    const DescriptorKey real_key = tsharp_of_type(9, 0x40);
    const std::uint32_t real_index = set.add_image(ImageDimension::dim_2d, view, VK_IMAGE_LAYOUT_GENERAL);
    EXPECT(real_index != 0);
    set.insert_key(real_key, real_index);

    const std::array<DescriptorKey, 6> keys{DescriptorKey{}, tsharp_of_type(10, 0x10), tsharp_of_type(11, 0x20),
                                            tsharp_of_type(8, 0x30), real_key, make_ssharp_key(std::array<std::uint32_t, 4>{}.data())};
    void* result_map = nullptr;
    void* key_map = nullptr;
    const VkBuffer result = d.buffer(result_words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &result_map);
    const VkBuffer key_buffer = d.buffer(sizeof(keys), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &key_map);
    EXPECT(result && key_buffer);
    std::memset(result_map, 0xcd, result_words * 4);
    std::memcpy(key_map, keys.data(), sizeof(keys));

    const std::vector<std::uint32_t> code = load_spirv(LEONIDA_TEST_NULL_DESCRIPTORS_SPV);
    EXPECT(!code.empty());
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize = code.size() * 4;
    module_info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkCreateShaderModule>("vkCreateShaderModule")(dev, &module_info, nullptr, &module) == VK_SUCCESS);

    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    }};
    VkDescriptorSetLayoutCreateInfo set1_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set1_info.bindingCount = std::uint32_t(bindings.size());
    set1_info.pBindings = bindings.data();
    VkDescriptorSetLayout set1_layout = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout")(dev, &set1_info, nullptr,
                                                                                &set1_layout) == VK_SUCCESS);
    const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool")(dev, &pool_info, nullptr, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set1_layout;
    VkDescriptorSet set1 = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets")(dev, &allocate, &set1) == VK_SUCCESS);
    const VkDescriptorBufferInfo buffer_infos[2] = {{result, 0, VK_WHOLE_SIZE}, {key_buffer, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[2]{};
    for (std::uint32_t b = 0; b < 2; ++b) {
        writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[b].dstSet = set1;
        writes[b].dstBinding = b;
        writes[b].descriptorCount = 1;
        writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[b].pBufferInfo = &buffer_infos[b];
    }
    d.fn<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets")(dev, 2, writes, 0, nullptr);

    const VkDescriptorSetLayout layouts[2] = {set.layout(), set1_layout};
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 2;
    layout_info.pSetLayouts = layouts;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(dev, &layout_info, nullptr, &layout) ==
           VK_SUCCESS);
    VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipeline_info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipeline_info.stage.module = module;
    pipeline_info.stage.pName = "main";
    pipeline_info.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    EXPECT(d.fn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(dev, VK_NULL_HANDLE, 1, &pipeline_info,
                                                                           nullptr, &pipeline) == VK_SUCCESS);

    bool flushed = false;
    const bool ran = d.run([&](VkCommandBuffer cb) {
        set.record_fallback_init(cb);
        flushed = set.flush(cb, 0, [] { return std::uint64_t(0); });
        VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        to_general.srcQueueFamilyIndex = to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_general.image = image;
        to_general.subresourceRange = view_info.subresourceRange;
        d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                                                               nullptr, 1, &to_general);
        const VkClearColorValue colour{{0.5f, 0.25f, 0.75f, 1.0f}};
        d.fn<PFN_vkCmdClearColorImage>("vkCmdClearColorImage")(cb, image, VK_IMAGE_LAYOUT_GENERAL, &colour, 1,
                                                               &view_info.subresourceRange);
        VkMemoryBarrier cleared{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        cleared.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        cleared.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &cleared, 0,
                                                               nullptr, 0, nullptr);
        d.fn<PFN_vkCmdBindPipeline>("vkCmdBindPipeline")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const VkDescriptorSet sets[2] = {set.set(), set1};
        d.fn<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets")(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 2,
                                                                     sets, 0, nullptr);
        for (std::uint32_t mode = 0; mode < 2; ++mode) {
            d.fn<PFN_vkCmdPushConstants>("vkCmdPushConstants")(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &mode);
            d.fn<PFN_vkCmdDispatch>("vkCmdDispatch")(cb, 1, 1, 1);
            VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            between.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            between.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT;
            d.fn<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
                cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &between, 0, nullptr, 0,
                nullptr);
        }
        return true;
    });
    set.table_upload_result(ran);
    EXPECT(ran && flushed);
    std::memcpy(out.data(), result_map, sizeof(out));

    d.fn<PFN_vkDestroyPipeline>("vkDestroyPipeline")(dev, pipeline, nullptr);
    d.fn<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(dev, layout, nullptr);
    d.fn<PFN_vkDestroyDescriptorPool>("vkDestroyDescriptorPool")(dev, pool, nullptr);
    d.fn<PFN_vkDestroyDescriptorSetLayout>("vkDestroyDescriptorSetLayout")(dev, set1_layout, nullptr);
    d.fn<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(dev, module, nullptr);
    set.release_retired();
    d.fn<PFN_vkDestroyImageView>("vkDestroyImageView")(dev, view, nullptr);
    return 0;
}

void print(const char* what, const std::array<std::uint32_t, result_words>& r, std::uint32_t first,
           std::uint32_t count) {
    std::printf("  %-28s", what);
    for (std::uint32_t i = 0; i < count; ++i) std::printf(" %08x", r[first + i]);
    std::printf("\n");
}

// The checks every mode shares: the keys' indices, the sampled fallbacks and the real image.
int check_common(const std::array<std::uint32_t, result_words>& r) {
    print("indices (0 3D cube 1D real S#)", r, 0, 6);
    print("fetch 2D / 2D array", r, 16, 8);
    print("fetch 3D / 1D", r, 24, 8);
    print("fetch real 2D", r, 32, 4);
    print("sample 2D / cube", r, 36, 8);
    print("sample 3D / 1D", r, 44, 8);
    print("storage 2D f u i", r, 64, 12);
    print("storage 2D array f u i", r, 76, 12);
    print("storage 3D f u i", r, 88, 12);
    print("storage 1D f u i", r, 100, 12);
    print("atomics pass 0 / pass 1", r, 8, 2);
    print("", r, 112, 2);
    // Every null key finds index 0, the fallback of the binding the instruction picks; the real one its image.
    EXPECT(r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] == 0 && r[5] == 0);
    EXPECT(r[4] != 0);
    // Every sampled fallback reads zeros, though pass 0 stored and added through every storage fallback.
    for (std::uint32_t i = 16; i < 32; ++i) EXPECT(r[i] == 0);
    for (std::uint32_t i = 36; i < 52; ++i) EXPECT(r[i] == 0);
    // The real image reads its clear colour (0.5, 0.25, 0.75, 1) as RGBA8: the shader does reach images.
    EXPECT(r[32] != 0 && r[35] == 0x3f800000u);
    return 0;
}

}  // namespace
#endif  // LEONIDA_TEST_NULL_DESCRIPTORS_SPV

int main() {
#ifndef LEONIDA_TEST_NULL_DESCRIPTORS_SPV
    std::printf("unsupported: the test shader was not built (no glslangValidator)\n");
    return 77;
#else
    vktest::Device d;
    d.request_null_descriptor = true;
    if (const auto status = d.initialize(); status != vktest::DeviceStatus::ready)
        return status == vktest::DeviceStatus::unavailable ? 77 : 1;
    if (!d.bindless) {
        std::printf("unsupported: the device lacks the bindless set's descriptor-indexing features\n");
        return 77;
    }
    VkPhysicalDeviceMemoryProperties memory{};
    d.instance_fn<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(d.physical, &memory);

    std::array<std::uint32_t, result_words> r{};
    if (d.null_descriptor) {
        std::printf("null descriptors (VK_EXT_robustness2 nullDescriptor):\n");
        EXPECT(run(d, memory, true, r) == 0);
        EXPECT(check_common(r) == 0);
        // The stores were dropped: every storage fallback reads zero, and the atomics returned zero both times.
        for (std::uint32_t i = 64; i < 112; ++i) EXPECT(r[i] == 0);
        EXPECT(r[8] == 0 && r[9] == 0 && r[112] == 0 && r[113] == 0);
    } else {
        std::printf("the device has no VK_EXT_robustness2 nullDescriptor: only the image fallbacks are tested\n");
    }

    std::printf("storage fallback images:\n");
    EXPECT(run(d, memory, false, r) == 0);
    EXPECT(check_common(r) == 0);
    // The storage fallbacks are images of their own: they took the stores (which the sampled reads above did not
    // see), and the atomics of pass 1 found what pass 0 left. (The uint class's storage fallback is also the
    // r32ui one, so the exact values depend on the order of pass 0's store and add, which is not checked.)
    EXPECT(r[64] != 0 && r[88] != 0 && r[100] != 0);
    EXPECT(r[112] != 0 && r[113] != 0);

    if (d.validation_errors()) {
        std::printf("%u validation errors\n", d.validation_errors());
        return 1;
    }
    std::printf("vk_null_descriptor: ok\n");
    return 0;
#endif
}
