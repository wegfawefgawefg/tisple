#pragma once

// The gap finder: where the compiler's output is worse than it could be, ranked by the
// cycles a run spends there. tisple gaps compiles, profiles (profile.hpp), optionally
// superoptimizes the hot windows (hot_windows.hpp) and reports (report.hpp); --fuzz does the
// same over the fuzz generator's programs and adds them up by pattern.

#include "gaps/report.hpp"

#include <cstdint>
#include <string>

namespace tisple {

struct GapsOptions {
    ProfileRunOptions run;
    FindOptions find;
    bool superopt = false;
    HotWindowOptions windows;
    std::string only; // report only functions whose name contains this
};

bool FindProgramGaps(const Program& program, const Compiled& compiled, const GapsOptions& options,
                     WindowCache& cache, ProgramGaps& out, std::string& error);

// the fuzz generator's program for a seed (it lives with the tests)
using ProgramTextFn = std::string (*)(std::uint64_t seed);

// seeds first..first+count-1, split over threads by seed; the totals do not depend on the
// number of threads
BulkGaps FuzzGaps(ProgramTextFn generate, std::uint64_t first, std::uint64_t count, int threads,
                  const GapsOptions& options);

// tisple gaps prog.tsp [--cycles N] [--superopt] [--window N] ...; tisple gaps --fuzz N ...
int RunGapsCommand(int argc, char** argv, ProgramTextFn generate);

} // namespace tisple
