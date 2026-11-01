#pragma once

// The program's call graph as the overlay plan sees it (overlay_plan.hpp): what may never be in
// a group, what runs without any call, each function's size, and the groups of sets of roots.

#include "backend/overlay_guards.hpp"

#include <vector>

namespace tisple {

struct OverlayGraph {
    const Program* program = nullptr;
    std::vector<bool> barred;          // by function: never in a group
    std::vector<bool> always;          // by function: reached without any call (main, ...)
    std::vector<std::uint32_t> weight; // by function: its body's expressions
    Guards guards;
};

OverlayGraph MakeOverlayGraph(const Program& program);

// every function reached from the always-reached ones without passing through blocked
std::vector<bool> ReachedWithout(const OverlayGraph& g, const std::vector<bool>& blocked);

// the functions reached only through roots, the roots included, but the barred ones
std::vector<bool> GroupOf(const OverlayGraph& g, const std::vector<std::int32_t>& roots);

// the expressions of the functions in group
std::uint32_t Weight(const OverlayGraph& g, const std::vector<bool>& group);

} // namespace tisple
