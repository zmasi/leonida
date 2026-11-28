// LEONIDA_COMPILE_GATE=auto without a GPU (src/compile_gate_policy.h): the setting, NVIDIA's driver version, the
// verdict over a calibration's timings, the cache file and the slow pipeline's SPIR-V.
#include "../src/compile_gate_policy.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);     \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

using namespace leonida::compile_gate_policy;
using leonida::CompileGateMode;

void settings() {
    CHECK(parse_setting(nullptr).setting == Setting::automatic);
    CHECK(parse_setting("").setting == Setting::automatic);
    CHECK(parse_setting("auto").setting == Setting::automatic);
    CHECK(parse_setting("0").setting == Setting::off);
    CHECK(parse_setting("1").setting == Setting::always);
    CHECK(parse_setting("adaptive").setting == Setting::adaptive);
    CHECK(parse_setting("classic").setting == Setting::classic);
    CHECK(!parse_setting("classic").unknown);
    // Unknown values are classic, as before `auto`: never the adaptive gate.
    CHECK(parse_setting("2").setting == Setting::classic && parse_setting("2").unknown);
    CHECK(parse_setting("Adaptive").setting == Setting::classic);
    // Forced settings name their mode; auto is classic until the calibration decides.
    CHECK(forced_mode(Setting::automatic) == CompileGateMode::classic);
    CHECK(forced_mode(Setting::classic) == CompileGateMode::classic);
    CHECK(forced_mode(Setting::adaptive) == CompileGateMode::adaptive);
    CHECK(forced_mode(Setting::always) == CompileGateMode::always);
    CHECK(forced_mode(Setting::off) == CompileGateMode::off);
    CHECK(recalibrate("1") && !recalibrate("0") && !recalibrate(nullptr) && !recalibrate(""));
}

void nvidia_versions() {
    constexpr std::uint32_t v621_40 = encode_nvidia(621, 40);
    static_assert(decode_nvidia(v621_40).major == 621 && decode_nvidia(v621_40).minor == 40);
    CHECK(v621_40 == ((621u << 22) | (40u << 14)));
    CHECK(decode_nvidia(encode_nvidia(610, 88)).major == 610 && decode_nvidia(encode_nvidia(610, 88)).minor == 88);
    CHECK(decode_nvidia(encode_nvidia(630, 20) | 0x3fffu).minor == 20);  // the low 14 bits are not the minor
    CHECK(describe_driver(kNvidiaVendorId, encode_nvidia(621, 40)) == "NVIDIA 621.40");
    CHECK(describe_driver(kNvidiaVendorId, encode_nvidia(630, 2)) == "NVIDIA 630.02");
    CHECK(describe_driver(kAmdVendorId, 0x00802001u) == "AMD driver 0x00802001");
    CHECK(describe_driver(0, 0) == "an unknown GPU");
    CHECK(describe_driver(0x10005, 1) == "vendor 0x10005 driver 0x00000001");
}

Trial quick_trial(double create_ms, double every_ms, double each_ms) {
    Trial t;
    t.create_ms = create_ms;
    for (double s = 20; s < create_ms; s += every_ms) t.samples.push_back({s, s + each_ms});
    return t;
}

void verdicts() {
    // 621.40-like: submissions come back in a couple of ms all through a 900 ms create.
    {
        const Verdict v = judge(quick_trial(900, 10, 2));
        CHECK(v.outcome == Outcome::adaptive);
        CHECK(v.quick > 50);
        CHECK(v.max_quick_ms == 2);
        CHECK(mode_for(v.outcome) == CompileGateMode::adaptive);
    }
    // 630.20-like: the first submission is held until the create leaves.
    {
        Trial t;
        t.create_ms = 1200;
        t.samples = {{20, 1205}, {1210, 1212}};
        const Verdict v = judge(t);
        CHECK(v.outcome == Outcome::holds);
        CHECK(v.slow_ms == 1185);
        CHECK(mode_for(v.outcome) == CompileGateMode::classic);
    }
    // A hold outweighs any number of quick submissions (the driver may lock for only part of the create).
    {
        Trial t = quick_trial(600, 10, 1);
        t.create_ms = 1500;
        t.samples.push_back({700, 1510});
        CHECK(judge(t).outcome == Outcome::holds);
    }
    // Too fast to tell: the next trial is larger.
    {
        const Verdict v = judge(quick_trial(120, 10, 1));
        CHECK(v.outcome == Outcome::too_fast);
        CHECK(mode_for(v.outcome) == CompileGateMode::classic);
    }
    // A slow submission that did not come back with the create (something else on the GPU): unclear, classic.
    {
        Trial t = quick_trial(1000, 10, 1);
        t.samples.push_back({300, 700});
        CHECK(judge(t).outcome == Outcome::unclear);
        CHECK(mode_for(Outcome::unclear) == CompileGateMode::classic);
    }
    // Quick ones only at the start: not enough to say the create was still in the driver later.
    {
        Trial t;
        t.create_ms = 1000;
        t.samples = {{20, 21}, {30, 31}, {40, 41}, {50, 51}};
        CHECK(judge(t).outcome == Outcome::unclear);
    }
    // Submissions that came back as the create left, or that entered after it, tell nothing.
    {
        Trial t;
        t.create_ms = 1000;
        t.samples = {{600, 605}, {990, 995}, {1001, 1003}};
        const Verdict v = judge(t);
        CHECK(v.quick == 1);
        CHECK(v.outcome == Outcome::unclear);
    }
    // No samples at all: unclear (or too fast), never adaptive.
    {
        Trial t;
        t.create_ms = 2000;
        CHECK(judge(t).outcome == Outcome::unclear);
    }
    CHECK(next_statements(2000, 100, 32000) == 16000);   // at most 8x
    CHECK(next_statements(2000, 700, 32000) == 4000);    // at least 2x
    CHECK(next_statements(2000, 200, 32000) == 8000);    // towards 800 ms
    CHECK(next_statements(20000, 100, 32000) == 32000);  // capped
    CHECK(describe_verdict(judge(quick_trial(900, 10, 2))).find("submissions came back in <= 2.0 ms beside a 900 ms "
                                                                "create") != std::string::npos);
}

CacheKey key_a() {
    CacheKey k;
    k.vendor = kNvidiaVendorId;
    k.device = 0x2820;
    k.driver = encode_nvidia(621, 40);
    for (int i = 0; i < 16; ++i) k.uuid[i] = std::uint8_t(i * 17);
    return k;
}

void cache() {
    CacheEntry e;
    e.key = key_a();
    e.verdict.outcome = Outcome::adaptive;
    e.verdict.quick = 42;
    e.verdict.max_quick_ms = 3.14;
    e.verdict.create_ms = 912.0;
    const std::string line = format_entry(e);
    CHECK(line.find("outcome=adaptive") != std::string::npos);
    CHECK(line.find("driver=9b4a0000") != std::string::npos);
    const auto back = parse_entry(line);
    CHECK(back && same_key(back->key, e.key));
    CHECK(back && back->verdict.outcome == Outcome::adaptive && back->verdict.quick == 42);
    CHECK(back && back->verdict.max_quick_ms == 3.1 && back->verdict.create_ms == 912.0);
    CHECK(parse_entry(line + "\r\n").has_value());
    CHECK(parse_entry(line + " later_field=7").has_value());  // a later version's field is skipped

    // Damaged or foreign lines are not entries.
    CHECK(!parse_entry(""));
    CHECK(!parse_entry("compile-gate-calibration v2 vendor=10de"));
    CHECK(!parse_entry(line.substr(0, line.size() / 2)));
    std::string bad = line;
    bad.replace(bad.find("outcome=adaptive"), 16, "outcome=maybe");
    CHECK(!parse_entry(bad));
    std::string bad_uuid = line;
    bad_uuid.replace(bad_uuid.find("uuid=") + 5, 2, "zz");
    CHECK(!parse_entry(bad_uuid));

    // Another driver (630.20 on the same GPU) is another key.
    CacheEntry other = e;
    other.key.driver = encode_nvidia(630, 20);
    other.verdict = {};
    other.verdict.outcome = Outcome::holds;
    other.verdict.slow_ms = 1185;
    other.verdict.create_ms = 1200;
    std::string text = replace_entry("", e);
    text = replace_entry("garbage line\n" + text, other);
    CHECK(text.find("garbage") == std::string::npos);
    CHECK(find_entry(text, e.key) && find_entry(text, e.key)->verdict.outcome == Outcome::adaptive);
    CHECK(find_entry(text, other.key) && find_entry(text, other.key)->verdict.outcome == Outcome::holds);
    CacheKey unknown = key_a();
    unknown.uuid[3] ^= 1;
    CHECK(!find_entry(text, unknown));
    // Only verdicts that say something about the driver are cached: an unclear one would hold every later launch
    // on the classic gate.
    CHECK(cache_verdict(Outcome::adaptive) && cache_verdict(Outcome::holds) && cache_verdict(Outcome::too_fast));
    CHECK(!cache_verdict(Outcome::unclear));
    // Recalibrating writes over the entry for the key, keeping the others.
    CacheEntry again = e;
    again.verdict.outcome = Outcome::holds;
    text = replace_entry(text, again);
    CHECK(find_entry(text, e.key)->verdict.outcome == Outcome::holds);
    CHECK(find_entry(text, other.key).has_value());
    // An unclear line (written by an earlier build) is not a cached verdict: the next launch measures again.
    CacheEntry unclear = e;
    unclear.verdict.outcome = Outcome::unclear;
    CHECK(parse_entry(format_entry(unclear)).has_value());
    CHECK(!find_entry(replace_entry(text, unclear), e.key));
    CHECK(find_entry(replace_entry(text, unclear), other.key).has_value());
    std::size_t lines = 0;
    for (const char c : text) lines += c == '\n';
    CHECK(lines == 2);
}

void spirv() {
    const auto small = slow_compute_spirv(10);
    const auto big = slow_compute_spirv(1000);
    CHECK(small.size() > 5 && small[0] == 0x07230203u && small[1] == 0x00010000u);
    CHECK(big.size() > small.size() * 50);
    // Every instruction's word count walks exactly to the end, and no id reaches the bound.
    for (const auto* words : {&small, &big}) {
        std::size_t at = 5;
        bool ok = true;
        while (at < words->size()) {
            const std::uint32_t count = (*words)[at] >> 16;
            if (!count) {
                ok = false;
                break;
            }
            at += count;
        }
        CHECK(ok && at == words->size());
        CHECK((*words)[3] > 20);
    }
    // The same size gives the same program: the seed (specialization constant 0) is what varies it.
    CHECK(slow_compute_spirv(10) == small);
}

}  // namespace

int main() {
    settings();
    nvidia_versions();
    verdicts();
    cache();
    spirv();
    if (failures) {
        std::fprintf(stderr, "compile_gate_policy_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("compile_gate_policy_test: ok\n");
    return 0;
}
