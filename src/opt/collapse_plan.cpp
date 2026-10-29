// Which loop bodies do the same on every pass (collapse.cpp runs them once). A pass does what
// the one before did when nothing it reads was written by an earlier pass: a body is the same
// on every pass when no cell it writes (collapse_scan.cpp) overlaps one it reads, and no store
// through a pointer reaches what it reads or loads (collapse_memory.cpp).
//
// - Accumulators (collapse.hpp) are taken out first; their steps are read like the rest.
// - A variable a pass sets before anything in the pass reads it is the body's own too.
// - A body that reads its counter: the last pass decides when every write outside the body's
//   own variables lands in the same place on every pass, no store through a pointer or call
//   may land elsewhere, and the accumulators add what does not differ between passes.

#include "opt/collapse.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

bool Overlap(const Cell& x, const Cell& y) {
    return x.var == y.var && x.lo < y.hi && y.lo < x.hi;
}

// does a pass read a cell some pass writes (the body's own variables aside)
bool Conflict(const Pass& p, MemoryFacts& m, const Access& a) {
    if (a.all) {
        return true;
    }
    for (const Cell& w : a.writes) {
        for (const Cell& r : a.reads) {
            if (!Own(a, w.var) && Overlap(w, r)) {
                return true;
            }
        }
    }
    if (a.memory.load_ptrs.empty() && a.memory.store_ptrs.empty() && a.memory.calls.empty()) {
        return false;
    }
    MemoryUse use = a.memory;
    for (const Cell& w : a.writes) {
        if (!Own(a, w.var)) {
            use.written.push_back(w.var);
        }
    }
    for (const Cell& r : a.reads) {
        if (!Own(a, r.var)) {
            use.read.push_back(r.var);
        }
    }
    return MemoryClash(p, m, use);
}

bool Mentions(const std::vector<Cell>& cells, std::int32_t var) {
    for (const Cell& c : cells) {
        if (c.var == var) {
            return true;
        }
    }
    return false;
}

bool IsVar(const Pass& p, ExprId id, std::int32_t var) {
    const Expr& e = At(p, id);
    return e.kind == ExprKind::VarRef && e.ref == var;
}

// (set x (op x k)), or (op k x) for an op that commutes; false for anything else
bool AsAccumulator(const Pass& p, ExprId stmt, Accumulator& acc) {
    const Expr& set = At(p, stmt);
    if (set.kind != ExprKind::Set || At(p, set.a).kind != ExprKind::VarRef) {
        return false;
    }
    std::int32_t x = At(p, set.a).ref;
    const Var& v = VarOf(p, x);
    const Expr& value = At(p, set.b);
    if (!IsIntegerKind(KindOf(*p.program, v.type)) || Hardware(v) || v.address_taken ||
        value.kind != ExprKind::Binary) {
        return false;
    }
    bool commutes = value.op != Op::Sub;
    ExprId step = -1;
    if (IsVar(p, value.a, x)) {
        step = value.b;
    } else if (commutes && IsVar(p, value.b, x)) {
        step = value.a;
    }
    switch (value.op) {
    case Op::Add:
    case Op::Sub:
    case Op::Xor:
    case Op::Or:
    case Op::And:
        break;
    default:
        return false;
    }
    if (step < 0 || !IsPure(p, step)) {
        return false;
    }
    acc.stmt = stmt;
    acc.var = x;
    acc.op = value.op;
    acc.step = step;
    return true;
}

// every write outside the body's own variables lands in the same place on every pass, and
// each accumulator's step is the same
bool LastPassDecides(const Pass& p, const Access& a, const BodyPlan& plan) {
    if (a.unsteady) {
        return false;
    }
    for (const Cell& w : a.writes) {
        if (!w.steady && !Own(a, w.var)) {
            return false;
        }
    }
    for (const Accumulator& acc : plan.accumulators) {
        if (Varies(p, a, acc.step)) {
            return false;
        }
    }
    return true;
}

// (set x v) as a pass's statement, nothing before it in the pass (nor v) reading x: x holds
// what this pass gives it, like the body's own lets (a scalar no pointer reaches)
void MarkFirstWrite(const Pass& p, Access& a, ExprId item) {
    const Expr& set = At(p, item);
    if (set.kind != ExprKind::Set || At(p, set.a).kind != ExprKind::VarRef) {
        return;
    }
    std::int32_t x = At(p, set.a).ref;
    const Var& v = VarOf(p, x);
    if (x == a.counter || Own(a, x) || v.address_taken || Hardware(v) ||
        !IsScalarKind(KindOf(*p.program, v.type)) || Mentions(a.reads, x)) {
        return;
    }
    std::size_t at = static_cast<std::size_t>(x);
    a.own[at] = true;
    a.steady[at] = !a.varying && !Varies(p, a, set.b) && CountSets(p, a.body, x) == 1;
}

} // namespace

void PassStatements(const Pass& p, ExprId body, std::vector<ExprId>& out) {
    const Expr& e = At(p, body);
    if (e.kind != ExprKind::Do || e.type != kVoidType || Shared(p, body)) {
        out.push_back(body);
        return;
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        PassStatements(p, Item(p, e, i), out);
    }
}

BodyPlan PlanBody(const Pass& p, MemoryFacts& m, ExprId body, std::int32_t counter, ExprId skip) {
    BodyPlan plan;
    if (MayLeave(p, body, 0)) {
        return plan;
    }
    Access a;
    a.own.assign(p.program->vars.size(), false);
    a.steady.assign(p.program->vars.size(), false);
    a.body = body;
    a.counter = counter;
    std::vector<ExprId> items;
    PassStatements(p, body, items);
    for (ExprId item : items) {
        Accumulator acc;
        if (item == skip) {
            continue;
        }
        if (AsAccumulator(p, item, acc) && acc.var != counter && !Own(a, acc.var)) {
            plan.accumulators.push_back(acc);
            Scan(p, a, acc.step);
            continue;
        }
        Scan(p, a, item);
        MarkFirstWrite(p, a, item);
    }
    if (Conflict(p, m, a)) {
        return plan;
    }
    if (counter >= 0 &&
        (Mentions(a.writes, counter) || Own(a, counter) || VarOf(p, counter).address_taken)) {
        return plan;
    }
    // a body that reads the counter: the last pass decides, when every pass writes the same
    // places and the accumulators add the same each pass
    plan.last = counter >= 0 && Mentions(a.reads, counter);
    if (plan.last && !LastPassDecides(p, a, plan)) {
        return plan;
    }
    for (std::size_t i = 0; i < plan.accumulators.size(); i++) {
        std::int32_t x = plan.accumulators[i].var;
        if (Mentions(a.reads, x) || Mentions(a.writes, x) || Own(a, x)) {
            return plan;
        }
        for (std::size_t j = i + 1; j < plan.accumulators.size(); j++) {
            if (plan.accumulators[j].var == x) {
                return plan;
            }
        }
    }
    plan.same = true;
    return plan;
}

} // namespace tisple::opt
