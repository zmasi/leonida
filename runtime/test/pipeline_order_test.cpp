// RT-PIPELINE-PREWARM-ORDER phase 1: the file format and the recorder of runtime/src/pipeline_order.h. No GPU, no
// game data: keys are made up here. Covered: the first request of a pipeline and variant is recorded and a repeated one
// is not; the order of asking is the order in the file; a truncated or damaged file is read as far as it goes; many
// threads at once record each pipeline once; the hot path allocates nothing; the file stays bounded; the counts line.
#include "../src/pipeline_order.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <new>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace po = leonida::pipeline_order;

// Counts the allocations made while `counting` is set, on any thread.
static std::atomic<bool> counting{false};
static std::atomic<long> allocations{0};

void* operator new(std::size_t size) {
    if (counting.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int failures = 0;
#define CHECK(condition)                                                          \
    do {                                                                          \
        if (!(condition)) {                                                       \
            std::fprintf(stderr, "line %d: %s\n", __LINE__, #condition);          \
            ++failures;                                                           \
        }                                                                         \
    } while (0)

namespace {

std::atomic<std::uint32_t> clock_ms{0};
std::uint32_t fake_now() { return clock_ms.load(); }

std::filesystem::path scratch(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "leonida_pipeline_order_test";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path).concat(".prev"), ec);
    return path;
}

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::string key_of(int n) {
    char text[80];
    std::snprintf(text, sizeof(text), "%064x", static_cast<unsigned>(n) * 2654435761u + 17u);
    return text;
}

po::Recorder::Config config(const std::filesystem::path& path, std::size_t capacity = 64) {
    po::Recorder::Config c;
    c.path = path;
    c.capacity = capacity;
    c.vendor = 0x10de;
    c.device = 0x2504;
    c.now_ms = &fake_now;
    return c;
}

// What the title's hook does for one request.
bool request(po::Recorder& r, int n, std::uint32_t variant = 0, char kind = 'c', bool ready = false,
             std::uint32_t created = 0, std::uint32_t build = po::unknown_ms) {
    const std::string key = key_of(n);
    const auto claim = r.claim(po::key_hash(key), variant);
    if (!claim) return false;
    r.publish(claim, key, kind, ready, created, build);
    return true;
}

void test_switch() {
    CHECK(!po::parse_enabled(nullptr));
    CHECK(!po::parse_enabled(""));
    CHECK(!po::parse_enabled("0"));
    CHECK(!po::parse_enabled("11"));
    CHECK(!po::parse_enabled("true"));
    CHECK(po::parse_enabled("1"));
}

void test_first_request_only() {
    const auto path = scratch("first.log");
    {
        clock_ms = 100;
        po::Recorder r(config(path));
        CHECK(request(r, 1));
        clock_ms = 200;
        CHECK(!request(r, 1));  // asked again: not recorded again
        CHECK(!request(r, 1));
        CHECK(request(r, 2));
        CHECK(!request(r, 2));
        CHECK(r.flush() == 2);
        CHECK(r.flush() == 0);  // nothing new
        CHECK(!request(r, 1));
        CHECK(r.flush() == 0);
    }
    const auto read = po::parse(slurp(path));
    CHECK(read.bad_lines == 0 && !read.cut_tail);
    CHECK(read.sessions.size() == 1);
    CHECK(read.sessions[0].vendor == 0x10de && read.sessions[0].device == 0x2504);
    CHECK(read.sessions[0].records.size() == 2);
    CHECK(read.sessions[0].records[0].key == key_of(1));
    CHECK(read.sessions[0].records[0].first_ms == 100);  // the time of the first ask, not of the later ones
}

void test_variants_are_distinct() {
    const auto path = scratch("variants.log");
    po::Recorder r(config(path));
    CHECK(request(r, 5, 0));
    CHECK(request(r, 5, 2));  // same pipeline, another variant: a record of its own
    CHECK(request(r, 5, 6));
    CHECK(!request(r, 5, 2));
    CHECK(request(r, 6, 2));  // same variant, another pipeline
    CHECK(r.flush() == 4);
    const auto read = po::parse(slurp(path));
    CHECK(read.sessions.size() == 1 && read.sessions[0].records.size() == 4);
    CHECK(read.sessions[0].records[1].variant == 2 && read.sessions[0].records[2].variant == 6);
}

void test_order_is_kept() {
    const auto path = scratch("order.log");
    std::vector<int> asked;
    std::mt19937 rng(7);
    {
        po::Recorder r(config(path, 600));
        std::vector<int> pool(500);
        for (int i = 0; i < 500; ++i) pool[std::size_t(i)] = i;
        std::shuffle(pool.begin(), pool.end(), rng);
        std::set<int> seen;
        for (int round = 0; round < 3; ++round)
            for (const int n : pool) {
                ++clock_ms;
                if (request(r, n)) asked.push_back(n);
                if (n % 50 == 0) r.flush();  // some lines go out in several flushes
            }
        r.flush();
    }
    CHECK(asked.size() == 500);
    const auto read = po::parse(slurp(path));
    CHECK(read.sessions.size() == 1 && read.sessions[0].records.size() == 500);
    bool same = read.sessions[0].records.size() == asked.size();
    for (std::size_t i = 0; same && i < asked.size(); ++i) same = read.sessions[0].records[i].key == key_of(asked[i]);
    CHECK(same);
    std::uint32_t last = 0;
    bool monotonic = true;
    for (const auto& rec : read.sessions[0].records) {
        monotonic = monotonic && rec.first_ms >= last;
        last = rec.first_ms;
    }
    CHECK(monotonic);
}

void test_fields_round_trip() {
    po::Record a;
    a.kind = 'm';
    a.key = key_of(3);
    a.variant = 0x2a0;
    a.created_ms = 12;
    a.first_ms = 3456;
    a.build_ms = 789;
    a.ready = true;
    po::Record b = a;
    b.kind = 'g';
    b.key = key_of(4);
    b.build_ms = po::unknown_ms;
    b.ready = false;
    std::string text = po::format_header() + po::format_session(0x1002, 0x73bf) + po::format_record(a) +
                       po::format_record(b) + po::format_build(b.key, b.variant, 4000);
    const auto read = po::parse(text);
    CHECK(read.bad_lines == 0 && !read.cut_tail && read.sessions.size() == 1);
    CHECK(read.sessions[0].vendor == 0x1002 && read.sessions[0].device == 0x73bf);
    CHECK(read.sessions[0].records.size() == 2);
    CHECK(read.sessions[0].records[0] == a);
    b.build_ms = 4000;  // the late B line completes the record of the same key and variant
    CHECK(read.sessions[0].records[1] == b);
    // A B line for a pipeline that was never recorded is dropped, not invented.
    const auto stray = po::parse(po::format_session(1, 2) + po::format_build(key_of(99), 0, 5));
    CHECK(stray.sessions.size() == 1 && stray.sessions[0].records.empty());
}

// request(): the callbacks run for the first request only, ready before build, both after the claim.
void test_request_helper() {
    const auto path = scratch("helper.log");
    po::Recorder r(config(path));
    int ready_calls = 0, build_calls = 0;
    std::string order;
    const auto ready = [&] {
        ++ready_calls;
        order += 'r';
        return true;
    };
    const auto build = [&] {
        ++build_calls;
        order += 'b';
        return std::uint32_t(77);
    };
    const std::string key = key_of(1);
    const auto hash = po::key_hash(key);
    CHECK(r.request(hash, key, 0, 'c', 3, ready, build));
    CHECK(!r.request(hash, key, 0, 'c', 3, ready, build));
    CHECK(!r.request(hash, key, 0, 'c', 3, ready, build));
    CHECK(r.request(hash, key, 2, 'c', 3, ready, build));
    CHECK(ready_calls == 2 && build_calls == 2 && order == "rbrb");
    CHECK(r.flush() == 2);
    const auto read = po::parse(slurp(path));
    CHECK(read.sessions.size() == 1 && read.sessions[0].records.size() == 2);
    CHECK(read.sessions[0].records[0].ready && read.sessions[0].records[0].build_ms == 77 &&
          read.sessions[0].records[0].created_ms == 3 && read.sessions[0].records[0].kind == 'c');
    // The hot path with callbacks does not allocate either.
    allocations = 0;
    counting = true;
    for (int i = 0; i < 100; ++i) r.request(hash, key, 0, 'c', 3, ready, build);
    counting = false;
    CHECK(allocations.load() == 0);
}

void test_late_build() {
    const auto path = scratch("late.log");
    po::Recorder r(config(path));
    // A build time known when the record is made is in its line; one that arrives later is a B line.
    CHECK(request(r, 1, 0, 'c', true, 5, 40));
    CHECK(request(r, 2, 0, 'g', false, 5));
    CHECK(r.flush() == 2);
    r.note_build(po::key_hash(key_of(2)), 0, 1500);
    r.note_build(po::key_hash(key_of(2)), 0, 9999);  // a second time is ignored
    r.note_build(po::key_hash(key_of(77)), 0, 5);    // never asked for: ignored
    CHECK(r.flush() == 1);
    CHECK(r.flush() == 0);
    // A build that ends between the claim and the publish is not lost.
    const std::string key = key_of(3);
    const auto claim = r.claim(po::key_hash(key), 4);
    CHECK(claim);
    r.note_build(po::key_hash(key), 4, 321);
    r.publish(claim, key, 'g', false, 1, po::unknown_ms);
    CHECK(r.flush() == 1);
    const auto read = po::parse(slurp(path));
    CHECK(read.bad_lines == 0 && read.sessions.size() == 1 && read.sessions[0].records.size() == 3);
    CHECK(read.sessions[0].records[0].build_ms == 40 && read.sessions[0].records[0].ready);
    CHECK(read.sessions[0].records[1].build_ms == 1500);
    CHECK(read.sessions[0].records[2].build_ms == 321);
}

// The file of a few records, cut at every byte: each cut reads as far as it goes, never more, never an error.
void test_truncated() {
    std::string text = po::format_header() + po::format_session(0x10de, 0x2504);
    std::vector<std::size_t> ends;
    for (int i = 0; i < 6; ++i) {
        po::Record r;
        r.key = key_of(i);
        r.variant = std::uint32_t(i);
        r.first_ms = std::uint32_t(100 * i);
        r.build_ms = std::uint32_t(i);
        text += po::format_record(r);
        ends.push_back(text.size());
    }
    for (std::size_t cut = 0; cut <= text.size(); ++cut) {
        const auto read = po::parse(std::string_view(text).substr(0, cut));
        std::size_t whole = 0;
        for (const auto end : ends) whole += cut >= end;
        std::size_t records = 0;
        for (const auto& s : read.sessions) records += s.records.size();
        CHECK(records == whole);
        CHECK(read.bad_lines == 0);
        const bool at_line_end = cut == 0 || text[cut - 1] == '\n';
        CHECK(read.cut_tail == !at_line_end);
    }
}

void test_damaged() {
    std::string text = po::format_session(0x10de, 0x2504);
    std::vector<std::pair<std::size_t, std::size_t>> lines;  // [begin, end) of each record line
    for (int i = 0; i < 5; ++i) {
        po::Record r;
        r.key = key_of(i);
        r.first_ms = std::uint32_t(i);
        const std::size_t begin = text.size();
        text += po::format_record(r);
        lines.push_back({begin, text.size()});
    }
    // One byte changed anywhere in the third line: that line goes, the four others are kept.
    for (std::size_t at = lines[2].first; at + 1 < lines[2].second; ++at) {
        for (const char replacement : {'X', '0', ' '}) {
            std::string damaged = text;
            if (damaged[at] == replacement) continue;
            damaged[at] = replacement;
            const auto read = po::parse(damaged);
            CHECK(read.sessions.size() == 1);
            std::size_t records = read.sessions.empty() ? 0 : read.sessions[0].records.size();
            CHECK(records == 4);
            CHECK(read.bad_lines == 1);
            for (const auto& rec : read.sessions.empty() ? std::vector<po::Record>{} : read.sessions[0].records)
                CHECK(rec.key != key_of(2));
        }
    }
    // Garbage between good lines, a line without a checksum, an empty file, binary noise, Windows line ends.
    const auto noisy = po::parse(text.substr(0, lines[1].second) + "garbage\n\n*\n P x\n" + text.substr(lines[1].second));
    CHECK(noisy.sessions.size() == 1 && noisy.sessions[0].records.size() == 5 && noisy.bad_lines == 3);
    CHECK(po::parse("").sessions.empty());
    std::mt19937 rng(11);
    for (int round = 0; round < 200; ++round) {
        std::string noise(std::size_t(rng() % 600), '\0');
        for (char& c : noise) c = char(rng());
        po::parse(noise);  // must not crash or throw
        std::string mixed = text;
        for (int k = 0; k < 6; ++k) mixed[rng() % mixed.size()] = char(rng());
        po::parse(mixed);
    }
    std::string crlf;
    for (const char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    const auto windows = po::parse(crlf);
    CHECK(windows.sessions.size() == 1 && windows.sessions[0].records.size() == 5 && windows.bad_lines == 0);
}

void test_sessions_append() {
    const auto path = scratch("sessions.log");
    for (int run = 0; run < 3; ++run) {
        po::Recorder r(config(path));
        CHECK(request(r, run));       // different in each run
        CHECK(request(r, 100));       // the same in each run: recorded in each
        CHECK(r.flush() == 2);
    }
    const std::string text = slurp(path);
    CHECK(text.rfind("# leonida pipeline order 1\n", 0) == 0);
    CHECK(text.find("# leonida", 5) == std::string::npos);  // the header once
    const auto read = po::parse(text);
    CHECK(read.sessions.size() == 3 && read.bad_lines == 0);
    for (const auto& s : read.sessions) CHECK(s.records.size() == 2 && s.records[1].key == key_of(100));
}

void test_full_record() {
    const auto path = scratch("full.log");
    po::Recorder r(config(path, 100));
    int kept = 0;
    for (int n = 0; n < 300; ++n) kept += request(r, n);
    CHECK(kept == 100);
    for (int n = 0; n < 300; ++n) CHECK(!request(r, n));  // the full record neither grows nor fails
    CHECK(r.flush() == 100);
    const auto counts = r.counts();
    CHECK(counts.requested == 100);
    CHECK(counts.overflow >= 200);
    CHECK(po::parse(slurp(path)).sessions[0].records.size() == 100);
    CHECK(r.close().find("not recorded") != std::string::npos);
}

void test_file_is_bounded() {
    const auto path = scratch("bounded.log");
    auto c = config(path);
    c.max_file_bytes = 600;
    for (int run = 0; run < 6; ++run) {
        po::Recorder r(c);
        for (int n = 0; n < 5; ++n) request(r, run * 10 + n);
        CHECK(r.flush() == 5);
    }
    // A file past the limit is set aside as .prev and a new one is started: never more than two files of about the limit.
    CHECK(std::filesystem::exists(std::filesystem::path(path).concat(".prev")));
    CHECK(std::filesystem::file_size(path) < 600 + 1000);
    CHECK(std::filesystem::file_size(std::filesystem::path(path).concat(".prev")) < 600 + 1000);
    CHECK(po::parse(slurp(path)).bad_lines == 0);
}

void test_unwritable() {
    // A directory where the file should be: the open fails, the recorder says so and the hot path goes on.
    const auto dir = std::filesystem::temp_directory_path() / "leonida_pipeline_order_test" / "isdir.log";
    std::filesystem::create_directories(dir);
    po::Recorder r(config(dir));
    CHECK(request(r, 1));
    CHECK(r.flush() == -1);
    CHECK(r.failed());
    CHECK(request(r, 2));
    CHECK(r.flush() == -1);
    CHECK(!r.close().empty());
}

void test_concurrent() {
    const auto path = scratch("threads.log");
    constexpr int threads = 8, distinct = 1500, variants = 2;
    po::Recorder r(config(path, 4096));
    r.start(std::chrono::milliseconds(2));  // the writer runs while the threads ask
    std::atomic<int> recorded{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&, t] {
            std::mt19937 rng(unsigned(t) + 1);
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < distinct * variants * 3; ++i) {
                const int n = int(rng() % distinct);
                const std::uint32_t variant = (rng() % variants) * 2;
                const std::string key = key_of(n);
                const auto claim = r.claim(po::key_hash(key), variant);
                if (!claim) continue;
                ++recorded;
                r.publish(claim, key, 'g', false, 1, po::unknown_ms);
                if (i % 7 == 0) r.note_build(po::key_hash(key), variant, std::uint32_t(i));
            }
        });
    go = true;
    for (auto& th : pool) th.join();
    r.close();
    const auto read = po::parse(slurp(path));
    CHECK(read.bad_lines == 0 && !read.cut_tail && read.sessions.size() == 1);
    std::set<std::pair<std::string, std::uint32_t>> seen;
    std::size_t duplicates = 0;
    for (const auto& rec : read.sessions[0].records) duplicates += !seen.insert({rec.key, rec.variant}).second;
    CHECK(duplicates == 0);                                       // each pipeline and variant once, whoever asked first
    CHECK(int(read.sessions[0].records.size()) == recorded.load());  // and every one that was claimed reached the file
    CHECK(recorded.load() > distinct * variants * 9 / 10);        // almost all of the 3000 were asked at least once
    CHECK(recorded.load() <= distinct * variants);
}

// The hot path (claim, publish, note_build) does not allocate, on a first request or a repeated one.
void test_no_allocation() {
    const auto path = scratch("alloc.log");
    po::Recorder r(config(path, 2000));
    std::vector<std::string> keys;
    for (int n = 0; n < 1000; ++n) keys.push_back(key_of(n));
    allocations = 0;
    counting = true;
    for (int round = 0; round < 3; ++round)
        for (const auto& key : keys) {
            const auto hash = po::key_hash(key);
            const auto claim = r.claim(hash, 0);
            if (claim) r.publish(claim, key, 'c', false, 0, po::unknown_ms);
            r.note_build(hash, 0, 5);
        }
    counting = false;
    CHECK(allocations.load() == 0);
    CHECK(r.counts().requested == 1000);
}

// The writer thread alone puts a record in the file, and stop() leaves nothing running.
void test_thread_writes() {
    const auto path = scratch("thread.log");
    po::Recorder r(config(path));
    r.start(std::chrono::milliseconds(5));
    r.start(std::chrono::milliseconds(5));  // idempotent
    CHECK(request(r, 1));
    for (int i = 0; i < 400 && slurp(path).find("\nP ") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CHECK(po::parse(slurp(path)).sessions.size() == 1);
    CHECK(!po::parse(slurp(path)).sessions.empty() && po::parse(slurp(path)).sessions[0].records.size() == 1);
    r.stop();
    r.stop();
}

void test_counts_and_report() {
    const auto path = scratch("counts.log");
    po::Recorder r(config(path));
    // created, first use: leads 10, 20, 30, 40, 1000 (median 30), two ready out of five.
    const std::uint32_t leads[] = {40, 10, 1000, 30, 20};
    for (int i = 0; i < 5; ++i) {
        clock_ms = 5000;
        request(r, i, 0, 'c', i < 2, 5000 - leads[i]);
    }
    request(r, 2);  // a repeat changes nothing
    const auto c = r.counts();
    CHECK(c.requested == 5 && c.ready == 2 && c.overflow == 0 && c.median_lead_ms == 30);
    // A first use before the creation time (clocks, a restart of the counter) is a lead of 0, not a huge number.
    clock_ms = 10;
    request(r, 50, 0, 'c', false, 900);
    CHECK(r.counts().requested == 6);
    const std::string line = r.close();
    CHECK(line.find("requested 6") != std::string::npos);
    CHECK(line.find("already ready 2") != std::string::npos);
    CHECK(line.find("median lead 20 ms") != std::string::npos);  // leads 0 10 20 30 40 1000: the lower middle one
    CHECK(r.close().empty());                                    // once
    CHECK(slurp(path).find("\nP ") != std::string::npos);        // close() wrote the rest
}

void test_key_is_one_word() {
    const auto path = scratch("word.log");
    po::Recorder r(config(path));
    const std::string odd = "a b\nc*d" + std::string(100, 'z');
    const auto claim = r.claim(po::key_hash(odd), 0);
    CHECK(claim);
    r.publish(claim, odd, 'c', false, 0, po::unknown_ms);
    CHECK(r.flush() == 1);
    const auto read = po::parse(slurp(path));
    CHECK(read.bad_lines == 0 && read.sessions.size() == 1 && read.sessions[0].records.size() == 1);
    CHECK(read.sessions[0].records[0].key.size() == po::key_chars);
    CHECK(read.sessions[0].records[0].key.rfind("a_b_c_d", 0) == 0);
}

// A process killed in the middle of a line leaves no newline; the next session must not stick to the cut line.
void test_cut_file_then_new_session() {
    const auto path = scratch("cutfile.log");
    {
        po::Recorder r(config(path));
        CHECK(request(r, 1));
        CHECK(request(r, 2));
        CHECK(r.flush() == 2);
    }
    std::string text = slurp(path);
    CHECK(!text.empty() && text.back() == '\n');
    text.resize(text.size() - 7);  // the last record line cut short, no newline
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    {
        po::Recorder r(config(path));
        CHECK(request(r, 3));
        CHECK(request(r, 4));
        CHECK(r.flush() == 2);
    }
    const auto read = po::parse(slurp(path));
    CHECK(read.sessions.size() == 2);
    if (read.sessions.size() == 2) {
        CHECK(read.sessions[0].records.size() == 1 && read.sessions[0].records[0].key == key_of(1));
        CHECK(read.sessions[1].records.size() == 2);
        if (read.sessions[1].records.size() == 2) {
            CHECK(read.sessions[1].records[0].key == key_of(3));
            CHECK(read.sessions[1].records[1].key == key_of(4));
        }
    }
    CHECK(read.bad_lines == 1);  // the cut line itself, skipped
}

// The writer stops at a slot still being filled, so the file keeps the order of asking.
void test_flush_stops_at_unpublished() {
    const auto path = scratch("holdback.log");
    po::Recorder r(config(path));
    const std::string ka = key_of(10), kb = key_of(11);
    const auto a = r.claim(po::key_hash(ka), 0);
    const auto b = r.claim(po::key_hash(kb), 0);
    CHECK(a && b);
    r.publish(b, kb, 'c', false, 0, po::unknown_ms);
    CHECK(r.flush() == 0);  // B is published but A, asked first, is not yet
    r.publish(a, ka, 'c', false, 0, po::unknown_ms);
    CHECK(r.flush() == 2);
    const auto read = po::parse(slurp(path));
    CHECK(read.sessions.size() == 1 && read.sessions[0].records.size() == 2);
    if (read.sessions.size() == 1 && read.sessions[0].records.size() == 2) {
        CHECK(read.sessions[0].records[0].key == ka);
        CHECK(read.sessions[0].records[1].key == kb);
    }
}

// A session line of a version this reader does not know is not read as one of this one.
void test_unknown_session_version() {
    const std::string known = po::format_session(0x10de, 0x2504);
    CHECK(po::parse(known).sessions.size() == 1);
    // The same line with version 2 and a good checksum: the checksum is the one of the text before " *".
    std::string body = "S 2 10de 2504";
    char sum[16];
    std::snprintf(sum, sizeof(sum), " *%08x\n", po::checksum(body));
    const auto read = po::parse(body + sum);
    CHECK(read.sessions.empty());
    CHECK(read.bad_lines == 1);
}

// --- E lines (the names are made up) ---

std::uint64_t hash_of(const char* name) { return po::entry_name_hash(name); }

// A log as the recorder writes it for compute pipelines: a P line and its E line for each name, in this order.
std::string log_of(const std::vector<const char*>& names) {
    std::string text = po::format_header() + po::format_session(0x10de, 0x2504);
    int n = 0;
    for (const char* name : names) {
        po::Record r;
        r.key = key_of(++n);
        r.first_ms = std::uint32_t(n);
        text += po::format_record(r);
        text += po::format_entry(r.key, 0, hash_of(name));
    }
    return text;
}

void test_entry_line_format() {
    // "E <key> <variant hex> <16 hex digits> *<checksum>", the checksum over the text before " *".
    const std::string line = po::format_entry(key_of(1), 0x1f, 0x0123456789abcdefull);
    const std::string body = "E " + key_of(1) + " 1f 0123456789abcdef";
    char sum[16];
    std::snprintf(sum, sizeof(sum), " *%08x\n", po::checksum(body));
    CHECK(line == body + sum);
    const auto read = po::parse(line);
    CHECK(read.bad_lines == 0 && read.sessions.empty());
    CHECK(read.entries.size() == 1);
    CHECK(read.entries[0].key == key_of(1) && read.entries[0].variant == 0x1f &&
          read.entries[0].name_hash == 0x0123456789abcdefull);
    // The hash is a stable function of the name, and always 16 digits.
    CHECK(hash_of("blur_a") == hash_of("blur_a") && hash_of("blur_a") != hash_of("blur_b"));
    // The disk format: FNV-1a 64 of the name, so the hash never changes under old logs.
    CHECK(po::entry_name_hash("a") == 0xaf63dc4c8601ec8cull);
    CHECK(po::format_entry(key_of(1), 0, 1).find(" 0000000000000001 ") != std::string::npos);
}

// A reader from before E lines skips them (counted as lines not read) and keeps the same sessions and records; this
// reader reads them and counts no damaged line.
void test_entry_lines_are_skipped_by_the_record_reader() {
    const auto with = po::parse(log_of({"blur_a", "sky_c", "tile_d"}));
    std::string without = po::format_header() + po::format_session(0x10de, 0x2504);
    for (int n = 1; n <= 3; ++n) {
        po::Record r;
        r.key = key_of(n);
        r.first_ms = std::uint32_t(n);
        without += po::format_record(r);
    }
    const auto plain = po::parse(without);
    CHECK(with.bad_lines == 0 && !with.cut_tail);
    CHECK(with.sessions.size() == 1 && plain.sessions.size() == 1);
    CHECK(with.sessions[0].records == plain.sessions[0].records);
    CHECK(with.sessions[0].records.size() == 3);
    CHECK(with.entries.size() == 3);
}

void test_malformed_entry_lines() {
    const auto bad = [](const std::string& body) {
        char sum[16];
        std::snprintf(sum, sizeof(sum), " *%08x\n", po::checksum(body));
        const auto read = po::parse(body + sum);
        return read.entries.empty() && read.bad_lines == 1;
    };
    const std::string k = key_of(1);
    CHECK(bad("E " + k + " 0 0123456789abcde"));     // 15 digits
    CHECK(bad("E " + k + " 0 0123456789abcdef0"));   // 17 digits
    CHECK(bad("E " + k + " 0 0123456789ABCDEF"));    // upper case
    CHECK(bad("E " + k + " 0 0123456789abcdeg"));    // not hex
    CHECK(bad("E " + k + " zz 0123456789abcdef"));   // the variant
    CHECK(bad("E " + k + " 0"));                     // no hash
    CHECK(bad("E " + k + " 0 0123456789abcdef x"));  // an extra field
    CHECK(bad("E  0 0123456789abcdef"));             // no key
    CHECK(!bad("E " + k + " 0 0123456789abcdef"));   // the good one
    // A good line with a wrong checksum is damaged too.
    std::string line = po::format_entry(k, 0, 5);
    char& last_digit = line[line.size() - 2];
    last_digit = last_digit == '0' ? '1' : '0';
    const auto read = po::parse(line);
    CHECK(read.entries.empty() && read.bad_lines == 1);
}

// The recorder writes the E line right after the P line of a compute pipeline, and none for a pipeline without a name.
void test_recorder_writes_entry_lines() {
    const auto path = scratch("entry.log");
    {
        po::Recorder r(config(path));
        const std::string k1 = key_of(1), k2 = key_of(2);
        r.publish(r.claim(po::key_hash(k1), 0), k1, 'c', false, 0, po::unknown_ms, hash_of("blur_a"));
        r.publish(r.claim(po::key_hash(k2), 5), k2, 'g', false, 0, po::unknown_ms);
        CHECK(r.flush() == 3);  // P, E, P
    }
    const std::string text = slurp(path);
    const auto read = po::parse(text);
    CHECK(read.bad_lines == 0);
    CHECK(read.sessions.size() == 1 && read.sessions[0].records.size() == 2);
    CHECK(read.entries.size() == 1);
    CHECK(read.entries[0].key == key_of(1) && read.entries[0].name_hash == hash_of("blur_a"));
    CHECK(text.find("\nE " + key_of(1)) != std::string::npos);
    CHECK(text.find("\nE " + key_of(2)) == std::string::npos);
    // The E line comes after the P line of its key and before the next P line.
    CHECK(text.find("\nP c " + key_of(1)) < text.find("\nE " + key_of(1)));
    CHECK(text.find("\nE " + key_of(1)) < text.find("\nP g " + key_of(2)));
}

}  // namespace

int main() {
    test_switch();
    test_first_request_only();
    test_variants_are_distinct();
    test_order_is_kept();
    test_fields_round_trip();
    test_request_helper();
    test_late_build();
    test_truncated();
    test_damaged();
    test_sessions_append();
    test_full_record();
    test_file_is_bounded();
    test_unwritable();
    test_concurrent();
    test_no_allocation();
    test_thread_writes();
    test_counts_and_report();
    test_key_is_one_word();
    test_cut_file_then_new_session();
    test_flush_stops_at_unpublished();
    test_unknown_session_version();
    test_entry_line_format();
    test_entry_lines_are_skipped_by_the_record_reader();
    test_malformed_entry_lines();
    test_recorder_writes_entry_lines();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("pipeline_order_test: ok");
    return 0;
}
