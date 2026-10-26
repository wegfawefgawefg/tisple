#include "gaps/hot_windows.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

namespace {

struct Candidate {
    std::uint64_t potential = 0; // executions x cycles
    std::size_t run = 0;
    std::size_t start = 0;
    std::size_t len = 0;
};

// what the profile saw on the window's entry, as facts
Facts ObservedFacts(const Window& w, const std::vector<RawIns>& raw, const Profiler& p) {
    Facts f;
    const SiteStats& entry = p.sites[raw[0].address];
    if ((entry.p_and & kFlagC) != 0) {
        f.lo[5] = 1;
    } else if ((entry.p_or & kFlagC) == 0) {
        f.hi[5] = 0;
    }
    f.lo[0] = entry.a_min;
    f.hi[0] = entry.a_max;
    f.lo[1] = entry.x_min;
    f.hi[1] = entry.x_max;
    f.lo[2] = entry.y_min;
    f.hi[2] = entry.y_max;
    // a byte read before the window writes it: the range its first reader saw
    std::vector<bool> written(w.cells.size(), false);
    for (std::size_t i = 0; i < w.code.size(); i++) {
        const SoIns& ins = w.code[i];
        if (ins.cell < 0) {
            continue;
        }
        std::size_t c = static_cast<std::size_t>(ins.cell);
        LocEffects e = EffectsOf(ins.mn, ins.mode);
        const SiteStats& s = p.sites[raw[i].address];
        if (e.mem_read && !written[c] && s.m_min <= s.m_max) {
            f.lo[7 + c] = s.m_min;
            f.hi[7 + c] = s.m_max;
        }
        if (e.mem_write) {
            written[c] = true;
        }
    }
    return f;
}

bool Overlaps(const WindowFinding& a, const WindowFinding& b) {
    std::uint32_t a_end = a.window.address + static_cast<std::uint32_t>(a.bytes_before);
    std::uint32_t b_end = b.window.address + static_cast<std::uint32_t>(b.bytes_before);
    return a.window.address < b_end && b.window.address < a_end;
}

void Note(HotWindowStats& stats, const SearchResult& r, const Window& w, bool facts,
          bool verbose) {
    int len = static_cast<int>(w.code.size());
    if (verbose) {
        std::fprintf(stderr, "search %d ins %d cycles%s: %s; %llu nodes, %s, %.3f s%s%s\n", len,
                     w.cycles, facts ? " (facts)" : "", CodeText(w.code, w.cells.data()).c_str(),
                     static_cast<unsigned long long>(r.nodes), r.complete ? "complete" : "budget",
                     r.seconds, r.found ? " => " : "",
                     r.found ? CodeText(r.code, w.cells.data()).c_str() : "");
    }
    stats.searched++;
    stats.nodes += r.nodes;
    stats.seconds += r.seconds;
    stats.slowest = r.seconds > stats.slowest ? r.seconds : stats.slowest;
    if (r.complete) {
        stats.complete++;
        stats.longest_complete = len > stats.longest_complete ? len : stats.longest_complete;
    }
}

CachedSearch SearchWindow(const Window& w, const Facts& observed, const HotWindowOptions& options,
                          HotWindowStats& stats) {
    CachedSearch out;
    SearchOptions so = options.search;
    so.max_len = std::min(so.max_len, static_cast<int>(w.code.size()));
    out.strict = Superoptimize(w, Facts(), so);
    Note(stats, out.strict, w, false, options.verbose);
    if (!options.facts || !HasFacts(observed)) {
        return out;
    }
    SearchResult with = Superoptimize(w, observed, so);
    Note(stats, with, w, true, options.verbose);
    bool cheaper = with.found && (!out.strict.found || with.cycles < out.strict.cycles);
    if (!cheaper) {
        return out;
    }
    Facts needed = NeededFacts(w, with.code, observed, so);
    out.with_facts = with;
    out.facts = FactsText(w, needed);
    return out;
}

WindowFinding MakeFinding(const Compiled& compiled, const SiteMap& map, const Window& w,
                          const SearchResult& r, const std::string& facts, std::uint64_t count) {
    WindowFinding f;
    f.window = w;
    f.original = CodeText(w.code, w.cells.data());
    f.replacement = CodeText(r.code, w.cells.data());
    f.replacement_code = r.code;
    f.cycles_before = w.cycles;
    f.cycles_after = r.cycles;
    f.bytes_before = w.bytes;
    f.bytes_after = r.bytes;
    f.count = count;
    f.saved = count * static_cast<std::uint64_t>(w.cycles - r.cycles);
    f.proof = r.proof;
    f.input_bits = r.input_bits;
    f.facts = facts;
    f.function = SiteFunction(compiled, map, w.address);
    f.source = SiteSource(compiled, map, w.address);
    return f;
}

} // namespace

std::vector<WindowFinding> SuperoptHotWindows(const Compiled& compiled, const SiteMap& map,
                                              const Profiler& p, const HotWindowOptions& options,
                                              WindowCache& cache, HotWindowStats& stats) {
    std::vector<RawRun> runs = StraightRuns(compiled);
    // window starts, hottest first: executions x the cycles of the longest window there
    std::vector<Candidate> starts;
    for (std::size_t r = 0; r < runs.size(); r++) {
        const std::vector<RawIns>& ins = runs[r].ins;
        for (std::size_t start = 0; start + 1 < ins.size(); start++) {
            std::uint64_t count = p.sites[ins[start].address].count;
            if (count < options.min_count) {
                continue;
            }
            std::size_t len = std::min(static_cast<std::size_t>(options.window), ins.size() - start);
            int cycles = 0;
            for (std::size_t i = start; i < start + len; i++) {
                int opcode = OpcodeFor(ins[i].mn, ins[i].mode);
                cycles += opcode < 0 ? 0 : OpcodeInfoFor(static_cast<std::uint8_t>(opcode)).cycles;
            }
            starts.push_back({count * static_cast<std::uint64_t>(cycles), r, start, len});
        }
    }
    std::stable_sort(starts.begin(), starts.end(),
                     [](const Candidate& a, const Candidate& b) { return a.potential > b.potential; });

    // every window length at each of the top starts
    std::vector<WindowFinding> found;
    for (std::size_t k = 0; k < starts.size() && k < static_cast<std::size_t>(options.top); k++) {
        const Candidate& c = starts[k];
        const RawRun& run = runs[c.run];
        for (std::size_t len = 2; len <= c.len; len++) {
            std::vector<RawIns> code(run.ins.begin() + static_cast<std::ptrdiff_t>(c.start),
                                     run.ins.begin() + static_cast<std::ptrdiff_t>(c.start + len));
            std::vector<RawIns> after =
                FollowingIns(compiled, run.span, run.items[c.start + len - 1]);
            Window w;
            std::string why;
            if (!MakeWindow(code, after, w, why)) {
                continue;
            }
            stats.windows++;
            Facts observed = ObservedFacts(w, code, p);
            std::string key = WindowKey(w) + " | " + FactsText(w, observed);
            auto it = cache.find(key);
            if (it == cache.end()) {
                it = cache.emplace(key, SearchWindow(w, observed, options, stats)).first;
            } else {
                stats.cached++;
            }
            const CachedSearch& result = it->second;
            std::uint64_t count = p.sites[code[0].address].count;
            if (result.strict.found) {
                found.push_back(MakeFinding(compiled, map, w, result.strict, "", count));
            }
            if (result.with_facts.found) {
                found.push_back(
                    MakeFinding(compiled, map, w, result.with_facts, result.facts, count));
            }
        }
    }

    // the best of overlapping windows, most cycles saved first; those that need facts are
    // chosen among themselves, so an exact result is never hidden behind one that needs facts
    std::stable_sort(found.begin(), found.end(), [](const WindowFinding& a, const WindowFinding& b) {
        if (a.saved != b.saved) {
            return a.saved > b.saved;
        }
        return a.bytes_before - a.bytes_after > b.bytes_before - b.bytes_after;
    });
    std::vector<WindowFinding> kept;
    for (int pass = 0; pass < 2; pass++) {
        std::vector<WindowFinding> chosen;
        for (const WindowFinding& f : found) {
            if (f.facts.empty() != (pass == 0)) {
                continue;
            }
            bool overlaps = false;
            for (const WindowFinding& k : chosen) {
                overlaps = overlaps || Overlaps(f, k);
            }
            if (!overlaps && (f.saved > 0 || f.bytes_after < f.bytes_before)) {
                chosen.push_back(f);
            }
        }
        kept.insert(kept.end(), chosen.begin(), chosen.end());
    }
    return kept;
}

} // namespace tisple
