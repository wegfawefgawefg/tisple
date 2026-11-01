#pragma once

// Which code takes turns in the overlay region (overlay_plan.cpp), worked out on the program as
// checked, before the mid-level passes copy functions into their callers: the groups' roots are
// then never copied (BuildOptions::no_inline), and the backend finds each group's code again in
// the generated code (overlay.hpp).
//
// - A group is a set of roots and every function that runs only through them: one the program
//   reaches from main, the interrupt handler or a function pointer only by way of a root.
// - A root runs only under some dispatch (overlay_guards.hpp: its facts), and it is not code
//   the backend copies into every caller anyway (more than kMaxInlineExprs expressions): a
//   small function under a dispatch hands its place to the functions it calls.
// - Never in a group: the interrupt handler and what it runs, main, a function whose address
//   is taken, and code that runs with an aux switch on or has a with_aux.
// - Two groups take turns in the one region only when every root of one and every root of the
//   other have facts that contradict: they never run in the same pass of a loop, and a load
//   happens only when the dispatch changes. Nor may one reach the other's roots, by its own
//   code or through resident code it calls (it would be overwritten under its own feet).
// - Groups whose code shares functions are one group: what they share then goes with them, and
//   the screens they are part of never load each other (the fight's phases).
// - Of the groups that may take turns, the plan takes the set that frees the most bytes (their
//   sum less the biggest, which the region holds), the biggest group's code (or its roots')
//   cut first when it is much bigger than the rest. Sizes here are estimates (expressions); the
//   backend measures the real ones.

#include "backend/overlay_guards.hpp"

#include <string>
#include <vector>

namespace tisple {

struct OverlayGroupPlan {
    std::vector<std::int32_t> roots;   // functions
    std::vector<std::int32_t> members; // functions, the roots included
    std::uint32_t weight = 0;          // expressions, an estimate of its bytes
};

struct OverlayPlan {
    std::vector<OverlayGroupPlan> groups; // that take turns in one region
    bool marked = false;                  // from roots the program marks :overlay
    // functions marked :overlay that stay resident: the interrupt handler runs them, main, a
    // function whose address is taken, code with an aux switch on (or not called at all)
    std::vector<std::int32_t> resident_marks;
};

// the groups for the program; tests (BuildOptions::overlay_test) pick any functions as roots,
// all that may be for 1, a seeded share otherwise
OverlayPlan PlanOverlays(const Program& program, std::uint32_t capacity);
OverlayPlan PlanOverlaysTest(const Program& program, int pick);

// TISPLE_OVERLAY_NOTES=1: each group's roots and functions, to stderr
void PrintOverlayPlan(const Program& program, const OverlayPlan& plan);

// the roots' names, for BuildOptions::no_inline
std::vector<std::string> OverlayRootNames(const Program& program, const OverlayPlan& plan);

} // namespace tisple
