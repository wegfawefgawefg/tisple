// The call graph as the overlay plan sees it: overlay_graph.hpp.

#include "backend/overlay_graph.hpp"

#include "backend/analysis.hpp"
#include "backend/irq.hpp"
#include "check/aux_mode.hpp"

namespace tisple {

OverlayGraph MakeOverlayGraph(const Program& program) {
    std::size_t count = program.functions.size();
    OverlayGraph g;
    g.program = &program;
    g.barred.assign(count, false);
    g.always.assign(count, false);
    g.weight.assign(count, 0);
    std::vector<bool> handler = HandlerFunctions(program);
    AuxModes modes = FindAuxModes(program);
    std::vector<bool> called(count, false);
    for (const Function& fn : program.functions) {
        for (std::int32_t callee : fn.callees) {
            called[static_cast<std::size_t>(callee)] = true;
        }
    }
    for (std::size_t f = 0; f < count; f++) {
        const Function& fn = program.functions[f];
        std::int32_t id = static_cast<std::int32_t>(f);
        bool aux = modes.any && (AuxFunction(modes, id) || AuxMirrored(modes, id));
        g.always[f] = id == program.main || id == program.handler || fn.address_taken ||
                      (program.main < 0 && !called[f]);
        g.barred[f] = g.always[f] || handler[f] || aux || fn.dispatcher || fn.io_patch >= 0;
        g.weight[f] = CountExprs(program, fn.body);
    }
    g.guards = FindGuards(program);
    return g;
}

// every function reached from the always-reached ones without passing through blocked
std::vector<bool> ReachedWithout(const OverlayGraph& g, const std::vector<bool>& blocked) {
    const Program& program = *g.program;
    std::vector<bool> seen(program.functions.size(), false);
    std::vector<std::int32_t> work;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (g.always[f] && !blocked[f]) {
            seen[f] = true;
            work.push_back(static_cast<std::int32_t>(f));
        }
    }
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(f)].callees) {
            std::size_t c = static_cast<std::size_t>(callee);
            if (!seen[c] && !blocked[c]) {
                seen[c] = true;
                work.push_back(callee);
            }
        }
    }
    return seen;
}

// the functions reached only through roots, the roots included, but the barred ones
std::vector<bool> GroupOf(const OverlayGraph& g, const std::vector<std::int32_t>& roots) {
    const Program& program = *g.program;
    std::vector<bool> blocked(program.functions.size(), false);
    for (std::int32_t r : roots) {
        blocked[static_cast<std::size_t>(r)] = true;
    }
    std::vector<bool> outside = ReachedWithout(g, blocked);
    std::vector<bool> group(program.functions.size(), false);
    std::vector<std::int32_t> work = roots;
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        std::size_t i = static_cast<std::size_t>(f);
        if (group[i] || outside[i]) {
            continue;
        }
        group[i] = true;
        for (std::int32_t callee : program.functions[i].callees) {
            work.push_back(callee);
        }
    }
    for (std::size_t f = 0; f < group.size(); f++) {
        group[f] = group[f] && !g.barred[f];
    }
    return group;
}

std::uint32_t Weight(const OverlayGraph& g, const std::vector<bool>& group) {
    std::uint32_t total = 0;
    for (std::size_t f = 0; f < group.size(); f++) {
        total += group[f] ? g.weight[f] : 0;
    }
    return total;
}

} // namespace tisple
