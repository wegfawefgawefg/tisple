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

namespace {

// the 16-bit constant o is: a number, or a symbol's address plus an offset
bool ConstWord(const Opnd& o, AsmValue& out) {
    if (o.size != 2 || o.in_a || o.b[0].kind != ByteKind::Imm || o.b[1].kind != ByteKind::Imm) {
        return false;
    }
    const AsmValue& lo = o.b[0].value;
    const AsmValue& hi = o.b[1].value;
    if (lo.sym < 0 && hi.sym < 0) {
        out = Num(static_cast<std::uint16_t>((lo.offset & 0xFF) | ((hi.offset & 0xFF) << 8)));
        return true;
    }
    if (lo.sym >= 0 && lo.sym == hi.sym && lo.offset == hi.offset && lo.part == Part::Low &&
        hi.part == Part::High) {
        out = lo;
        out.part = Part::Whole;
        return true;
    }
    return false;
}

Opnd WordOpnd(AsmValue v) {
    if (v.sym < 0) {
        return ImmOpnd(static_cast<std::uint16_t>(v.offset & 0xFFFF), 2);
    }
    return SymImmOpnd(v, 2);
}

// with a constant start and end, the let's value at the start is a constant too (an array's
// address plus the start): then the difference and the stop are constants, worked out here
bool ConstDiff(Gen& g, const ForParts& f, ExprId value, Opnd& diff, Opnd& stop) {
    AsmValue start;
    AsmValue end;
    if (!ConstWord(f.start, start) || start.sym >= 0 || !ConstWord(f.end, end) || end.sym >= 0) {
        return false;
    }
    g.bound.push_back({f.var, ExprOf(*g.program, f.id).a});
    Opnd first = TrySimple(g, value);
    g.bound.pop_back();
    AsmValue at;
    if (first.size != 2 || !ConstWord(first, at)) {
        return false;
    }
    at.offset = (at.offset - start.offset) & 0xFFFF;
    diff = WordOpnd(at);
    at.offset = (at.offset + end.offset) & 0xFFFF;
    stop = WordOpnd(at);
    return true;
}

} // namespace

bool ReplacedLoop(Gen& g, const ForParts& f) {
    if (f.size != 2 || f.down) {
        return false;
    }
    std::vector<ReducedLet> reduced = FindReducedLets(g, f.var, f.body);
    std::int32_t by_one = -1;
    ExprId value = -1;
    for (const ReducedLet& r : reduced) {
        if (r.step == 1 && by_one < 0) {
            by_one = r.var;
            value = r.value;
        }
    }
    if (by_one < 0) {
        return false;
    }
    Opnd counter = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(f.var)]), 2, 1);
    Opnd let = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(by_one)]), 2, 1);
    // the setup runs once each time the loop starts
    g.loop_depth--;
    Opnd diff;
    Opnd stop;
    if (ConstDiff(g, f, value, diff, stop)) {
        // the lets' first values with the counter at its start (its memory is never set)
        g.bound.push_back({f.var, ExprOf(*g.program, f.id).a});
        InitReducedLets(g, reduced);
        g.bound.pop_back();
    } else {
        CopyOpnd(g, f.start, counter);
        InitReducedLets(g, reduced);
        // the let minus the counter, and the let's value where the counter would end
        diff = NewTemp(g, 2);
        Combine16(g, Op::Sub, let, counter, diff);
        stop = NewTemp(g, 2);
        Combine16(g, Op::Add, diff, f.end, stop);
    }
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
