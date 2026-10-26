#pragma once

// The gap report: one program's findings and superoptimized windows ranked by cycles, and
// the same added up over many fuzz programs by pattern.

#include "gaps/findings.hpp"
#include "gaps/hot_windows.hpp"
#include "gaps/run.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tisple {

struct ProgramGaps {
    std::string name;
    std::string machine; // "the Apple IIe", "the bare simulator", ...
    ProfileRunResult run;
    std::uint64_t instructions = 0;
    std::uint64_t cycles = 0; // traced
    FindOptions find;
    std::vector<Finding> findings;
    bool superopt = false;
    std::vector<WindowFinding> windows;
    HotWindowStats window_stats;
};

std::string FormatProgramReport(const ProgramGaps& g, std::size_t top);

struct PatternTotal {
    std::uint64_t saved = 0;
    std::uint64_t sites = 0;
    std::uint64_t programs = 0;
    std::uint64_t first_seed = UINT64_MAX; // the smallest seed that shows it
    std::string example;                   // from that seed
};

struct BulkGaps {
    std::uint64_t programs = 0;
    std::uint64_t profiled = 0;
    std::uint64_t failed = 0; // did not check, compile or start
    std::uint64_t cycles = 0;
    std::uint64_t saved = 0;
    std::uint64_t window_saved = 0;
    std::map<std::string, PatternTotal> patterns; // by pattern
    std::map<std::string, PatternTotal> keys;     // by pattern and instruction shapes
    std::map<std::string, PatternTotal> windows;  // superoptimizer: before -> after shapes
    HotWindowStats window_stats;
};

void AddProgram(BulkGaps& bulk, const ProgramGaps& g, std::uint64_t seed);
void MergeBulk(BulkGaps& into, const BulkGaps& from);
std::string FormatBulkReport(const BulkGaps& bulk, std::uint64_t first_seed, std::size_t top);

// a window's code with its bytes named m0, m1, ... so the same shape matches anywhere
std::string ShapeCode(const std::vector<SoIns>& code, const std::vector<Cell>& cells);

} // namespace tisple
