// RT-PIPELINE-PREWARM-ORDER, phase 2 (in-game): build at boot, ahead of their first draw, the pipelines an earlier run
// asked for first, in the order it asked for them. Off unless LEONIDA_PIPELINE_PREWARM_ORDER names an order log
// (pipeline_order.h, written with LEONIDA_PIPELINE_ORDER_LOG=1); "1" names pipeline_order.log beside the session cache.
//
// The plan is read once, when the device starts. A pipeline the title creates whose description key is in it gets its
// recorded variants queued on the compile workers (d3d12_pipeline_compile.h) as warm-ups, at a priority that keeps the
// recorded order: below every demanded build, and one warm-up at a time, none while a demanded build runs.
//
// Which run: of the sessions recorded on this GPU (vendor and device id), the one with the most records, the newest of
// those on a tie. A file from another GPU, a missing or unreadable file and a file with no usable record are ignored
// with one line on stderr; a damaged line is skipped (pipeline_order::parse), as are line types a v1 reader does not
// know (MoonEyes' `E` lines for the offline precompiler).
//
// Graphics and mesh records only: compute pipelines already have their own warm-up by module size, and the offline
// precompiler takes their recorded order (gta6rt --precompile-pipelines --order-file).
//
// The budget is the build time the earlier run recorded: records are taken in order until their build times add up to
// LEONIDA_PIPELINE_PREWARM_BUDGET_MS (default 300000), and at most LEONIDA_PIPELINE_PREWARM_MAX_ENTRIES (default 4096).
// A record with no build time counts as unknown_build_ms.
#pragma once

#include "pipeline_order.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace leonida::pipeline_prewarm {

inline constexpr std::uint64_t default_budget_ms = 300000;
inline constexpr std::size_t default_max_entries = 4096;
inline constexpr std::uint32_t unknown_build_ms = 1000;  // what a record with no build time counts against the budget

struct Options {
    std::uint64_t budget_ms = default_budget_ms;
    std::size_t max_entries = default_max_entries;
};

// One variant of one pipeline to build, and where it came in the recorded order (0 first).
struct Entry {
    std::uint32_t rank = 0;
    std::uint32_t variant = 0;
    char kind = 'g';
};

// The compile-worker priority of a prewarm: after demanded builds (0), in recorded order. Compute warm-ups use their
// module size (some hundred KiB), so prewarms mostly go first; the checkpoint (SIZE_MAX) stays last.
inline std::size_t priority(std::uint32_t rank) { return std::size_t{1} + rank; }

struct Plan {
    std::unordered_map<std::string, std::vector<Entry>> by_key;  // description key -> its variants, in recorded order
    std::size_t entries = 0;          // variants planned
    std::size_t session_records = 0;  // records of the session read
    std::size_t compute_skipped = 0;  // compute records left to the size-ordered warm-up
    std::size_t over_budget = 0;      // graphics and mesh records past the budget or the entry limit
    std::uint64_t budget_used_ms = 0; // recorded build time of the planned ones
    std::size_t bad_lines = 0;        // damaged or unknown lines skipped
    std::string message;              // the one line for stderr: what was read, or why nothing is prewarmed

    explicit operator bool() const { return entries != 0; }
    const std::vector<Entry>* find(std::string_view key) const {
        const auto it = by_key.find(std::string(key));
        return it == by_key.end() ? nullptr : &it->second;
    }
};

// Drops the lines a v1 reader does not know but that are not damage: `E` lines (the library entry of a record, for the
// offline precompiler). pipeline_order::parse would count them as bad lines.
inline std::string without_unknown_lines(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        const bool last = end == std::string_view::npos;
        if (last) end = text.size();
        const std::string_view line = text.substr(at, end - at);
        if (!(line.size() >= 2 && line[0] == 'E' && line[1] == ' ')) {
            out += line;
            if (!last) out += '\n';
        }
        at = last ? end : end + 1;
    }
    return out;
}

// The plan from the text of an order log, for the GPU with this vendor and device id. `source` names the file in the
// message. Never throws on any text.
inline Plan make_plan(std::string_view text, std::uint32_t vendor, std::uint32_t device, const Options& options = {},
                      std::string_view source = "the order log") {
    Plan plan;
    const auto read = pipeline_order::parse(without_unknown_lines(text));
    plan.bad_lines = read.bad_lines;
    const pipeline_order::Session* chosen = nullptr;
    std::size_t other_gpu = 0;
    for (const auto& session : read.sessions) {
        if (session.records.empty()) continue;
        if (session.vendor != vendor || session.device != device) {
            ++other_gpu;
            continue;
        }
        if (!chosen || session.records.size() >= chosen->records.size()) chosen = &session;
    }
    const std::string head = "[rt] pipeline-prewarm: ", where(source);
    if (!chosen) {
        plan.message = head + "nothing to prewarm from " + where + ": " +
                       (other_gpu ? std::to_string(other_gpu) + " recorded run(s), none on this GPU"
                                  : std::string("no readable record")) +
                       (plan.bad_lines ? " (" + std::to_string(plan.bad_lines) + " damaged line(s) skipped)" : "");
        return plan;
    }
    plan.session_records = chosen->records.size();
    std::unordered_set<std::string> seen;  // key + variant: a record repeated in a session is planned once
    std::uint32_t rank = 0;
    for (const auto& record : chosen->records) {
        if (record.kind == 'c') {
            ++plan.compute_skipped;
            continue;
        }
        if (!seen.insert(record.key + '/' + std::to_string(record.variant)).second) continue;
        const std::uint64_t cost = record.build_ms == pipeline_order::unknown_ms ? unknown_build_ms : record.build_ms;
        if (plan.entries >= options.max_entries || plan.budget_used_ms + cost > options.budget_ms) {
            ++plan.over_budget;
            continue;
        }
        plan.budget_used_ms += cost;
        plan.by_key[record.key].push_back({rank++, record.variant, record.kind});
        ++plan.entries;
    }
    plan.message = head + std::to_string(plan.entries) + " graphics variant(s) of " +
                   std::to_string(plan.by_key.size()) + " pipeline(s) to prewarm from " + where + " (a run of " +
                   std::to_string(plan.session_records) + " record(s); " + std::to_string(plan.budget_used_ms) +
                   " ms recorded build time; " + std::to_string(plan.over_budget) + " over the budget; " +
                   std::to_string(plan.compute_skipped) + " compute left to the warm-up";
    if (plan.bad_lines) plan.message += "; " + std::to_string(plan.bad_lines) + " damaged line(s) skipped";
    plan.message += ")";
    return plan;
}

// Reads the file and makes the plan. A file that cannot be read gives an empty plan with the reason.
inline Plan load_plan(const std::filesystem::path& path, std::uint32_t vendor, std::uint32_t device,
                      const Options& options = {}) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        Plan plan;
        plan.message = "[rt] pipeline-prewarm: cannot read " + path.string() + "; nothing is prewarmed";
        return plan;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return make_plan(text.str(), vendor, device, options, path.string());
}

// A positive whole number, or `fallback` for unset, empty, zero, or anything else.
inline std::uint64_t parse_count(const char* text, std::uint64_t fallback) {
    if (!text || !*text) return fallback;
    std::uint64_t v = 0;
    for (const char* p = text; *p; ++p) {
        if (*p < '0' || *p > '9' || v > (UINT64_MAX - 9) / 10) return fallback;
        v = v * 10 + std::uint64_t(*p - '0');
    }
    return v ? v : fallback;
}

}  // namespace leonida::pipeline_prewarm
