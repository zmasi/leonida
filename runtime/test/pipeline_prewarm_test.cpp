// RT-PIPELINE-PREWARM-ORDER phase 2 (src/pipeline_prewarm.h) without a device: the plan read from an order log, and the
// plan's warm-ups on the compile workers (shims/d3d12_pipeline_compile.h), whose jobs wait at gates the test opens.
//   - the recorded order is kept, on the workers too;
//   - the budget (recorded build time) and the entry limit are respected;
//   - a demanded build overtakes the queued prewarms and starts while one runs;
//   - a missing, garbled or other-GPU log gives no plan, with a message; `E` lines are skipped as not damage.
#include "../src/pipeline_prewarm.h"
#include "../shims/d3d12_pipeline_compile.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
#define EXPECT(cond)                                                                       \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

namespace order = leonida::pipeline_order;
namespace prewarm = leonida::pipeline_prewarm;
using rt::d3d12::CompilePool;
using rt::d3d12::PipelineCompiler;
using namespace std::chrono_literals;

constexpr std::uint32_t vendor = 0x10de, device_id = 0x2783;

std::string key(int i) {
    char text[65];
    std::snprintf(text, sizeof(text), "%064x", i + 1);
    return text;
}

order::Record record(char kind, int i, std::uint32_t variant, std::uint32_t build_ms) {
    order::Record r;
    r.kind = kind;
    r.key = key(i);
    r.variant = variant;
    r.created_ms = 100;
    r.first_ms = 200 + std::uint32_t(i);
    r.build_ms = build_ms;
    return r;
}

std::string session(const std::vector<order::Record>& records, std::uint32_t device = device_id) {
    std::string text = order::format_session(vendor, device);
    for (const auto& r : records) text += order::format_record(r);
    return text;
}

void test_order_is_kept() {
    const std::string text = order::format_header() +
                             session({record('g', 3, 0, 50), record('m', 1, 0, 50), record('c', 7, 0, 50),
                                      record('g', 3, 0x100, 50), record('g', 2, 0, 50), record('g', 3, 0, 50)});
    const auto plan = prewarm::make_plan(text, vendor, device_id);
    EXPECT(plan);
    EXPECT(plan.entries == 4);  // the repeated (key 3, variant 0) once, the compute record left out
    EXPECT(plan.compute_skipped == 1);
    EXPECT(plan.session_records == 6);
    EXPECT(plan.by_key.size() == 3);
    const auto* three = plan.find(key(3));
    EXPECT(three && three->size() == 2);
    if (three && three->size() == 2) {
        EXPECT((*three)[0].rank == 0 && (*three)[0].variant == 0 && (*three)[0].kind == 'g');
        EXPECT((*three)[1].rank == 2 && (*three)[1].variant == 0x100);
    }
    const auto* one = plan.find(key(1));
    EXPECT(one && one->size() == 1 && (*one)[0].rank == 1 && (*one)[0].kind == 'm');
    const auto* two = plan.find(key(2));
    EXPECT(two && two->size() == 1 && (*two)[0].rank == 3);
    EXPECT(!plan.find(key(7)));
    EXPECT(prewarm::priority(0) > 0 && prewarm::priority(0) < prewarm::priority(1));
    EXPECT(plan.message.find("4 graphics variant(s) of 3 pipeline(s)") != std::string::npos);
}

void test_the_largest_run_on_this_gpu_is_read() {
    const std::string text = order::format_header() + session({record('g', 1, 0, 1), record('g', 2, 0, 1)}) +
                             session({record('g', 3, 0, 1), record('g', 4, 0, 1), record('g', 5, 0, 1)}, 0x9999) +
                             session({record('g', 6, 0, 1)}) +
                             session({record('g', 7, 0, 1), record('g', 8, 0, 1)});
    const auto plan = prewarm::make_plan(text, vendor, device_id);
    EXPECT(plan.entries == 2);
    EXPECT(plan.find(key(7)) && plan.find(key(8)));  // the newer of the two largest; the other GPU's run is not read
    EXPECT(!plan.find(key(3)) && !plan.find(key(1)));
}

void test_the_budget_is_respected() {
    const std::string text = session({record('g', 0, 0, 400), record('g', 1, 0, 400), record('g', 2, 0, 300),
                                      record('g', 3, 0, order::unknown_ms), record('g', 4, 0, 100)});
    prewarm::Options options;
    options.budget_ms = 1000;
    auto plan = prewarm::make_plan(text, vendor, device_id, options);
    // 400 + 400 fit, 300 would make 1100, the unknown one counts 1000, 100 fits: 900.
    EXPECT(plan.entries == 3);
    EXPECT(plan.find(key(0)) && plan.find(key(1)) && plan.find(key(4)));
    EXPECT(!plan.find(key(2)) && !plan.find(key(3)));
    EXPECT(plan.budget_used_ms == 900);
    EXPECT(plan.over_budget == 2);
    if (const auto* four = plan.find(key(4))) EXPECT((*four)[0].rank == 2);  // ranks stay consecutive

    options.budget_ms = prewarm::default_budget_ms;
    options.max_entries = 2;
    plan = prewarm::make_plan(text, vendor, device_id, options);
    EXPECT(plan.entries == 2 && plan.over_budget == 3);

    EXPECT(prewarm::parse_count(nullptr, 7) == 7);
    EXPECT(prewarm::parse_count("", 7) == 7);
    EXPECT(prewarm::parse_count("0", 7) == 7);
    EXPECT(prewarm::parse_count("12x", 7) == 7);
    EXPECT(prewarm::parse_count("-5", 7) == 7);
    EXPECT(prewarm::parse_count("99999999999999999999999", 7) == 7);
    EXPECT(prewarm::parse_count("120000", 7) == 120000);
}

void test_a_missing_or_garbled_log_is_ignored_with_a_message() {
    const auto missing = prewarm::load_plan(std::filesystem::temp_directory_path() / "leonida-no-such-order.log",
                                            vendor, device_id);
    EXPECT(!missing && missing.message.find("cannot read") != std::string::npos);

    const auto garbled = prewarm::make_plan("\x01\x02 not a log\nP g zz\n\xff\xfe", vendor, device_id);
    EXPECT(!garbled && garbled.message.find("no readable record") != std::string::npos);
    EXPECT(garbled.bad_lines >= 1);

    std::string damaged = session({record('g', 1, 0, 5), record('g', 2, 0, 5)});
    damaged[damaged.find(key(2)) + 3] ^= 1;  // the checksum no longer matches: that line is skipped
    const auto partial = prewarm::make_plan(damaged, vendor, device_id);
    EXPECT(partial.entries == 1 && partial.find(key(1)) && partial.bad_lines == 1);
    EXPECT(partial.message.find("1 damaged line(s) skipped") != std::string::npos);

    const auto other_gpu = prewarm::make_plan(session({record('g', 1, 0, 5)}, 0x1234), vendor, device_id);
    EXPECT(!other_gpu && other_gpu.message.find("none on this GPU") != std::string::npos);

    EXPECT(!prewarm::make_plan("", vendor, device_id));

    // An `E` line (the offline precompiler's) is neither read nor counted as damage.
    std::string with_e = order::format_session(vendor, device_id) + order::format_record(record('g', 1, 0, 5)) +
                         "E " + key(1) + " 0 0123456789abcdef *00000000\n" + order::format_record(record('g', 2, 0, 5));
    const auto e_lines = prewarm::make_plan(with_e, vendor, device_id);
    EXPECT(e_lines.entries == 2 && e_lines.bad_lines == 0);

    // A file on disk, read the same way.
    const auto path = std::filesystem::temp_directory_path() / "leonida-pipeline-prewarm-test.log";
    {
        std::ofstream out(path, std::ios::binary);
        out << order::format_header() << session({record('g', 9, 0, 5)});
    }
    const auto file = prewarm::load_plan(path, vendor, device_id);
    EXPECT(file.entries == 1 && file.find(key(9)));
    EXPECT(file.message.find(path.string()) != std::string::npos);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// The plan's warm-ups on the compile workers.
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    void wait() {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return open; });
    }
    void release() {
        {
            std::lock_guard lock(mutex);
            open = true;
        }
        changed.notify_all();
    }
};

struct Log {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> started, finished;
    void start(const std::string& name) {
        std::lock_guard lock(mutex);
        started.push_back(name);
        changed.notify_all();
    }
    void finish(const std::string& name) {
        std::lock_guard lock(mutex);
        finished.push_back(name);
        changed.notify_all();
    }
    bool has_started(const std::string& name) {
        std::lock_guard lock(mutex);
        return std::find(started.begin(), started.end(), name) != started.end();
    }
    bool wait_started(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [&] { return started.size() >= count; });
    }
    bool wait_finished(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 10s, [&] { return finished.size() >= count; });
    }
};

const char device_slot = 0;
const void* const device = &device_slot;

PipelineCompiler& make_pool(unsigned threads) {
    auto* pool = new PipelineCompiler;  // never destroyed, as in d3d12_compile_pool_test
    CompilePool config;
    config.threads = threads;
    config.heavy = threads - 1;
    EXPECT(pool->configure(config));
    pool->retain();
    return *pool;
}

void submit(PipelineCompiler& pool, Log& log, Gate* gate, const void* owner, const std::string& name,
            std::size_t priority) {
    EXPECT(pool.submit(owner, device, priority, [&log, gate, name] {
        log.start(name);
        if (gate) gate->wait();
        log.finish(name);
    }, true));
}

// What the device does with a plan: each pipeline the title creates gets its planned variants queued (here, in an
// order unlike the recorded one, as the title may create them).
void queue_plan(PipelineCompiler& pool, Log& log, const prewarm::Plan& plan, const std::vector<int>& created,
                Gate* first_gate = nullptr) {
    for (const int i : created)
        if (const auto* entries = plan.find(key(i)))
            for (const auto& e : *entries)
                submit(pool, log, e.rank == 0 ? first_gate : nullptr, &plan.by_key.at(key(i)),
                       key(i).substr(60) + "/" + std::to_string(e.variant), prewarm::priority(e.rank));
}

void test_prewarms_build_in_recorded_order() {
    const auto plan = prewarm::make_plan(
        session({record('g', 3, 0, 5), record('g', 1, 0, 5), record('g', 3, 4, 5), record('g', 2, 0, 5)}), vendor,
        device_id);
    auto& pool = make_pool(3);
    Log log;
    Gate busy;
    char title_slot = 0;
    submit(pool, log, &busy, &title_slot, "demand", 0);  // queued while a demanded build runs: none starts yet
    EXPECT(log.wait_started(1));
    queue_plan(pool, log, plan, {2, 1, 3});
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("0004/0"));
    busy.release();
    EXPECT(log.wait_finished(5));
    std::lock_guard lock(log.mutex);
    const std::vector<std::string> want{"demand", "0004/0", "0002/0", "0004/4", "0003/0"};
    EXPECT(log.started == want);
    pool.release();
}

void test_a_demanded_build_overtakes_the_prewarms() {
    const auto plan = prewarm::make_plan(
        session({record('g', 1, 0, 5), record('g', 2, 0, 5), record('g', 3, 0, 5)}), vendor, device_id);
    auto& pool = make_pool(2);
    Log log;
    Gate first, demand;
    queue_plan(pool, log, plan, {1, 2, 3}, &first);
    EXPECT(log.wait_started(1));  // the first prewarm runs, the others wait (one warm-up at a time)
    std::this_thread::sleep_for(50ms);
    EXPECT(!log.has_started("0003/0"));
    char title_slot = 0;
    submit(pool, log, &demand, &title_slot, "demand", 0);
    EXPECT(log.wait_started(2));  // on the free worker, while the prewarm runs
    EXPECT(log.has_started("demand"));
    first.release();
    EXPECT(log.wait_finished(1));
    std::this_thread::sleep_for(100ms);
    EXPECT(!log.has_started("0003/0") && !log.has_started("0004/0"));  // none while the demanded build runs
    demand.release();
    EXPECT(log.wait_finished(4));
    std::lock_guard lock(log.mutex);
    const std::vector<std::string> want{"0002/0", "demand", "0003/0", "0004/0"};
    EXPECT(log.started == want);
    pool.release();
}
}  // namespace

int main() {
    test_order_is_kept();
    test_the_largest_run_on_this_gpu_is_read();
    test_the_budget_is_respected();
    test_a_missing_or_garbled_log_is_ignored_with_a_message();
    test_prewarms_build_in_recorded_order();
    test_a_demanded_build_overtakes_the_prewarms();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("pipeline_prewarm_test: ok");
    return 0;
}
