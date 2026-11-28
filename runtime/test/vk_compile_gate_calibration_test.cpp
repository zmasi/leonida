// The LEONIDA_COMPILE_GATE=auto calibration (src/compile_gate_calibration.h) on this machine's GPU: it must finish
// within kBoundMs and, on NVIDIA, reach a verdict (adaptive or holds) rather than give up. On NVIDIA 621.40 (an RTX
// 4070 Laptop) submissions were measured never to wait for a create, so there the verdict must be adaptive;
// LEONIDA_EXPECT_COMPILE_GATE=adaptive|classic states the expected gate on any other GPU. 77 without a device.
#include "vk_test_device.h"

#include "../src/compile_gate_calibration.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr double kBoundMs = 20000;
}

int main() {
    namespace policy = leonida::compile_gate_policy;
    vktest::Device device;
    if (!device.create()) {
        std::printf("vk_compile_gate_calibration_test: SKIP (no Vulkan device)\n");
        return 77;
    }
    VkPhysicalDeviceProperties props{};
    device.instance_fn<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(device.physical, &props);
    const std::string driver = policy::describe_driver(props.vendorID, props.driverVersion);
    std::printf("vk_compile_gate_calibration_test: %s, %s\n", props.deviceName, driver.c_str());
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        std::printf("vk_compile_gate_calibration_test: SKIP (a CPU device: the runtime does not calibrate those)\n");
        return 77;
    }
    const auto seed = static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;
    const auto result = leonida::compile_gate_calibration::calibrate(device.device_proc, device.device, device.queue,
                                                                      device.family, seed, "[test]");
    std::printf("vk_compile_gate_calibration_test: %s after %u trial(s), %u statements, %.0f ms: %s\n",
                result.ok ? policy::outcome_name(result.verdict.outcome) : "failed", result.trials,
                unsigned(result.statements), result.total_ms,
                result.ok ? policy::describe_verdict(result.verdict).c_str() : result.error.c_str());
    int failures = 0;
    if (!result.ok) {
        std::fprintf(stderr, "FAIL: calibration failed: %s\n", result.error.c_str());
        ++failures;
    }
    if (result.total_ms > kBoundMs) {
        std::fprintf(stderr, "FAIL: calibration took %.0f ms (bound %.0f ms)\n", result.total_ms, kBoundMs);
        ++failures;
    }
    const bool decided =
        result.verdict.outcome == policy::Outcome::adaptive || result.verdict.outcome == policy::Outcome::holds;
    if (result.ok && props.vendorID == policy::kNvidiaVendorId && !decided) {
        std::fprintf(stderr, "FAIL: no verdict on NVIDIA (%s)\n", policy::outcome_name(result.verdict.outcome));
        ++failures;
    }
    const char* expect = std::getenv("LEONIDA_EXPECT_COMPILE_GATE");
    const bool measured_621_40 = props.vendorID == policy::kNvidiaVendorId &&
                                 policy::decode_nvidia(props.driverVersion).major == 621 &&
                                 policy::decode_nvidia(props.driverVersion).minor == 40;
    if (!expect && measured_621_40) expect = "adaptive";
    if (result.ok && expect) {
        const char* got = policy::mode_for(result.verdict.outcome) == leonida::CompileGateMode::adaptive ? "adaptive"
                                                                                                         : "classic";
        if (std::strcmp(expect, got)) {
            std::fprintf(stderr, "FAIL: expected the %s gate, the calibration gives %s\n", expect, got);
            ++failures;
        }
    }
    if (failures) return 1;
    std::printf("vk_compile_gate_calibration_test: ok\n");
    return 0;
}
