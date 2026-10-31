// Loop collapse: a loop whose passes all do the same (collapse_plan.cpp) runs its body once.
// Inner loops go first, so a nest whose inner loop collapses may collapse in turn.
//
// - A for whose body does not read its variable:
//     (for i T a b body)  ->  (if (< a b) body)
//   with no test when the shapes of a and b say the body runs (-4 below (& x 7)), and nothing
//   when they say it never does. a and b are still worked out once, in order.
// - A counting while, (let c v) just before (lets of other variables may stand between) and
//   (while (< c k) ... (set c (+ c s)) ...),
//   whose other statements do not read c: the body once and c set to where it ends.
// - Accumulators, (set x (+ x k)) with x read nowhere else, add k once per pass:
//   (set x (+ x (* k n))) for n passes, (^ x k) when n is odd, (| x k) and (& x k) once.
//   Only when n is known, but for | and &; or, for + and - in a for, from b - a worked out
//   before it (collapse_count.cpp).
// - A body that reads its counter and whose last pass decides: a for with a literal range
//   runs that pass with the counter's last value put in, a counting while sets the counter
//   to where its last pass starts and runs that pass.
// A loop that may end its pass early (break, continue, return) is left alone, but for
// (if x (continue)) items, which first become elses of the rest (collapse_continue.cpp).

#include "opt/collapse.hpp"

#include "check/arith.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

struct Collapser {
    Pass* p = nullptr;
    std::int32_t function = -1; // the one being walked
    // the do and index of the branch whose arm the walk is in, when nothing but lets stand
    // between it and the arm's items: a counting while there may have its let before the
    // branch (unswitching puts it there); -1 for none
    ExprId outer_list = -1;
    std::uint32_t outer_index = 0;
    MemoryFacts memory;
    std::uint64_t changes = 0;
};

ExprId NewLiteral(Pass& p, TypeId type, std::uint32_t bits, const Loc& loc) {
    Expr lit;
    lit.kind = ExprKind::Literal;
    lit.type = type;
    lit.value = MaskToKind(KindOf(*p.program, type), bits);
    lit.loc = loc;
    return NewExpr(p, lit);
}

ExprId NewRead(Pass& p, std::int32_t var, const Loc& loc) {
    Expr read;
    read.kind = ExprKind::VarRef;
    read.type = VarOf(p, var).type;
    read.ref = var;
    read.loc = loc;
    return NewExpr(p, read);
}

ExprId NewBinary(Pass& p, Op op, TypeId type, ExprId a, ExprId b, const Loc& loc) {
    Expr e;
    e.kind = ExprKind::Binary;
    e.op = op;
    e.type = type;
    e.a = a;
    e.b = b;
    e.loc = loc;
    return NewExpr(p, e);
}

ExprId NewSet(Pass& p, std::int32_t var, ExprId value, const Loc& loc) {
    Expr set;
    set.kind = ExprKind::Set;
    set.type = kVoidType;
    set.a = NewRead(p, var, loc);
    set.b = value;
    set.loc = loc;
    return NewExpr(p, set);
}

// what an accumulator does over n passes (n of at least one; -1 when not known, which only
// | and & take, and + and - when count, a u16 local, holds it): its new statement, or -1 when
// it ends where it started
ExprId Accumulate(Pass& p, const Accumulator& acc, std::int64_t n, std::int32_t count_var) {
    Loc loc = At(p, acc.stmt).loc;
    TypeId type = VarOf(p, acc.var).type;
    switch (acc.op) {
    case Op::Or:
    case Op::And:
        return acc.stmt;
    case Op::Xor:
        return n % 2 == 1 ? acc.stmt : -1;
    default: {
        ExprId count = NewLiteral(p, type, static_cast<std::uint32_t>(n), loc);
        if (count_var >= 0) {
            Expr convert;
            convert.kind = ExprKind::Convert;
            convert.type = type;
            convert.a = NewRead(p, count_var, loc);
            convert.loc = loc;
            count = type == kU16Type ? convert.a : NewExpr(p, convert);
        }
        ExprId times = NewBinary(p, Op::Mul, type, acc.step, count, loc);
        return NewSet(p, acc.var, NewBinary(p, acc.op, type, NewRead(p, acc.var, loc), times, loc),
                      loc);
    }
    }
}

// the statements of one pass of body (a do's items, or body itself), each accumulator's
// total where it stood (its step may read the body's own variables, set afresh on every
// pass), dos among them rebuilt the same way; skip is the while counter's step
std::vector<ExprId> OnePass(Pass& p, ExprId body, const BodyPlan& plan, std::int64_t n, ExprId skip,
                            std::int32_t count_var = -1) {
    std::vector<ExprId> out;
    Expr e = At(p, body);
    if (e.kind != ExprKind::Do || e.type != kVoidType || Shared(p, body)) {
        ExprId stmt = body == skip ? -1 : body;
        for (const Accumulator& acc : plan.accumulators) {
            if (acc.stmt == body) {
                stmt = Accumulate(p, acc, n, count_var);
            }
        }
        if (stmt >= 0) {
            out.push_back(stmt);
        }
        return out;
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        ExprId item = Item(p, e, i);
        std::vector<ExprId> stmts = OnePass(p, item, plan, n, skip, count_var);
        Expr inner = At(p, item);
        if (inner.kind != ExprKind::Do || (stmts.size() == 1 && stmts[0] == item)) {
            out.insert(out.end(), stmts.begin(), stmts.end());
            continue;
        }
        bool same = stmts.size() == inner.count;
        for (std::uint32_t k = 0; same && k < inner.count; k++) {
            same = stmts[k] == Item(p, inner, k);
        }
        if (same) {
            out.push_back(item);
            continue;
        }
        // a do of its own: its lets stay in its scope
        out.push_back(NewList(p, ExprKind::Do, kVoidType, stmts, inner.loc));
    }
    return out;
}

// (set c (+ c s)) among the while body's items, for counter c
ExprId CounterStep(const Pass& p, ExprId body, std::int32_t c) {
    const Expr& e = At(p, body);
    for (std::uint32_t i = 0; e.kind == ExprKind::Do && i < e.count; i++) {
        const Expr& set = At(p, Item(p, e, i));
        if (set.kind == ExprKind::Set && At(p, set.a).kind == ExprKind::VarRef &&
            At(p, set.a).ref == c) {
            return Item(p, e, i);
        }
    }
    return -1;
}

// every read of var under id becomes the literal bits (a for's variable, for the last pass)
void PutValue(Pass& p, ExprId id, std::int32_t var, std::uint32_t bits) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && e.ref == var) {
        ReplaceWithLiteral(
            p, id, static_cast<std::uint16_t>(MaskToKind(KindOf(*p.program, e.type), bits)));
        return;
    }
    Expr copy = e;
    EachChild(p, copy, [&](ExprId child) { PutValue(p, child, var, bits); });
}

ExprId Guard(Pass& p, ExprId test, const std::vector<ExprId>& stmts, const Loc& loc) {
    Expr branch;
    branch.kind = ExprKind::If;
    branch.type = kVoidType;
    branch.loc = loc;
    branch.a = test;
    branch.b = NewList(p, ExprKind::Do, kVoidType, stmts, loc);
    return NewExpr(p, branch);
}

// what stands for a for whose passes collapse (named by nobody yet); false when it stays
bool CollapseFor(Collapser& c, ExprId loop, std::vector<ExprId>& out) {
    Pass& p = *c.p;
    Expr e = At(p, loop);
    std::int64_t trips = KnownTrips(p, loop, -1);
    std::int32_t a_lo = 0;
    std::int32_t a_hi = 0;
    std::int32_t b_lo = 0;
    std::int32_t b_hi = 0;
    ShapeRange(p, e.a, a_lo, a_hi);
    if (e.b >= 0) {
        ShapeRange(p, e.b, b_lo, b_hi);
    }
    bool runs = trips > 0 || e.b < 0 || b_lo > a_hi;
    bool never = trips == 0 || (e.b >= 0 && b_hi <= a_lo);
    // no pass: whatever the body would do
    if (never) {
        return IsPure(p, e.a) && IsPure(p, e.b) && Allow(p, "empty for", loop, e.ref);
    }
    BodyPlan plan = PlanBody(p, c.memory, e.c, e.ref, -1);
    if (!plan.same || (plan.last && trips < 1)) {
        return false;
    }
    // passes not known: b - a of them, worked out before the loop (collapse_count.cpp)
    bool counted = NeedsCount(plan) && trips < 0;
    if (counted && (!CountWillDo(plan) || !CountableBounds(p, loop))) {
        return false;
    }
    if (!Allow(p, plan.last ? "for collapsed to its last pass" : "for collapsed", loop, e.ref)) {
        return false;
    }
    // the count goes first in what runs in place of the passes (behind the guard: nothing
    // to work out when there are none)
    std::int32_t count_var = -1;
    ExprId count_let = counted ? CountLet(p, c.function, loop, count_var) : -1;
    if (plan.last) {
        std::uint16_t start = 0;
        ConstValue(*p.program, e.a, start);
        PutValue(p, e.c, e.ref, static_cast<std::uint32_t>(start + trips - 1));
    }
    std::vector<ExprId> stmts = OnePass(p, e.c, plan, trips, -1, count_var);
    if (count_let >= 0) {
        stmts.insert(stmts.begin(), count_let);
    }
    if (runs) {
        // the range is still worked out, for what it does
        for (ExprId bound : {e.a, e.b}) {
            if (bound >= 0 && !IsPure(p, bound)) {
                out.push_back(bound);
            }
        }
        out.insert(out.end(), stmts.begin(), stmts.end());
        return true;
    }
    ExprId test = NewBinary(p, Op::Lt, kBoolType, e.a, e.b, e.loc);
    out.push_back(Guard(p, test, stmts, e.loc));
    return true;
}

// a counting while just after its counter's let: the body once, the counter where it ends
bool CollapseWhile(Collapser& c, ExprId loop, ExprId before, std::vector<ExprId>& out) {
    Pass& p = *c.p;
    if (before < 0) {
        return false;
    }
    std::int64_t trips = KnownTrips(p, loop, before);
    if (trips < 0) {
        return false;
    }
    Expr e = At(p, loop);
    std::int32_t counter = At(p, At(p, e.a).a).ref;
    if (trips == 0) {
        return Allow(p, "empty while", loop, counter);
    }
    ExprId step = CounterStep(p, e.b, counter);
    if (step < 0) {
        return false;
    }
    BodyPlan plan = PlanBody(p, c.memory, e.b, counter, step);
    const char* what = plan.last ? "while collapsed to its last pass" : "while collapsed";
    if (!plan.same || !Allow(p, what, loop, counter)) {
        return false;
    }
    // (KnownTrips took the step as (+ c s), s a literal, and the let's value as a literal)
    ExprId s = At(p, At(p, step).b).b;
    if (plan.last) {
        // the counter as the last pass starts, then that pass with its step
        std::uint16_t start = 0;
        ConstValue(*p.program, At(p, before).a, start);
        std::uint32_t last = start + static_cast<std::uint32_t>((trips - 1) * At(p, s).value);
        TypeId type = VarOf(p, counter).type;
        out.push_back(NewSet(p, counter, NewLiteral(p, type, last, e.loc), e.loc));
        std::vector<ExprId> stmts = OnePass(p, e.b, plan, trips, -1);
        out.insert(out.end(), stmts.begin(), stmts.end());
        return true;
    }
    out = OnePass(p, e.b, plan, trips, step);
    // where the counter ends: its start plus a step per pass
    Accumulator count;
    count.stmt = step;
    count.var = counter;
    count.op = Op::Add;
    count.step = s;
    out.push_back(Accumulate(p, count, trips, -1));
    return true;
}

// a loop named only here whose body is named only by it
bool Collapsible(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    if (Shared(p, id) || (e.kind != ExprKind::While && e.kind != ExprKind::For)) {
        return false;
    }
    return !Shared(p, e.kind == ExprKind::For ? e.c : e.b);
}

bool TryCollapse(Collapser& c, ExprId loop, ExprId before, std::vector<ExprId>& out) {
    // the round after the passes settled only unrolls and unswitches (unroll.cpp,
    // unswitch.cpp): collapse found nothing in the round before, and nothing changed since
    if (c.p->unroll || c.p->unswitch) {
        return (c.p->unroll && UnrollLoop(*c.p, c.function, loop, before, out)) ||
               (c.p->unswitch && UnswitchLoop(*c.p, c.memory, c.function, loop, out));
    }
    // (if x (continue)) items first become elses (collapse_continue.cpp)
    c.changes += ContinuesToElse(*c.p, loop);
    if (At(*c.p, loop).kind == ExprKind::For) {
        return CollapseFor(c, loop, out);
    }
    return CollapseWhile(c, loop, before, out) || ScanToEnd(*c.p, loop, before, out) ||
           RunsOnce(*c.p, loop, out);
}

void Visit(Collapser& c, ExprId id, bool shared);

// a do's loop items collapsed, each after the loops inside it
void VisitDo(Collapser& c, ExprId id, bool shared) {
    Pass& p = *c.p;
    std::uint32_t i = 0;
    while (i < At(p, id).count) {
        ExprId item = Item(p, At(p, id), i);
        ExprId saved_list = c.outer_list;
        std::uint32_t saved_index = c.outer_index;
        const Expr& e = At(p, item);
        if ((e.kind == ExprKind::If || e.kind == ExprKind::Match) && IsPure(p, e.a)) {
            c.outer_list = id;
            c.outer_index = i;
        } else if (!LetsBefore(p, id, i)) {
            c.outer_list = -1;
        }
        Visit(c, item, shared);
        c.outer_list = saved_list;
        c.outer_index = saved_index;
        ExprId before = CounterBefore(p, id, i);
        if (c.outer_list >= 0 && LetsBefore(p, id, i) && !LetOfCounter(p, before, item)) {
            ExprId outer = OuterCounterLet(p, c.outer_list, c.outer_index, item);
            before = outer >= 0 ? outer : before;
        }
        std::vector<ExprId> stmts;
        if (shared || !Collapsible(p, item) || !TryCollapse(c, item, before, stmts)) {
            i++;
            continue;
        }
        c.changes++;
        for (ExprId stmt : stmts) {
            Retain(p, stmt);
        }
        Expr d = At(p, id);
        std::vector<ExprId> items(p.program->lists.begin() + d.first,
                                  p.program->lists.begin() + d.first + d.count);
        items.erase(items.begin() + i);
        items.insert(items.begin() + i, stmts.begin(), stmts.end());
        Release(p, item); // what stands for it names its parts now
        Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
        out.first = static_cast<std::uint32_t>(p.program->lists.size());
        out.count = static_cast<std::uint32_t>(items.size());
        p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
        // what stands for it was looked at inside the loop already
        i += static_cast<std::uint32_t>(stmts.size());
    }
}

// a loop in an if's arm or a clause's body: what stands for it, in a new do
ExprId VisitSlot(Collapser& c, ExprId slot, bool shared) {
    Pass& p = *c.p;
    Visit(c, slot, shared);
    std::vector<ExprId> stmts;
    ExprId before = c.outer_list >= 0 ? OuterCounterLet(p, c.outer_list, c.outer_index, slot) : -1;
    if (shared || !Collapsible(p, slot) || !TryCollapse(c, slot, before, stmts)) {
        return slot;
    }
    c.changes++;
    ExprId wrap = NewList(p, ExprKind::Do, kVoidType, stmts, At(p, slot).loc);
    Retain(p, wrap);
    Release(p, slot);
    return wrap;
}

void Visit(Collapser& c, ExprId id, bool shared) {
    if (id < 0) {
        return;
    }
    Pass& p = *c.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::Do:
        VisitDo(c, id, shared);
        return;
    case ExprKind::If:
        Visit(c, e.a, shared);
        for (int arm = 0; arm < 2; arm++) {
            ExprId slot = arm == 0 ? e.b : e.c;
            if (slot < 0) {
                continue;
            }
            slot = VisitSlot(c, slot, shared || e.type != kVoidType);
            Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
            (arm == 0 ? out.b : out.c) = slot;
        }
        return;
    case ExprKind::Match:
        Visit(c, e.a, shared);
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId slot = p.program->clauses[e.first + i].body;
            p.program->clauses[e.first + i].body =
                VisitSlot(c, slot, shared || e.type != kVoidType);
        }
        return;
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::FnRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::Store:
    case ExprKind::LoadField:
    case ExprKind::StoreField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Call:
    case ExprKind::CallPtr:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
    case ExprKind::AuxSwitch:
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::Return:
    case ExprKind::Let:
    case ExprKind::Set:
    case ExprKind::While:
    case ExprKind::For: {
        ExprId saved = c.outer_list;
        c.outer_list = -1;
        EachChild(p, e, [&](ExprId child) { Visit(c, child, shared); });
        c.outer_list = saved;
        return;
    }
    default:
        return; // not known here
    }
}

} // namespace

std::uint64_t CollapseLoops(Pass& p) {
    Collapser c;
    c.p = &p;
    for (std::size_t f = 0; f < p.program->functions.size(); f++) {
        c.function = static_cast<std::int32_t>(f);
        Visit(c, p.program->functions[f].body, false);
    }
    return c.changes;
}

} // namespace tisple::opt
