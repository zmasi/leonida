// Which compile gate (SubmitMutex, vulkan_device_fault.h) a run uses, and the pure half of the calibration that
// decides it when LEONIDA_COMPILE_GATE is unset or `auto`: the setting, the verdict over the timings, the cache file
// and the SPIR-V of the slow pipeline. No Vulkan calls and no environment here, so compile_gate_policy_test checks it
// without a GPU; compile_gate_calibration.h runs the measurement.
//
// `auto` measures the driver once, at device creation and before the title runs: a worker thread creates a compute
// pipeline that takes the driver about a second to compile, and meanwhile the device's queue gets empty submissions,
// each waited for on a fence. Submissions that come back in under kQuickMs while the create is still in the driver
// mean this driver does not hold a submission behind a create: the adaptive gate. One held until the create leaves
// means it does (NVIDIA 630.20 held one 10 s beside a long compute create and lost the device): the classic gate. A
// create too fast to tell, a submission slow for another reason, or any failure: classic, the gate as before `auto`.
// Holding an empty submission for a second beside a one-second create cannot lose the device; the probes the adaptive
// gate makes in the title, beside creates of 30-60 s, can (docs/d3d12_vulkan.md). The verdict is cached per GPU and
// driver (compile_gate_calibration.txt beside the pipeline cache), so the measurement runs once per driver.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace leonida {

// The gate a run uses. off: no gate. classic: compute creates only, no limit, no probes. adaptive: bounded, probes,
// off once the driver is seen not to hold submissions. always: both kinds, no probes (LEONIDA_COMPILE_GATE=1).
enum class CompileGateMode { off, classic, adaptive, always };

namespace compile_gate_policy {

// What LEONIDA_COMPILE_GATE asks for. automatic: unset, empty or `auto`.
enum class Setting { automatic, off, classic, adaptive, always };

struct ParsedSetting {
    Setting setting = Setting::automatic;
    bool unknown = false;  // a value none of the below: classic, as before `auto`
};

// "0" off, "1" always, "adaptive", "classic", "auto"; unset or empty: auto. Anything else is classic, as it was
// before `auto`: an unknown value never turns the adaptive gate on.
inline ParsedSetting parse_setting(const char* value) {
    if (!value || !*value || !std::strcmp(value, "auto")) return {Setting::automatic, false};
    if (!std::strcmp(value, "0")) return {Setting::off, false};
    if (!std::strcmp(value, "1")) return {Setting::always, false};
    if (!std::strcmp(value, "adaptive")) return {Setting::adaptive, false};
    if (!std::strcmp(value, "classic")) return {Setting::classic, false};
    return {Setting::classic, true};
}

// The mode a forced setting names; automatic is classic until the calibration decides.
inline CompileGateMode forced_mode(Setting setting) {
    switch (setting) {
    case Setting::off: return CompileGateMode::off;
    case Setting::adaptive: return CompileGateMode::adaptive;
    case Setting::always: return CompileGateMode::always;
    case Setting::classic:
    case Setting::automatic: break;
    }
    return CompileGateMode::classic;
}

// Set by gta6rt's title path (main.cpp) only: the calibration never runs in a build host (--precompile-pipelines,
// --compile-server) or in a test process that creates a device, which keep the classic gate under `auto`.
inline std::atomic<bool> calibration_allowed{false};

inline constexpr std::uint32_t kNvidiaVendorId = 0x10DE;
inline constexpr std::uint32_t kAmdVendorId = 0x1002;
inline constexpr std::uint32_t kIntelVendorId = 0x8086;

// NVIDIA's VkPhysicalDeviceProperties::driverVersion: 10 bits major, 8 bits minor, then 8 and 6 bits the driver does
// not show on Windows ("621.40" is major 621, minor 40).
struct NvidiaVersion {
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
};
constexpr NvidiaVersion decode_nvidia(std::uint32_t version) {
    return {(version >> 22) & 0x3ffu, (version >> 14) & 0xffu};
}
constexpr std::uint32_t encode_nvidia(std::uint32_t major, std::uint32_t minor) {
    return ((major & 0x3ffu) << 22) | ((minor & 0xffu) << 14);
}

// "NVIDIA 621.40", "AMD driver 0x...", "vendor 0x... driver 0x...", "an unknown GPU", for the log.
inline std::string describe_driver(std::uint32_t vendor_id, std::uint32_t driver_version) {
    char text[96];
    if (vendor_id == kNvidiaVendorId) {
        const NvidiaVersion v = decode_nvidia(driver_version);
        std::snprintf(text, sizeof text, "NVIDIA %u.%02u", unsigned(v.major), unsigned(v.minor));
    } else if (vendor_id == kAmdVendorId) {
        std::snprintf(text, sizeof text, "AMD driver 0x%08x", unsigned(driver_version));
    } else if (vendor_id == kIntelVendorId) {
        std::snprintf(text, sizeof text, "Intel driver 0x%08x", unsigned(driver_version));
    } else if (vendor_id == 0) {
        std::snprintf(text, sizeof text, "an unknown GPU");
    } else {
        std::snprintf(text, sizeof text, "vendor 0x%04x driver 0x%08x", unsigned(vendor_id), unsigned(driver_version));
    }
    return text;
}

// ---- The verdict over one trial.

inline constexpr double kQuickMs = 50;        // a submission back within this did not wait for the create
inline constexpr double kMinCreateMs = 300;   // a shorter create cannot tell: the next trial is bigger
inline constexpr unsigned kMinQuick = 3;      // quick submissions needed for the adaptive verdict
inline constexpr double kEndMarginMs = 20;    // a quick one counts only if it came back this long before the create
inline constexpr double kHeldMs = 250;        // a submission this long ...
inline constexpr double kHeldReturnMs = 200;  // ... that came back within this of the create leaving was held by it
inline constexpr double kTargetCreateMs = 800;

// One submission, in ms from the moment the create entered the driver: entered the queue at start_ms, its fence
// signalled at end_ms.
struct Sample {
    double start_ms = 0;
    double end_ms = 0;
};

struct Trial {
    double create_ms = 0;  // how long vkCreateComputePipelines took
    std::vector<Sample> samples;
};

enum class Outcome {
    adaptive,  // submissions came back quickly with the create still in the driver
    holds,     // a submission was held until the create left
    too_fast,  // the create left before it could tell
    unclear,   // a submission was slow, but not held by the create; or too few samples
};

inline const char* outcome_name(Outcome outcome) {
    switch (outcome) {
    case Outcome::adaptive: return "adaptive";
    case Outcome::holds: return "holds";
    case Outcome::too_fast: return "too_fast";
    case Outcome::unclear: break;
    }
    return "unclear";
}
inline std::optional<Outcome> parse_outcome(std::string_view name) {
    for (const Outcome o : {Outcome::adaptive, Outcome::holds, Outcome::too_fast, Outcome::unclear})
        if (name == outcome_name(o)) return o;
    return std::nullopt;
}
inline CompileGateMode mode_for(Outcome outcome) {
    return outcome == Outcome::adaptive ? CompileGateMode::adaptive : CompileGateMode::classic;
}

struct Verdict {
    Outcome outcome = Outcome::unclear;
    unsigned quick = 0;        // submissions back within kQuickMs, before the create left
    double max_quick_ms = 0;   // the longest of those
    double create_ms = 0;
    double slow_ms = 0;        // the longest submission over kQuickMs (held or not), 0 if none
};

inline Verdict judge(const Trial& trial) {
    Verdict v;
    v.create_ms = trial.create_ms;
    bool held = false, slow = false, late_quick = false;
    for (const Sample& s : trial.samples) {
        if (s.start_ms < 0 || s.start_ms >= trial.create_ms) continue;  // entered while the create was in the driver
        const double ms = s.end_ms - s.start_ms;
        if (ms > kQuickMs) {
            v.slow_ms = (std::max)(v.slow_ms, ms);
            if (ms >= kHeldMs && s.end_ms >= trial.create_ms - kEndMarginMs &&
                s.end_ms <= trial.create_ms + kHeldReturnMs)
                held = true;
            else
                slow = true;
            continue;
        }
        if (s.end_ms > trial.create_ms - kEndMarginMs) continue;  // back as the create left: tells nothing
        ++v.quick;
        v.max_quick_ms = (std::max)(v.max_quick_ms, ms);
        if (s.start_ms >= trial.create_ms * 0.5) late_quick = true;
    }
    if (held) v.outcome = Outcome::holds;  // first: a hold outweighs any number of quick ones
    else if (slow) v.outcome = Outcome::unclear;
    else if (trial.create_ms < kMinCreateMs) v.outcome = Outcome::too_fast;
    else if (v.quick >= kMinQuick && late_quick) v.outcome = Outcome::adaptive;
    else v.outcome = Outcome::unclear;
    return v;
}

// How many statements the next trial gets after one of `statements` took `create_ms`: towards kTargetCreateMs, at
// least twice and at most eight times as many, and never more than `most`.
inline std::uint32_t next_statements(std::uint32_t statements, double create_ms, std::uint32_t most) {
    const double scaled = double(statements) * kTargetCreateMs / (std::max)(create_ms, 1.0);
    const double grown = std::clamp(scaled, double(statements) * 2, double(statements) * 8);
    return std::uint32_t((std::min)(grown, double(most)));
}

// "N submissions came back in <= X ms beside a Y ms create" and the other outcomes, for the log.
inline std::string describe_verdict(const Verdict& v) {
    char text[200];
    switch (v.outcome) {
    case Outcome::adaptive:
        std::snprintf(text, sizeof text, "%u submissions came back in <= %.1f ms beside a %.0f ms create", v.quick,
                      v.max_quick_ms, v.create_ms);
        break;
    case Outcome::holds:
        std::snprintf(text, sizeof text, "a submission was held %.0f ms, until a %.0f ms create left", v.slow_ms,
                      v.create_ms);
        break;
    case Outcome::too_fast:
        std::snprintf(text, sizeof text, "inconclusive: the largest create took only %.0f ms", v.create_ms);
        break;
    case Outcome::unclear:
        if (v.slow_ms > 0)
            std::snprintf(text, sizeof text, "inconclusive: a submission took %.0f ms beside a %.0f ms create, "
                                             "but did not come back with it", v.slow_ms, v.create_ms);
        else
            std::snprintf(text, sizeof text, "inconclusive: %u quick submissions beside a %.0f ms create", v.quick,
                          v.create_ms);
        break;
    }
    return text;
}

// ---- The cache: one line per GPU and driver.

struct CacheKey {
    std::uint32_t vendor = 0;
    std::uint32_t device = 0;
    std::uint32_t driver = 0;
    std::uint8_t uuid[16] = {};  // pipelineCacheUUID
};
inline bool same_key(const CacheKey& a, const CacheKey& b) {
    return a.vendor == b.vendor && a.device == b.device && a.driver == b.driver &&
           !std::memcmp(a.uuid, b.uuid, sizeof a.uuid);
}

struct CacheEntry {
    CacheKey key;
    Verdict verdict;
};

inline constexpr std::string_view kCacheTag = "compile-gate-calibration v1";

inline std::string format_entry(const CacheEntry& e) {
    char uuid[33];
    for (int i = 0; i < 16; ++i) std::snprintf(uuid + 2 * i, 3, "%02x", unsigned(e.key.uuid[i]));
    char text[320];
    // Whole tenths of a millisecond, not decimals: no locale reads a comma into them.
    std::snprintf(text, sizeof text,
                  "%s vendor=%04x device=%04x driver=%08x uuid=%s outcome=%s quick=%u max_quick_ds=%lld "
                  "create_ds=%lld slow_ds=%lld",
                  std::string(kCacheTag).c_str(), unsigned(e.key.vendor), unsigned(e.key.device),
                  unsigned(e.key.driver), uuid, outcome_name(e.verdict.outcome), e.verdict.quick,
                  static_cast<long long>(e.verdict.max_quick_ms * 10 + 0.5),
                  static_cast<long long>(e.verdict.create_ms * 10 + 0.5),
                  static_cast<long long>(e.verdict.slow_ms * 10 + 0.5));
    return text;
}

namespace detail {
inline bool hex_u32(std::string_view text, std::uint32_t& out) {
    if (text.empty() || text.size() > 8) return false;
    std::uint32_t v = 0;
    for (const char c : text) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | std::uint32_t(d);
    }
    out = v;
    return true;
}
inline bool dec_ll(std::string_view text, long long& out) {
    if (text.empty() || text.size() > 18) return false;
    long long v = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}
}  // namespace detail

// One cache line; nullopt for a line that is not one (another version, damaged).
inline std::optional<CacheEntry> parse_entry(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
    if (line.substr(0, kCacheTag.size()) != kCacheTag) return std::nullopt;
    line.remove_prefix(kCacheTag.size());
    CacheEntry e;
    unsigned seen = 0;
    while (!line.empty()) {
        if (line.front() != ' ') return std::nullopt;
        line.remove_prefix(1);
        const std::size_t end = (std::min)(line.find(' '), line.size());
        const std::string_view field = line.substr(0, end);
        line.remove_prefix(end);
        const std::size_t eq = field.find('=');
        if (eq == std::string_view::npos) return std::nullopt;
        const std::string_view name = field.substr(0, eq), value = field.substr(eq + 1);
        long long n = 0;
        if (name == "vendor") { if (!detail::hex_u32(value, e.key.vendor)) return std::nullopt; seen |= 1; }
        else if (name == "device") { if (!detail::hex_u32(value, e.key.device)) return std::nullopt; seen |= 2; }
        else if (name == "driver") { if (!detail::hex_u32(value, e.key.driver)) return std::nullopt; seen |= 4; }
        else if (name == "uuid") {
            if (value.size() != 32) return std::nullopt;
            for (int i = 0; i < 16; ++i) {
                std::uint32_t byte = 0;
                if (!detail::hex_u32(value.substr(std::size_t(2 * i), 2), byte)) return std::nullopt;
                e.key.uuid[i] = std::uint8_t(byte);
            }
            seen |= 8;
        } else if (name == "outcome") {
            const auto o = parse_outcome(value);
            if (!o) return std::nullopt;
            e.verdict.outcome = *o;
            seen |= 16;
        } else if (name == "quick") {
            if (!detail::dec_ll(value, n) || n > 0xffffffffLL) return std::nullopt;
            e.verdict.quick = unsigned(n);
        } else if (name == "max_quick_ds") { if (!detail::dec_ll(value, n)) return std::nullopt; e.verdict.max_quick_ms = double(n) / 10; }
        else if (name == "create_ds") { if (!detail::dec_ll(value, n)) return std::nullopt; e.verdict.create_ms = double(n) / 10; }
        else if (name == "slow_ds") { if (!detail::dec_ll(value, n)) return std::nullopt; e.verdict.slow_ms = double(n) / 10; }
        // An unknown field is skipped: a later version may add one.
    }
    if (seen != 31) return std::nullopt;
    return e;
}

// Whether a calibration's verdict is written to the cache. Not an unclear one: a submission slowed by something else
// on the GPU (another process, a driver background task) says nothing about this driver, and kept, it would hold
// every later launch on the classic gate until the driver changes; the next launch measures again instead. A create
// still too fast at kMostStatements is kept: that does not change from one launch to the next.
inline bool cache_verdict(Outcome outcome) { return outcome != Outcome::unclear; }

// The entry for `key` in a cache file's text, if it has one the cache keeps (an unclear one, written by an earlier
// build, is not one: the next launch measures again).
inline std::optional<CacheEntry> find_entry(std::string_view text, const CacheKey& key) {
    std::optional<CacheEntry> found;
    while (!text.empty()) {
        const std::size_t end = (std::min)(text.find('\n'), text.size());
        if (const auto e = parse_entry(text.substr(0, end));
            e && same_key(e->key, key) && cache_verdict(e->verdict.outcome))
            found = e;  // the last wins
        text.remove_prefix((std::min)(end + 1, text.size()));
    }
    return found;
}

// The text with `entry` in place of any line for its key, every other valid line kept, damaged lines dropped.
inline std::string replace_entry(std::string_view text, const CacheEntry& entry) {
    std::string out;
    while (!text.empty()) {
        const std::size_t end = (std::min)(text.find('\n'), text.size());
        if (const auto e = parse_entry(text.substr(0, end)); e && !same_key(e->key, entry.key))
            out += format_entry(*e) + "\n";
        text.remove_prefix((std::min)(end + 1, text.size()));
    }
    out += format_entry(entry) + "\n";
    return out;
}

// LEONIDA_COMPILE_GATE_RECALIBRATE=1: measure again and write over the cached verdict.
inline bool recalibrate(const char* value) { return value && !std::strcmp(value, "1"); }

// ---- The slow pipeline: a compute shader, SPIR-V 1.0, written here (no game data, no shader compiler).
//
// One storage buffer (set 0, binding 0) of floats: a = buf[0], b = buf[1] at run time, so nothing folds; then
// `statements` times
//     a = fract(sin(a * k1 + b) * 43758.5453);
//     if (a > k2) b = b * 0.97 + a * k1; else b = b - a * k2;
// with k1 and k2 made from the specialization constant 0 (the seed), and buf[2] = a + b. Each seed is a different
// program to the driver, so neither a pipeline cache nor the driver's own shader cache can answer the create. It is
// never dispatched.
class SpirvWriter {
public:
    std::uint32_t id() { return next_++; }
    void op(std::uint32_t opcode, std::initializer_list<std::uint32_t> operands) {
        words_.push_back(std::uint32_t(operands.size() + 1) << 16 | opcode);
        words_.insert(words_.end(), operands.begin(), operands.end());
    }
    void op_string(std::uint32_t opcode, std::initializer_list<std::uint32_t> before, const char* text,
                   std::initializer_list<std::uint32_t> after = {}) {
        const std::size_t length = std::strlen(text);
        const std::size_t string_words = length / 4 + 1;
        words_.push_back(std::uint32_t(1 + before.size() + string_words + after.size()) << 16 | opcode);
        words_.insert(words_.end(), before.begin(), before.end());
        for (std::size_t w = 0; w < string_words; ++w) {
            std::uint32_t word = 0;
            for (std::size_t b = 0; b < 4; ++b) {
                const std::size_t i = w * 4 + b;
                if (i < length) word |= std::uint32_t(std::uint8_t(text[i])) << (8 * b);
            }
            words_.push_back(word);
        }
        words_.insert(words_.end(), after.begin(), after.end());
    }
    std::vector<std::uint32_t> finish() {
        std::vector<std::uint32_t> out{0x07230203u, 0x00010000u, 0u, next_, 0u};
        out.insert(out.end(), words_.begin(), words_.end());
        return out;
    }

private:
    std::vector<std::uint32_t> words_;
    std::uint32_t next_ = 1;
};

inline std::uint32_t float_bits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

inline std::vector<std::uint32_t> slow_compute_spirv(std::uint32_t statements) {
    enum : std::uint32_t {
        OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpExecutionMode = 16,
        OpCapability = 17, OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22,
        OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33, OpConstant = 43,
        OpSpecConstant = 50, OpFunction = 54, OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62,
        OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpConvertUToF = 112, OpFAdd = 129,
        OpFSub = 131, OpFMul = 133, OpFOrdGreaterThan = 186, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249,
        OpBranchConditional = 250, OpReturn = 253,
    };
    enum : std::uint32_t { Uniform = 2, Function = 7, GLSLSin = 13, GLSLFract = 10 };
    SpirvWriter w;
    const std::uint32_t glsl = w.id(), main_fn = w.id(), void_t = w.id(), fn_t = w.id(), float_t = w.id(),
                        uint_t = w.id(), int_t = w.id(), bool_t = w.id(), seed = w.id(), array_t = w.id(),
                        block_t = w.id(), block_ptr = w.id(), buf = w.id(), ufloat_ptr = w.id(), ffloat_ptr = w.id(),
                        i0 = w.id(), i1 = w.id(), i2 = w.id(), c_big = w.id(), c_097 = w.id();
    constexpr std::uint32_t kConstants = 13;
    std::uint32_t ka[kConstants], kb[kConstants];
    for (std::uint32_t i = 0; i < kConstants; ++i) ka[i] = w.id(), kb[i] = w.id();

    w.op(OpCapability, {1});  // Shader
    w.op_string(OpExtInstImport, {glsl}, "GLSL.std.450");
    w.op(OpMemoryModel, {0, 1});
    w.op_string(OpEntryPoint, {5, main_fn}, "main");
    w.op(OpExecutionMode, {main_fn, 17, 1, 1, 1});  // LocalSize 1 1 1
    w.op(OpDecorate, {seed, 1, 0});                 // SpecId 0
    w.op(OpDecorate, {array_t, 6, 4});              // ArrayStride 4
    w.op(OpMemberDecorate, {block_t, 0, 35, 0});    // Offset 0
    w.op(OpDecorate, {block_t, 3});                 // BufferBlock
    w.op(OpDecorate, {buf, 34, 0});                 // DescriptorSet 0
    w.op(OpDecorate, {buf, 33, 0});                 // Binding 0
    w.op(OpTypeVoid, {void_t});
    w.op(OpTypeFunction, {fn_t, void_t});
    w.op(OpTypeFloat, {float_t, 32});
    w.op(OpTypeInt, {uint_t, 32, 0});
    w.op(OpTypeInt, {int_t, 32, 1});
    w.op(OpTypeBool, {bool_t});
    w.op(OpTypeRuntimeArray, {array_t, float_t});
    w.op(OpTypeStruct, {block_t, array_t});
    w.op(OpTypePointer, {block_ptr, Uniform, block_t});
    w.op(OpTypePointer, {ufloat_ptr, Uniform, float_t});
    w.op(OpTypePointer, {ffloat_ptr, Function, float_t});
    w.op(OpSpecConstant, {uint_t, seed, 0});
    w.op(OpConstant, {int_t, i0, 0});
    w.op(OpConstant, {int_t, i1, 1});
    w.op(OpConstant, {int_t, i2, 2});
    w.op(OpConstant, {float_t, c_big, float_bits(43758.5453f)});
    w.op(OpConstant, {float_t, c_097, float_bits(0.97f)});
    for (std::uint32_t i = 0; i < kConstants; ++i) {
        w.op(OpConstant, {float_t, ka[i], float_bits(0.0013f * float(i + 1))});
        w.op(OpConstant, {float_t, kb[i], float_bits(0.5f + 0.11f * float(i))});
    }
    w.op(OpVariable, {block_ptr, buf, Uniform});

    w.op(OpFunction, {void_t, main_fn, 0, fn_t});
    w.op(OpLabel, {w.id()});
    const std::uint32_t va = w.id(), vb = w.id();
    w.op(OpVariable, {ffloat_ptr, va, Function});
    w.op(OpVariable, {ffloat_ptr, vb, Function});
    const std::uint32_t p0 = w.id(), x = w.id(), p1 = w.id(), y = w.id(), seedf = w.id();
    w.op(OpAccessChain, {ufloat_ptr, p0, buf, i0, i0});
    w.op(OpLoad, {float_t, x, p0});
    w.op(OpAccessChain, {ufloat_ptr, p1, buf, i0, i1});
    w.op(OpLoad, {float_t, y, p1});
    w.op(OpConvertUToF, {float_t, seedf, seed});
    w.op(OpStore, {va, x});
    w.op(OpStore, {vb, y});
    for (std::uint32_t i = 0; i < statements; ++i) {
        const std::uint32_t c1 = i % kConstants, c2 = (i * 7 + 3) % kConstants;
        const std::uint32_t k1m = w.id(), k1 = w.id(), k2m = w.id(), k2 = w.id();
        w.op(OpFMul, {float_t, k1m, seedf, ka[c1]});
        w.op(OpFAdd, {float_t, k1, k1m, kb[c1]});
        w.op(OpFMul, {float_t, k2m, seedf, ka[c2]});
        w.op(OpFAdd, {float_t, k2, k2m, kb[c2]});
        const std::uint32_t a = w.id(), b = w.id(), t1 = w.id(), t2 = w.id(), s = w.id(), m = w.id(), f = w.id();
        w.op(OpLoad, {float_t, a, va});
        w.op(OpLoad, {float_t, b, vb});
        w.op(OpFMul, {float_t, t1, a, k1});
        w.op(OpFAdd, {float_t, t2, t1, b});
        w.op(OpExtInst, {float_t, s, glsl, GLSLSin, t2});
        w.op(OpFMul, {float_t, m, s, c_big});
        w.op(OpExtInst, {float_t, f, glsl, GLSLFract, m});
        w.op(OpStore, {va, f});
        const std::uint32_t cond = w.id(), then_l = w.id(), else_l = w.id(), merge_l = w.id();
        w.op(OpFOrdGreaterThan, {bool_t, cond, f, k2});
        w.op(OpSelectionMerge, {merge_l, 0});
        w.op(OpBranchConditional, {cond, then_l, else_l});
        w.op(OpLabel, {then_l});
        const std::uint32_t tb = w.id(), tf = w.id(), tsum = w.id();
        w.op(OpFMul, {float_t, tb, b, c_097});
        w.op(OpFMul, {float_t, tf, f, k1});
        w.op(OpFAdd, {float_t, tsum, tb, tf});
        w.op(OpStore, {vb, tsum});
        w.op(OpBranch, {merge_l});
        w.op(OpLabel, {else_l});
        const std::uint32_t ef = w.id(), ediff = w.id();
        w.op(OpFMul, {float_t, ef, f, k2});
        w.op(OpFSub, {float_t, ediff, b, ef});
        w.op(OpStore, {vb, ediff});
        w.op(OpBranch, {merge_l});
        w.op(OpLabel, {merge_l});
    }
    const std::uint32_t fa = w.id(), fb = w.id(), sum = w.id(), p2 = w.id();
    w.op(OpLoad, {float_t, fa, va});
    w.op(OpLoad, {float_t, fb, vb});
    w.op(OpFAdd, {float_t, sum, fa, fb});
    w.op(OpAccessChain, {ufloat_ptr, p2, buf, i0, i2});
    w.op(OpStore, {p2, sum});
    w.op(OpReturn, {});
    w.op(OpFunctionEnd, {});
    return w.finish();
}

}  // namespace compile_gate_policy
}  // namespace leonida
