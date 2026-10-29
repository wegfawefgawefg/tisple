#pragma once

// The mid-level optimiser (docs/midlevel-optimizer.md): tree-to-tree passes on the checked
// program, after expansion and before code generation, run to a fixed point. The backend
// runs them on its own copy of the program; the reference interpreter always runs the
// program as written, so it stays the oracle the differential tests and fuzzing check
// every pass against.
//
// Passes, in the order each round runs them:
// - dead: lets and stores nobody reads, (set x x), statements with no effect
// - loops: loops with no effect outside themselves that provably end
// Effect summaries (what each function may read and write) feed both.
//
// Nodes the passes do not know count as having every effect, so a new effect (an I/O-like
// primitive) is never deleted or moved until a pass is taught about it.

#include "check/program.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct OptOptions {
    int level = 1; // 0: no mid-level passes; 1: every pass
    // apply only the first N transformations (in a fixed order); -1: all of them
    std::int64_t bisect = -1;
    std::vector<std::string> disabled; // pass names (--no-pass)
};

struct OptReport {
    std::uint64_t applied = 0; // transformations made
    std::uint64_t total = 0;   // with --opt-bisect: how many an unlimited run makes
    std::string last;          // the last one made: what, which variable, where
};

// the pass names --no-pass takes
const std::vector<std::string>& OptPassNames();

void OptimizeProgram(Program& program, const OptOptions& options, OptReport& report);

// -O0, -O1, --opt-bisect N and --no-pass NAME at argv[i] (taking its value); false when
// argv[i] is none of them. A bad value sets error.
bool ParseOptFlag(int argc, char** argv, int& i, OptOptions& options, std::string& error);

// what --opt-bisect prints: "opt-bisect: 12 of 37 transformations; the last: ..."
std::string DescribeOptReport(const OptReport& report);

} // namespace tisple
