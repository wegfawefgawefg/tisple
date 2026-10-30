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
// - args: arguments for parameters their function never reads are not worked out
// - returns: return values no call uses are not worked out
// - collapse: loops whose passes all do the same, or whose last pass decides, run their body
//   once; accumulators (set x (+ x k)) add k times the passes
//   - scans: a while stepping its counter to a bound while nothing else it tests changes ends
//     at the bound
// - consts: constant propagation and folding across statements, branches on constants cut
// - ranges: compares and match clauses decided by the values their operands may hold, values
//   with one possible value made literals
//   - vars: what each variable may ever hold (a parameter every call passes the same, a step
//     of 65535 meaning -1), worked out at the start of each round for ranges to use
//   - elements: elements and fields no store reaches hold their initial values
//   - divisions: by 0 and 1, of a smaller dividend, (/ x (| x 1)) worked out without dividing
// - eval: calls of pure functions with constant arguments worked out at compile time, in the
//   interpreter (it runs before consts, which carries the values on)
// - licm: pure expressions a loop never changes the inputs of, worked out before it
// - cse: a value worked out again while the first working out holds, kept in a local
// - pointers: loads and stores through a pointer to a known element are its reads and sets
// Once a round changes nothing:
// - inline: calls the backend would inline become copies of the function's body
// - then (when nothing is left to inline) unroll and unswitch in collapse's walk
// Effect summaries (what each function may read and write) feed them all.
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
    // functions never inlined, by qualified name (BuildOptions::no_inline: the race's clock ends)
    std::vector<std::string> no_inline;
    // functions no code calls keep their bodies (BuildOptions::keep_all_functions: tests call
    // them by address); else inlining empties them
    bool keep_bodies = false;
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
