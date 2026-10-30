#pragma once

// The code size inlined calls and unrolled loops add, in bytes, and a budget for it.
//
// - Code generation notes each copy it makes: every inlined copy of a small function
//   (InlineKind::Always) and every unrolled for loop's copies, with their bytes and cycles
//   (Gen::cost, plus the runtime routines they call) and the code's place (gen_expr.cpp,
//   gen_unroll.cpp). An unrollable loop is made both ways, as copies and as a loop, so both
//   are measured; copies no faster and no smaller than the loop are dropped there.
// - A copy's growth is its bytes over the code it replaces: a JSR and the stores of the
//   arguments the copy reads in place, or the loop. A function copied at every call also
//   drops its own body. What it saves: against the callee's own cycles, a JSR, an RTS and
//   the stores, or the loop's cycles; times the runs of the function it is in (run_counts).
// - An item is a function's calls or one loop, all its copies together. Over budget, the
//   items that save the fewest cycles for their bytes go: that function's calls stay JSRs,
//   that loop stays a loop. Then the program is generated and measured again
//   (CompileProgram); an item inside or around one that went waits for that measure.
// - The budgets: kMaxItemGrowth bytes for one item and kMaxFunctionGrowth for one
//   function's code, except for items that save kHotCycles cycles or more a byte; and
//   kMaxProgramGrowth plus an eighth of the code for the whole program, hot items last.
// - The mid-level passes copy small functions too (opt/inline.cpp), and their copies are
//   plain code here. So the program as the mid-level leaves it before it inlines is
//   measured first, and a function whose copies at its calls would go over the item budget
//   is copied by neither.

#include "check/program.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace tisple {

struct Backend;

constexpr std::int64_t kMaxItemGrowth = 128;
constexpr std::int64_t kMaxFunctionGrowth = 512;
constexpr std::int64_t kMaxProgramGrowth = 1024;

// (true, a for expression) for a loop, (false, a function) for its calls
using SizeItem = std::pair<bool, std::int32_t>;

// one inlined copy of a call, or one unrolled loop's copies together
struct SizeNote {
    SizeItem item;
    ExprId site = -1;           // the call or the loop
    std::int32_t function = -1; // whose code holds it
    std::int64_t bytes = 0;
    std::int64_t instead = 0; // the bytes of the code it replaces
    // a loop's: the cycles its copies save over the loop, per run of the function, each loop
    // level weighed (Gen::cost; both were made)
    std::int64_t saved = 0;
    // a call's copy: its cycles as Gen::cost weighs them, and the bytes of the arguments it
    // reads in place
    std::uint64_t cost = 0;
    std::uint32_t bound_bytes = 0;
    // the code's weight where it is: as Gen::cost weighs (LoopWeight), and with the loops'
    // whole trip counts (RunWeight)
    std::uint64_t weight = 1;
    std::uint64_t runs = 1;
    std::vector<SizeItem> around; // the copies it is inside, in that function's code
};

// what code generation must not copy: by function, its calls stay JSRs; by for expression,
// it stays a loop
struct SizeChoices {
    std::vector<bool> keep_calls;
    std::vector<bool> keep_loops;
};

// by function, the functions whose code is in the binary (KeptFunctions); true when the
// notes of the generation in be go over a budget, with more kept in choices: the items in gone
bool TrimGrowth(const Backend& be, const std::vector<bool>& kept, SizeChoices& choices,
                std::vector<SizeItem>& gone);

// by function: one copy at each of its calls, as the mid-level makes them, would go over the
// item budget (be: a generation with no choices made)
std::vector<bool> CopiesOverBudget(const Backend& be, const std::vector<bool>& kept);

// TISPLE_SIZE_NOTES=1: the items and their growth on stderr, and what the budget turns down
void PrintSizeNotes(const Backend& be, const std::vector<bool>& kept);

} // namespace tisple
