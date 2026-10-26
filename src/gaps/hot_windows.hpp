#pragma once

// The superoptimizer on a profiled program: windows ranked by executions x cycles, the top
// ones searched, first exactly and then under the facts the profile saw on entry (the carry,
// register ranges, ranges of the bytes read), and the best non-overlapping results kept.

#include "gaps/profile.hpp"
#include "gaps/sites.hpp"
#include "superopt/search.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tisple {

struct WindowFinding {
    Window window;
    std::string original;
    std::string replacement;
    std::vector<SoIns> replacement_code;
    int cycles_before = 0;
    int cycles_after = 0;
    int bytes_before = 0;
    int bytes_after = 0;
    std::uint64_t count = 0; // executions of the window
    std::uint64_t saved = 0; // cycles in this run
    Proof proof = Proof::kNone;
    double input_bits = 0;
    std::string facts; // facts the replacement needs; empty when it needs none
    std::string function;
    std::string source;
};

struct HotWindowOptions {
    int window = 6; // longest window
    int top = 30;   // window starts searched, every length at each
    SearchOptions search;
    bool facts = true;
    std::uint64_t min_count = 16;
    std::uint64_t facts_min_count = 256; // fewer executions say too little to take as facts
    bool verbose = false;                // one line per search on stderr
    std::string only;                    // only windows in functions whose name contains this
};

struct HotWindowStats {
    std::uint64_t windows = 0;  // modelled windows that ran
    std::uint64_t searched = 0; // searches run (cache hits not counted)
    std::uint64_t cached = 0;
    std::uint64_t complete = 0; // searches that finished inside their budget
    std::uint64_t nodes = 0;
    double seconds = 0;
    double slowest = 0;
    int longest_complete = 0; // the longest window whose search finished
};

// results by window shape, so a shape met again (in this program or the next) is not
// searched twice; the search is deterministic, so this does not change any result
struct FactsSearch {
    SearchResult result; // found only when cheaper than the exact result
    std::string facts;   // the facts it needs
};
struct WindowCache {
    std::map<std::string, SearchResult> exact; // by WindowKey
    std::map<std::string, FactsSearch> facts;  // by WindowKey and the observed facts
};

std::vector<WindowFinding> SuperoptHotWindows(const Compiled& compiled, const SiteMap& map,
                                              const Profiler& p, const HotWindowOptions& options,
                                              WindowCache& cache, HotWindowStats& stats);

} // namespace tisple
