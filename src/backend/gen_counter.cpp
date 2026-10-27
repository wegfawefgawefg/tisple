#include "backend/gen.hpp"

// A u16 for counter in memory replaced by a let of its body that moves with it. search's
// (let p (ptr u8) (addr_of (at text i))) is a reduced let (gen_reduce.cpp) stepped by one each
// iteration, so p - i never changes: the loop keeps only p, stops when p reaches its value
// for the end (the difference plus the end, worked out once), and the body's reads of i work
// out p minus the difference. The counter's own INC and compare go. Whether that is cheaper
// depends on how often the body reads the counter; WordLoop (gen_loop.cpp) tries it as one
// more choice.

namespace tisple {

bool IsReplacedCounter(const Gen& g, std::int32_t var) {
    for (const auto& r : g.replaced) {
        if (r.first == var) {
            return true;
        }
    }
    return false;
}

bool ReadReplacedCounter(Gen& g, ExprId id, const Opnd* dst, Opnd& out) {
    const Expr& e = ExprOf(*g.program, id);
    if (e.kind != ExprKind::VarRef) {
        return false;
    }
    for (std::size_t i = 0; i < g.replaced.size(); i++) {
        if (g.replaced[i].first != e.ref) {
            continue;
        }
        std::size_t let = static_cast<std::size_t>(g.replaced[i].second);
        Opnd d = dst ? *dst : NewTemp(g, 2);
        Combine16(g, Op::Sub, MemOpnd(SymValue(g.be->var_sym[let]), 2, 1), g.replaced_diff[i], d);
        out = d;
        return true;
    }
    return false;
}

bool ReplacedLoop(Gen& g, const ForParts& f) {
    if (f.size != 2 || f.down) {
        return false;
    }
    std::vector<ReducedLet> reduced = FindReducedLets(g, f.var, f.body);
    std::int32_t by_one = -1;
    for (const ReducedLet& r : reduced) {
        if (r.step == 1 && by_one < 0) {
            by_one = r.var;
        }
    }
    if (by_one < 0) {
        return false;
    }
    Opnd counter = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(f.var)]), 2, 1);
    Opnd let = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(by_one)]), 2, 1);
    // the setup runs once each time the loop starts
    g.loop_depth--;
    CopyOpnd(g, f.start, counter);
    InitReducedLets(g, reduced);
    // the let minus the counter, and the let's value where the counter would end
    Opnd diff = NewTemp(g, 2);
    Combine16(g, Op::Sub, let, counter, diff);
    Opnd stop = NewTemp(g, 2);
    Combine16(g, Op::Add, diff, f.end, stop);
    g.loop_depth++;
    g.replaced.push_back({f.var, by_one});
    g.replaced_diff.push_back(diff);
    std::int32_t top = NewLabel(g);
    std::int32_t next = NewLabel(g);
    g.loops.back().continue_label = next;
    PlaceLabel(g, top);
    GenStmt(g, f.body);
    PlaceLabel(g, next);
    StepReducedLets(g, reduced);
    DropReducedLets(g, reduced);
    g.replaced.pop_back();
    g.replaced_diff.pop_back();
    // one step at a time, the let reaches stop exactly when the counter reaches the end
    LoadA(g, let, 0);
    OpA(g, Mn::CMP, stop, 0);
    Branch(g, Mn::BNE, top);
    LoadA(g, let, 1);
    OpA(g, Mn::CMP, stop, 1);
    Branch(g, Mn::BNE, top);
    return true;
}

} // namespace tisple
