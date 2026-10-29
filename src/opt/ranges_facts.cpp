// What a set gives the items after it, for the ranges pass (ranges.cpp): after (set x v) or
// (let x v) among a do's items, x holds v's range until something may write x. The facts sit
// in the walk's counters (fact set) and are undone in place (var -1) when a write may reach
// them: a set or let of x, a call whose summary writes it (or does I/O or jsr), io_write and
// jsr for globals, a loop from its start for what it writes anywhere in it.

#include "opt/ranges.hpp"

namespace tisple::opt {

namespace {

bool AnyFacts(const Ranger& g) {
    for (const Counter& c : g.counters) {
        if (c.fact && c.var >= 0) {
            return true;
        }
    }
    return false;
}

// what id may write: sets, lets and fors of variables, calls' summaries, io_write and jsr
// (globals)
void ForgetWritesIn(Ranger& g, ExprId id) {
    if (id < 0) {
        return;
    }
    const Pass& p = *g.p;
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Set:
        if (PlaceRoot(p, e.a) >= 0) {
            Forget(g, PlaceRoot(p, e.a));
        }
        break;
    case ExprKind::Let:
    case ExprKind::For:
        Forget(g, e.ref);
        break;
    case ExprKind::Call:
        ForgetCallWrites(g, e.ref);
        break;
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
        ForgetGlobals(g, false);
        break;
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    default:
        break;
    }
    EachChild(p, e, [&](ExprId child) { ForgetWritesIn(g, child); });
}

} // namespace

void Changed(Ranger& g) {
    g.p->memo.now++;
}

void Forget(Ranger& g, std::int32_t var) {
    Changed(g);
    for (Counter& c : g.counters) {
        if (c.fact && c.var == var) {
            c.var = -1;
        }
    }
}

void ForgetGlobals(Ranger& g, bool all) {
    Changed(g);
    for (Counter& c : g.counters) {
        if (c.fact && c.var >= 0 && (all || VarOf(*g.p, c.var).kind == VarKind::Global)) {
            c.var = -1;
        }
    }
}

void ForgetLoopWrites(Ranger& g, ExprId loop) {
    if (AnyFacts(g)) {
        ForgetWritesIn(g, loop);
    }
}

void ForgetCallWrites(Ranger& g, std::int32_t function) {
    const Summary& s = g.p->summaries[static_cast<std::size_t>(function)];
    if (s.io || s.jsr || s.unknown) {
        ForgetGlobals(g, false);
        return;
    }
    Changed(g);
    for (Counter& c : g.counters) {
        if (c.fact && c.var >= 0 && static_cast<std::size_t>(c.var) < s.writes.size() &&
            s.writes[static_cast<std::size_t>(c.var)]) {
            c.var = -1;
        }
    }
}

void Wrote(Ranger& g, ExprId id, std::int32_t var, ExprId value) {
    const Pass& p = *g.p;
    if (!RangedVar(p, var)) {
        Forget(g, var);
        return;
    }
    Range r = ValueRange(p, g.counters, value);
    Forget(g, var);
    g.last_write = id;
    g.written.var = var;
    g.written.range = r;
    g.written.fact = true;
}

} // namespace tisple::opt
