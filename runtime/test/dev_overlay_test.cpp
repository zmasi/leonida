// src/dev_overlay.h: the text of a sample, the counter differences, the phase bookkeeping behind its "now" line
// (submit_phases.h), the pipeline builds in flight (dev_status.h) and the memory figures it shares with the title HUD
// (memory_figures.h). No window, GPU or game file.
#include "dev_overlay.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#define CHECK(condition) do {                                        \
    if (!(condition)) {                                              \
        std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition); \
        return 1;                                                    \
    }                                                                \
} while (0)

namespace sp = leonida::submit_phases;
namespace dov = leonida::dev_overlay;

static bool has(const std::vector<std::string>& lines, const char* part) {
    for (const auto& line : lines)
        if (line.find(part) != std::string::npos) return true;
    return false;
}

// memory_figures.h, which the overlay and the title HUD share: the private bytes Wine reports, the text of both
// figures with and without one, and the device's published GPU memory, read at most once per refresh.
static int check_memory_figures() {
    namespace mf = leonida::memory_figures;
    constexpr std::uint64_t kGiB = 1ull << 30;

    // Wine fills PagefileUsage and leaves PrivateUsage 0; Windows fills both with the same value.
    mf::ProcessMemory ram;
    CHECK(mf::ram_text(ram) == "RAM n/a");  // GetProcessMemoryInfo failed
    ram.read = true;
    ram.working_set = kGiB;
    CHECK(mf::ram_text(ram) == "RAM n/a (ws 1.0)");
    ram.pagefile_usage = 5 * kGiB / 2;
    CHECK(mf::private_bytes(ram) == 5 * kGiB / 2 && mf::ram_text(ram) == "RAM 2.5 GB (ws 1.0)");
    ram.private_usage = 5 * kGiB / 2;
    CHECK(mf::private_bytes(ram) == 5 * kGiB / 2 && mf::ram_text(ram) == "RAM 2.5 GB (ws 1.0)");

    leonida::VramBudget vram;
    CHECK(mf::vram_text(vram) == "VRAM n/a");
    vram.available = true;
    vram.local_usage = 3 * kGiB + kGiB / 2;
    vram.local_budget = 16 * kGiB;
    vram.shared_usage = kGiB / 2;
    CHECK(mf::vram_text(vram) == "VRAM 3.5/16.0 GB +0.5 shared");

    // The overlay's line with both figures.
    dov::Sample s;
    s.ram = ram;
    s.vram = vram;
    CHECK(has(dov::lines(s, 1), "RAM 2.5 GB (ws 1.0)   VRAM 3.5/16.0 GB +0.5 shared"));

    // The published figure: none until a device publishes a reader; then read at most once per refresh_ms; another
    // device's withdrawal leaves it; its own takes it away.
    struct Fake {
        int reads = 0;
        leonida::VramBudget figure;
    };
    static Fake device;
    static const int other = 0;
    device.figure = vram;
    auto& publisher = mf::vram_publisher();
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
    CHECK(!publisher.latest(t0).available);
    mf::publish_vram_reader(
        [](const void* context) {
            auto* fake = static_cast<Fake*>(const_cast<void*>(context));
            ++fake->reads;
            return fake->figure;
        },
        &device);
    CHECK(publisher.latest(t0).local_usage == 3 * kGiB + kGiB / 2 && device.reads == 1);
    device.figure.local_usage = 4 * kGiB;
    CHECK(publisher.latest(t0 + mf::refresh_ms / 2).local_usage == 3 * kGiB + kGiB / 2 && device.reads == 1);
    CHECK(publisher.latest(t0 + mf::refresh_ms).local_usage == 4 * kGiB && device.reads == 2);
    mf::withdraw_vram_reader(&other);
    CHECK(mf::latest_vram().available);
    mf::withdraw_vram_reader(&device);
    CHECK(!mf::latest_vram().available);
    return 0;
}

int main() {
    // A total that went down was reset by its log line: what it holds is the new part.
    std::uint64_t before = 0;
    CHECK(dov::grown(10, before) == 10 && before == 10);
    CHECK(dov::grown(25, before) == 15);
    CHECK(dov::grown(4, before) == 4 && before == 4);

    // Nested scopes: the inner one is running, then the outer one again, then none.
    CHECK(sp::running().phase == -1);
    {
        const sp::Scope replay(sp::replay);
        CHECK(sp::running().phase == sp::replay);
        {
            const sp::Scope textures(sp::textures);
            CHECK(sp::running().phase == sp::textures);
        }
        CHECK(sp::running().phase == sp::replay);
    }
    CHECK(sp::running().phase == -1);

    // Two threads whose scopes overlap and end in the order they began: no phase stays behind.
    {
        std::atomic<int> step{0};
        std::thread a([&] {
            const sp::Scope heaps(sp::heaps);
            step = 1;
            while (step.load() < 2) std::this_thread::yield();
        });
        std::thread b([&] {
            while (step.load() < 1) std::this_thread::yield();
            const sp::Scope shadows(sp::shadows);
            step = 2;
            while (step.load() < 3) std::this_thread::yield();
        });
        a.join();
        CHECK(sp::running().phase == sp::shadows);
        step = 3;
        b.join();
        CHECK(sp::running().phase == -1);
    }

    // The builds in flight and the last one's name, cut to its buffer.
    auto& builds = leonida::dev_status::builds();
    const auto done = builds.done.load();
    {
        const leonida::dev_status::Build one("one");
        const leonida::dev_status::Build two(std::string(500, 'x').c_str());
        CHECK(builds.active.load() == 2 && builds.last() == std::string(95, 'x'));
    }
    CHECK(builds.active.load() == 0 && builds.done.load() == done + 2);

    dov::Sample s;
    auto text = dov::lines(s, 12);
    CHECK(text.size() == 7);
    // No counters and no device figure: "n/a", never a false 0.
    CHECK(has(text, "RAM n/a   VRAM n/a"));
    CHECK(has(text, "12 s") && has(text, "now: the title's own code") && has(text, "pipelines: 0 built"));
    CHECK(!has(text, "building"));
    s.frames = 8.44;
    s.phase = sp::textures;
    s.phase_for = 3.9;
    s.phase_ms[sp::shadows] = 2;
    s.phase_ms[sp::geometry] = 1.5;
    s.building = 3;
    s.built = 853;
    s.build = std::string(200, 'n');  // longer than the line's part for it
    s.drawn = 3029;
    s.skipped = 38363;
    text = dov::lines(s, 147);
    CHECK(has(text, "8.4 fps") && has(text, "now: refreshing textures for 3 s") && has(text, "copies 3.5"));
    CHECK(has(text, "853 built, 3 building (nnn") && has(text, "3029 drawn, 38363 skipped"));
    for (const auto& line : text) CHECK(line.size() < 160);
    // Regression: large or non-finite phase_for must not invoke float-to-int overflow UB.
    s.phase = sp::textures;
    s.phase_for = 1e12;
    text = dov::lines(s, 147);
    CHECK(has(text, "now: refreshing textures for 1000000000000 s"));
    s.phase_for = std::numeric_limits<double>::infinity();
    text = dov::lines(s, 147);
    CHECK(has(text, "now: refreshing textures") && !has(text, "for inf"));
    if (const int failed = check_memory_figures()) return failed;
    std::puts("dev_overlay_test: ok");
    return 0;
}
