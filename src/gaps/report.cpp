#include "gaps/report.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace tisple {

namespace {

std::string Format(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string Format(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    return text;
}

unsigned long long U(std::uint64_t v) {
    return static_cast<unsigned long long>(v);
}

double Percent(std::uint64_t part, std::uint64_t whole) {
    return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
}

struct Row {
    std::string name;
    std::uint64_t saved = 0;
    std::uint64_t sites = 0;
};

std::vector<Row> Sorted(const std::map<std::string, Row>& rows) {
    std::vector<Row> out;
    for (const auto& entry : rows) {
        out.push_back(entry.second);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Row& a, const Row& b) { return a.saved > b.saved; });
    return out;
}

std::string RowTable(const char* title, const std::vector<Row>& rows, std::uint64_t cycles,
                     std::size_t top) {
    std::string text = Format("\n%s\n     cycles      %%  sites\n", title);
    for (std::size_t i = 0; i < rows.size() && i < top; i++) {
        const Row& r = rows[i];
        text += Format("%11llu %6.2f %6llu  %s\n", U(r.saved), Percent(r.saved, cycles), U(r.sites),
                       r.name.c_str());
    }
    return text;
}

std::string Hex4(std::uint16_t v) {
    return Format("$%04X", v);
}

std::string ProofText(const WindowFinding& f) {
    std::string proof = f.proof == Proof::kProved
                            ? Format("proved over 2^%.0f inputs", f.input_bits)
                            : Format("tested (2^%.0f inputs, too many to try)", f.input_bits);
    if (!f.facts.empty()) {
        proof += "; needs " + f.facts + " (observed on entry)";
    }
    if (f.window.assumes_no_alias) {
        proof += "; assumes indexed bytes do not alias";
    }
    return proof;
}

std::string WindowsSection(const ProgramGaps& g, std::size_t top) {
    const HotWindowStats& st = g.window_stats;
    std::string text = Format(
        "\nsuperoptimizer: %llu hot windows (%llu shapes from the cache), %llu searches, %llu "
        "finished inside the budget, longest finished %d instructions; %.1f s (slowest %.2f s), "
        "%llu nodes\n",
        U(st.windows), U(st.cached), U(st.searched), U(st.complete), st.longest_complete,
        st.seconds, st.slowest, U(st.nodes));
    std::uint64_t exact = 0;
    std::uint64_t with_facts = 0;
    for (const WindowFinding& f : g.windows) {
        (f.facts.empty() ? exact : with_facts) += f.saved;
    }
    text += Format("could save %llu cycles exactly (%.2f%%), %llu more given observed facts\n",
                   U(exact), Percent(exact, g.cycles), U(with_facts));
    std::size_t shown = 0;
    for (const WindowFinding& f : g.windows) {
        if (shown++ >= top) {
            break;
        }
        text += Format("%11llu %10llu  %s %s  %d -> %d cycles, %d -> %d bytes\n", U(f.saved),
                       U(f.count), f.function.c_str(), Hex4(f.window.address).c_str(),
                       f.cycles_before, f.cycles_after, f.bytes_before, f.bytes_after);
        text += "                        " + f.original + "\n";
        text += "                     => " + f.replacement + "\n";
        text += "                        " + ProofText(f) + "\n";
        if (!f.source.empty()) {
            text += "                        " + f.source + "\n";
        }
    }
    return text;
}

// a window the superoptimizer improved that covers the address: exact ones first
const WindowFinding* CoveringWindow(const std::vector<WindowFinding>& windows,
                                    std::uint16_t address) {
    const WindowFinding* best = nullptr;
    for (const WindowFinding& w : windows) {
        std::uint32_t end = w.window.address + static_cast<std::uint32_t>(w.bytes_before);
        if (address < w.window.address || address >= end || w.saved == 0) {
            continue;
        }
        if (best == nullptr || (best->facts.empty() == w.facts.empty() && w.saved > best->saved) ||
            (!best->facts.empty() && w.facts.empty())) {
            best = &w;
        }
    }
    return best;
}

std::string Suggestion(const Finding& f, const std::vector<WindowFinding>& windows) {
    const WindowFinding* w = CoveringWindow(windows, f.address);
    if (w != nullptr) {
        return w->original + "  =>  " + w->replacement +
               (w->facts.empty() ? "" : "  (needs " + w->facts + ")");
    }
    switch (f.pattern) {
    case Pattern::kLoadAlready:
        return f.reason.find("N/Z") != std::string::npos
                   ? "drop it where the flags already follow A, else a cheaper flag test"
                   : "drop it";
    case Pattern::kCompareOneWay:
        return f.reason.find("never") != std::string::npos ? "drop the compare and the branch"
                                                           : "drop the compare; jump instead";
    case Pattern::kBranchOneWay:
        return f.reason.find("never") != std::string::npos ? "drop the branch"
                                                           : "lay the code out to fall through";
    case Pattern::kStrided:
        return "keep the value from one time to the next and add the step";
    default:
        return "drop it";
    }
}

std::string CellName(const std::vector<Cell>& cells, std::int8_t cell) {
    std::string name = "m" + std::to_string(cell);
    switch (cells[static_cast<std::size_t>(cell)].mode) {
    case Mode::ZpX:
    case Mode::AbsX:
        return name + ",X";
    case Mode::ZpY:
    case Mode::AbsY:
        return name + ",Y";
    case Mode::IndX:
        return "(" + name + ",X)";
    case Mode::IndY:
        return "(" + name + "),Y";
    default:
        return name;
    }
}

void Add(std::map<std::string, PatternTotal>& map, const std::string& key, std::uint64_t saved,
         std::uint64_t seed, const std::string& example, bool new_program) {
    PatternTotal& t = map[key];
    t.saved += saved;
    t.sites++;
    if (new_program) {
        t.programs++;
    }
    if (seed < t.first_seed) {
        t.first_seed = seed;
        t.example = example;
    }
}

void Merge(std::map<std::string, PatternTotal>& into,
           const std::map<std::string, PatternTotal>& from) {
    for (const auto& entry : from) {
        PatternTotal& t = into[entry.first];
        t.saved += entry.second.saved;
        t.sites += entry.second.sites;
        t.programs += entry.second.programs;
        if (entry.second.first_seed < t.first_seed) {
            t.first_seed = entry.second.first_seed;
            t.example = entry.second.example;
        }
    }
}

std::string TotalsTable(const char* title, const std::map<std::string, PatternTotal>& map,
                        std::uint64_t cycles, std::size_t top, bool examples) {
    std::vector<std::pair<std::string, PatternTotal>> rows(map.begin(), map.end());
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.second.saved > b.second.saved; });
    std::string text = Format("\n%s (%zu kinds)\n     cycles      %%  sites progs  first seed\n",
                              title, rows.size());
    for (std::size_t i = 0; i < rows.size() && i < top; i++) {
        const PatternTotal& t = rows[i].second;
        text += Format("%11llu %6.2f %6llu %5llu  %llu  %s\n", U(t.saved), Percent(t.saved, cycles),
                       U(t.sites), U(t.programs), U(t.first_seed), rows[i].first.c_str());
        if (examples && !t.example.empty()) {
            text += "                                       e.g. " + t.example + "\n";
        }
    }
    return text;
}

void AddStats(HotWindowStats& t, const HotWindowStats& s) {
    t.windows += s.windows;
    t.searched += s.searched;
    t.cached += s.cached;
    t.complete += s.complete;
    t.nodes += s.nodes;
    t.seconds += s.seconds;
    t.slowest = s.slowest > t.slowest ? s.slowest : t.slowest;
    t.longest_complete = std::max(t.longest_complete, s.longest_complete);
}

} // namespace

std::string ShapeCode(const std::vector<SoIns>& code, const std::vector<Cell>& cells) {
    std::string text;
    for (std::size_t i = 0; i < code.size(); i++) {
        const SoIns& ins = code[i];
        text += i == 0 ? "" : " / ";
        text += MnName(ins.mn);
        if (ins.mode == Mode::Acc) {
            text += " A";
        } else if (ins.mode == Mode::Imm) {
            text += Format(" #$%02X", ins.imm);
        } else if (ins.cell >= 0) {
            text += " " + CellName(cells, ins.cell);
        }
    }
    return code.empty() ? "(nothing)" : text;
}

std::string FormatProgramReport(const ProgramGaps& g, std::size_t top) {
    std::string text =
        Format("gaps: %s on %s, %llu cycles (%s), %llu instructions", g.name.c_str(),
               g.machine.c_str(), U(g.cycles), g.run.stop.c_str(), U(g.instructions));
    if (g.run.calls > 0) {
        text += Format(", %llu calls", U(g.run.calls));
    }
    text += "\n";
    if (!g.only.empty()) {
        text += "only functions named *" + g.only + "*; percentages are of the whole run\n";
    }
    text += Format("Hints, not proofs: each condition held on %s of at least %llu executions in\n"
                   "this run, so some fact the compiler could know would make the instruction\n"
                   "redundant. Cycles saved are what this run would have saved.\n",
                   g.find.min_fraction >= 1.0 ? "every one"
                                              : Format("%.0f%%", 100 * g.find.min_fraction).c_str(),
                   U(g.find.min_count));
    std::uint64_t total = TotalSaved(g.findings);
    text += Format("\ncould save %llu cycles of %llu (%.2f%%) at %zu findings\n", U(total),
                   U(g.cycles), Percent(total, g.cycles), g.findings.size());

    std::map<std::string, Row> by_pattern;
    std::map<std::string, Row> by_function;
    for (const Finding& f : g.findings) {
        Row& p = by_pattern[PatternName(f.pattern)];
        p.name = PatternName(f.pattern);
        p.saved += f.saved;
        p.sites++;
        Row& fn = by_function[f.function];
        fn.name = f.function;
        fn.saved += f.saved;
        fn.sites++;
    }
    text += RowTable("by pattern", Sorted(by_pattern), g.cycles, top);
    text += RowTable("by function", Sorted(by_function), g.cycles, top);

    text += Format("\ntop sites\n     cycles      count  where\n");
    for (std::size_t i = 0; i < g.findings.size() && i < top; i++) {
        const Finding& f = g.findings[i];
        text += Format("%11llu %10llu  %s %s  %s: %s%s\n", U(f.saved), U(f.count),
                       f.function.c_str(), Hex4(f.address).c_str(), f.text.c_str(),
                       f.reason.c_str(), f.estimate ? " (estimate)" : "");
        if (f.out_of != f.count) {
            text +=
                Format("                        held on %llu of %llu\n", U(f.held), U(f.out_of));
        }
        if (!f.source.empty()) {
            text += "                        " + f.source + "\n";
        }
        text += "                        suggest: " + Suggestion(f, g.windows) + "\n";
    }
    if (g.superopt) {
        text += WindowsSection(g, top);
    }
    return text;
}

void AddProgram(BulkGaps& bulk, const ProgramGaps& g, std::uint64_t seed) {
    bulk.profiled++;
    bulk.cycles += g.cycles;
    std::vector<Finding> code;
    std::vector<Finding> runtime;
    for (const Finding& f : g.findings) {
        bool fixed = f.function.rfind("runtime", 0) == 0 || f.function.rfind("startup", 0) == 0;
        (fixed ? runtime : code).push_back(f);
    }
    bulk.saved += TotalSaved(code);
    bulk.runtime_saved += TotalSaved(runtime);
    std::map<std::string, bool> seen_runtime;
    for (const Finding& f : runtime) {
        std::string key = f.function + ": " + f.key;
        Add(bulk.runtime, key, f.saved, seed, f.text, !seen_runtime[key]);
        seen_runtime[key] = true;
    }
    std::map<std::string, bool> seen_pattern;
    std::map<std::string, bool> seen_key;
    for (const Finding& f : code) {
        std::string pattern = PatternName(f.pattern);
        std::string example = f.function + " " + f.text + " at " + f.source;
        Add(bulk.patterns, pattern, f.saved, seed, example, !seen_pattern[pattern]);
        seen_pattern[pattern] = true;
        Add(bulk.keys, f.key, f.saved, seed, example, !seen_key[f.key]);
        seen_key[f.key] = true;
    }
    std::map<std::string, bool> seen_window;
    for (const WindowFinding& f : g.windows) {
        if (f.function.rfind("runtime", 0) == 0 || f.function.rfind("startup", 0) == 0) {
            continue;
        }
        std::string key = ShapeCode(f.window.code, f.window.cells) + "  =>  " +
                          ShapeCode(f.replacement_code, f.window.cells);
        bool facts = !f.facts.empty();
        (facts ? bulk.window_facts_saved : bulk.window_saved) += f.saved;
        std::map<std::string, PatternTotal>& table = facts ? bulk.windows_facts : bulk.windows;
        Add(table, key, f.saved, seed, f.function + " at " + f.source, !seen_window[key]);
        seen_window[key] = true;
    }
    AddStats(bulk.window_stats, g.window_stats);
}

void MergeBulk(BulkGaps& into, const BulkGaps& from) {
    into.programs += from.programs;
    into.profiled += from.profiled;
    into.failed += from.failed;
    into.cycles += from.cycles;
    into.saved += from.saved;
    into.runtime_saved += from.runtime_saved;
    into.window_saved += from.window_saved;
    into.window_facts_saved += from.window_facts_saved;
    Merge(into.patterns, from.patterns);
    Merge(into.keys, from.keys);
    Merge(into.runtime, from.runtime);
    Merge(into.windows, from.windows);
    Merge(into.windows_facts, from.windows_facts);
    AddStats(into.window_stats, from.window_stats);
}

std::string FormatBulkReport(const BulkGaps& bulk, std::uint64_t first_seed, std::size_t top) {
    std::string text =
        Format("gaps over fuzz programs, seeds %llu..%llu: %llu profiled, %llu failed to build or "
               "start, %llu cycles traced\n",
               U(first_seed), U(first_seed + bulk.programs - 1), U(bulk.profiled), U(bulk.failed),
               U(bulk.cycles));
    text += "Hints, not proofs (see tisple gaps on one program for the rules).\n";
    text += Format("could save %llu cycles (%.2f%%) in compiled code, %llu (%.2f%%) more in the "
                   "startup and runtime routines\n",
                   U(bulk.saved), Percent(bulk.saved, bulk.cycles), U(bulk.runtime_saved),
                   Percent(bulk.runtime_saved, bulk.cycles));
    text += TotalsTable("compiled code by pattern", bulk.patterns, bulk.cycles, top, false);
    text += TotalsTable("compiled code by pattern and shape", bulk.keys, bulk.cycles, top, true);
    text += TotalsTable("runtime routines by pattern and shape", bulk.runtime, bulk.cycles, top / 2,
                        false);
    if (bulk.window_stats.windows > 0) {
        const HotWindowStats& st = bulk.window_stats;
        text += Format("\nsuperoptimizer: %llu hot windows, %llu searches (%llu from the cache), "
                       "%llu finished in budget, longest finished %d; %.1f s, %llu nodes\n"
                       "compiled code could save %llu cycles (%.2f%%) exactly, %llu (%.2f%%) "
                       "given observed facts\n",
                       U(st.windows), U(st.searched), U(st.cached), U(st.complete),
                       st.longest_complete, st.seconds, U(st.nodes), U(bulk.window_saved),
                       Percent(bulk.window_saved, bulk.cycles), U(bulk.window_facts_saved),
                       Percent(bulk.window_facts_saved, bulk.cycles));
        text += TotalsTable("exact windows by shape", bulk.windows, bulk.cycles, top, true);
        text += TotalsTable("windows that need observed facts, by shape", bulk.windows_facts,
                            bulk.cycles, top / 2, true);
    }
    return text;
}

} // namespace tisple
