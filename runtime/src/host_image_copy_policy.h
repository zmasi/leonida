// LEONIDA_HOST_IMAGE_COPY: whether the Vulkan D3D12 layer (shims/d3d12_vulkan_device.cpp) enables
// VK_EXT_host_image_copy for WriteToSubresource/ReadFromSubresource. Pure stdlib, so the choice is tested without a
// GPU (test/host_image_copy_policy_test.cpp).
//
//   0     never: the extension is not enabled and every CPU texture copy takes the staging submit.
//   1     whenever the device has the extension and its hostImageCopy feature.
//   auto  (unset, empty or any other text) as 1, except on NVIDIA, where it is off: a real boot on an RTX 4070 SUPER
//         lost the device (WRITE_INVALID) in 7 of 7 runs with host image copies and in 0 of 2 without them. The
//         cause is not known yet (docs/d3d12_vulkan.md).
#pragma once

#include <cstdint>
#include <cstring>

namespace leonida {

constexpr std::uint32_t kNvidiaVendorId = 0x10DE;  // VkPhysicalDeviceProperties::vendorID

struct HostImageCopyChoice {
    bool use;
    const char* reason;  // static text for the device-creation log line
};

// `setting` is the variable's text (null when unset); `available` is the extension with its hostImageCopy feature.
inline HostImageCopyChoice host_image_copy_choice(const char* setting, bool available, std::uint32_t vendor_id) {
    if (setting && std::strcmp(setting, "0") == 0) return {false, "LEONIDA_HOST_IMAGE_COPY=0"};
    if (!available) return {false, "not available"};
    if (setting && std::strcmp(setting, "1") == 0) return {true, "LEONIDA_HOST_IMAGE_COPY=1"};
    if (vendor_id == kNvidiaVendorId) return {false, "auto: NVIDIA"};
    return {true, "available"};
}

}  // namespace leonida
