#pragma once

// What the program's dispatch says about when each function runs (overlay_guards.cpp), for
// the overlay groups (overlay.hpp): a function called only in the arm of a `match` on a global,
// or past `(if (!= g k) ... (return))`, runs only while that global holds that value. Two
// functions whose facts contradict (g = 1 against g = 2, or g = 1 against g /= 1) never run in
// the same pass of a loop unless the code between changes g: a screen change.
//
// The facts are on globals compared with constants (a `match` on one, `=` and `!=` with a
// constant, a bool global as a condition, `not`, `and`, `or`), each the values the global may
// hold or the ones it does not (an arm of `match` for two values, the wildcard arm), in the
// code's order: a set of the
// global, or a call that may set it (directly or not), forgets them; a loop forgets what its
// body may set before the body runs; an `if` whose one branch never goes on (a return, a break)
// leaves the other branch's facts for the code after it. A function's facts are the ones that
// hold at every call of it (its caller's at the call); main's, the handler's and a function
// whose address is taken have none.

#include "check/program.hpp"

#include <vector>

namespace tisple {

// var holds one of values (in), or none of them (not in); values sorted, no repeats
struct GuardFact {
    std::int32_t var = -1;
    std::vector<std::uint16_t> values;
    bool in = true;
};

// a function's facts, one a var at most, sorted by var
using GuardFacts = std::vector<GuardFact>;

struct Guards {
    std::vector<GuardFacts> functions; // by function
    // by function: only the facts a loop's own dispatch makes (a match or if inside a while or
    // for, in the function that has the loop): the program's screens, which change seldom.
    // The others (a state machine's phases a pass steps through) may change every few frames
    std::vector<GuardFacts> screens;
};

Guards FindGuards(const Program& program);

// some fact of a and some of b cannot both hold
bool Contradict(const GuardFacts& a, const GuardFacts& b);

} // namespace tisple
