// Unit tests for LEONIDA_HOST_IMAGE_COPY (runtime/src/host_image_copy_policy.h). No Vulkan instance and no GPU: the
// choice is a pure function of the variable's text, the extension's availability and the vendor.
#include "host_image_copy_policy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {

int failures = 0;

void expect(const char* setting, bool available, std::uint32_t vendor, bool use, const char* reason) {
    const leonida::HostImageCopyChoice got = leonida::host_image_copy_choice(setting, available, vendor);
    if (got.use == use && got.reason && std::strcmp(got.reason, reason) == 0) return;
    std::fprintf(stderr, "FAIL setting=%s available=%d vendor=0x%04X: got %s (%s), want %s (%s)\n",
                 setting ? setting : "(unset)", available ? 1 : 0, static_cast<unsigned>(vendor),
                 got.use ? "on" : "off", got.reason ? got.reason : "(null)", use ? "on" : "off", reason);
    ++failures;
}

}  // namespace

int main() {
    constexpr std::uint32_t nvidia = 0x10DE, amd = 0x1002, intel = 0x8086;
    static_assert(leonida::kNvidiaVendorId == nvidia, "NVIDIA's PCI vendor id");

    // auto (unset, empty, the word itself): on where available, except on NVIDIA.
    for (const char* automatic : {static_cast<const char*>(nullptr), "", "auto"}) {
        expect(automatic, true, amd, true, "available");
        expect(automatic, true, intel, true, "available");
        expect(automatic, true, nvidia, false, "auto: NVIDIA");
        expect(automatic, false, amd, false, "not available");
        expect(automatic, false, nvidia, false, "not available");
    }

    // 0: never, whatever the device has.
    expect("0", true, amd, false, "LEONIDA_HOST_IMAGE_COPY=0");
    expect("0", true, nvidia, false, "LEONIDA_HOST_IMAGE_COPY=0");
    expect("0", false, amd, false, "LEONIDA_HOST_IMAGE_COPY=0");

    // 1: whenever available, NVIDIA included; it cannot turn on what the device does not have.
    expect("1", true, nvidia, true, "LEONIDA_HOST_IMAGE_COPY=1");
    expect("1", true, amd, true, "LEONIDA_HOST_IMAGE_COPY=1");
    expect("1", false, nvidia, false, "not available");
    expect("1", false, amd, false, "not available");

    // Anything else is auto: only the exact texts 0 and 1 are switches.
    for (const char* garbage : {"yes", "on", "off", "true", "01", "10", " 1", "1 ", "2", "-1", "AUTO", "nvidia"}) {
        expect(garbage, true, amd, true, "available");
        expect(garbage, true, nvidia, false, "auto: NVIDIA");
        expect(garbage, false, amd, false, "not available");
    }

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("host_image_copy_policy: all cases passed\n");
    return 0;
}
