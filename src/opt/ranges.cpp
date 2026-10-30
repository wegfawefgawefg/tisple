// Value ranges (docs/midlevel-optimizer.md, pass 8): the values an expression may hold, from
// its shape and the loop counters around it, decide compares and cut match clauses.
//
// - Shapes: literals, (& x m) with m not negative, remainders and quotients, shifts, sums and
//   products, widenings, reads of const tables and of elements no store reaches.
// - Counters: in a for's body its variable lies in the range (the body cannot set it); in a
//   counting while, (let c v) just before and (while (< c k) ... (set c (+ c s)) ...) with
//   that step its one write, c lies in [v, k - 1 + s].
// - Sets: after (set x v) or (let x v) among a do's items, x holds v's range in the items
//   after it, until something may write x: a set or let of it, a call whose summary writes it
//   (or does I/O or jsr), io_write and jsr for globals, a node not known here; a loop forgets
//   from its start what it writes anywhere in it. (set g (% g 9)) (set g (/ g 9)) is g = 0.
// - Variables: what the round's ranges (ranges_vars.cpp) say each may ever hold.
// - Const data: a read at places its indexes' ranges allow holds the least to the most of the
//   values there, and when they are few, only those (const_data.cpp).
// - A compare whose operands' ranges decide it becomes a literal, and (= x k) of a const read x
//   none of whose few values is k; a value whose range is one value becomes it; a match clause
//   no value in the range (or of the few) reaches goes, and a match left with only its
//   wildcard becomes that clause; an and or or with a literal on its right is
//   decided by it. (~ (~ x)) and (not (not x)) are x, which leaves (set g (~ (~ g))) a store
//   of g itself.
//   Only when the operands are pure and read no hardware (:address, aux, loads), which may
//   answer differently or change when read.

#include "opt/ranges.hpp"

#include "check/arith.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

// 1 or 0 when the ranges decide the compare, -1 when they do not
int Decide(Op op, Range a, Range b) {
    switch (op) {
    case Op::Lt:
        return a.hi < b.lo ? 1 : a.lo >= b.hi ? 0 : -1;
    case Op::Le:
        return a.hi <= b.lo ? 1 : a.lo > b.hi ? 0 : -1;
    case Op::Gt:
        return a.lo > b.hi ? 1 : a.hi <= b.lo ? 0 : -1;
    case Op::Ge:
        return a.lo >= b.hi ? 1 : a.hi < b.lo ? 0 : -1;
    case Op::Eq:
        return a.hi < b.lo || b.hi < a.lo ? 0 : -1;
    case Op::Ne:
        return a.hi < b.lo || b.hi < a.lo ? 1 : -1;
    default:
        return -1;
    }
}

// the few values a read of const data may give (ConstDataRange); empty when it is not one,
// or there are many
std::vector<std::int64_t> DataValues(const Ranger& g, ExprId id) {
    Range unused;
    std::vector<std::int64_t> values;
    if (!ConstDataRange(*g.p, g.counters, id, unused, &values)) {
        values.clear();
    }
    return values;
}

bool Among(const std::vector<std::int64_t>& values, std::int64_t v) {
    for (std::int64_t w : values) {
        if (w == v) {
            return true;
        }
    }
    return false;
}

// (= read k) or (!= read k) with read's few values not holding k, k one value
int DecideByValues(const Ranger& g, const Expr& e, Range a, Range b) {
    if (e.op != Op::Eq && e.op != Op::Ne) {
        return -1;
    }
    for (int side = 0; side < 2; side++) {
        Range other = side == 0 ? b : a;
        std::vector<std::int64_t> values = DataValues(g, side == 0 ? e.a : e.b);
        if (!values.empty() && other.lo == other.hi && !Among(values, other.lo)) {
            return e.op == Op::Eq ? 0 : 1;
        }
    }
    return -1;
}

void FoldCompare(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    const Expr& e = At(p, id);
    TypeKind operand = KindOf(*p.program, At(p, e.a).type);
    bool ordered = IsIntegerKind(operand) || operand == TypeKind::Enum;
    if (!ordered || !Quiet(p, e.a) || !Quiet(p, e.b)) {
        return;
    }
    Range a = ValueRange(p, g.counters, e.a);
    Range b = ValueRange(p, g.counters, e.b);
    int known = Decide(e.op, a, b);
    if (known < 0) {
        known = DecideByValues(g, e, a, b);
    }
    if (known < 0 || !Allow(p, "compare decided by ranges", id, -1)) {
        return;
    }
    ReplaceWithLiteral(p, id, static_cast<std::uint16_t>(known));
    g.changes++;
}

// (and x false) and (or x true), x quiet, are their right side; (and x true) and (or x false)
// are x (constant folding decides them on their left side only)
void FoldLogic(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    std::uint16_t right = 0;
    if (!ConstValue(*p.program, e.b, right)) {
        return;
    }
    bool decides = (e.kind == ExprKind::And) == (right == 0);
    if (decides && !Quiet(p, e.a)) {
        return;
    }
    if (!Allow(p, "and or decided on its right", id, -1)) {
        return;
    }
    if (decides) {
        ReplaceWithLiteral(p, id, right != 0 ? 1 : 0);
    } else {
        ReplaceWith(p, id, e.a);
    }
    g.changes++;
}

// (~ (~ x)) and (not (not x)) are x
void FoldTwice(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    const Expr& inner = At(p, e.a);
    if (inner.kind != ExprKind::Unary || inner.op != e.op || inner.type != e.type ||
        !Allow(p, "not of a not", id, -1)) {
        return;
    }
    ReplaceWith(p, id, inner.a);
    g.changes++;
}

// clauses no value of the scrutinee reaches go; a match left with its wildcard alone becomes
// it (true then)
bool CutClauses(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    TypeKind kind = KindOf(*p.program, At(p, e.a).type);
    if ((!IsIntegerKind(kind) && kind != TypeKind::Enum) || !Quiet(p, e.a)) {
        return false;
    }
    Range r = ValueRange(p, g.counters, e.a);
    std::vector<std::int64_t> values = DataValues(g, e.a);
    std::vector<Clause> kept;
    bool wildcard = false;
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = p.program->clauses[e.first + i];
        std::int64_t v = SignedValue(kind, clause.value);
        bool reached = v >= r.lo && v <= r.hi && (values.empty() || Among(values, v));
        if (clause.wildcard || reached) {
            kept.push_back(clause);
            wildcard = wildcard || clause.wildcard;
        }
    }
    if (kept.size() == e.count || (!wildcard && e.type != kVoidType) ||
        !Allow(p, "match clauses cut by ranges", id, -1)) {
        return false;
    }
    g.changes++;
    if (kept.size() == 1 && wildcard && At(p, kept[0].body).type == e.type) {
        ReplaceWith(p, id, kept[0].body);
        return true;
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = p.program->clauses[e.first + i];
        bool stays = false;
        for (const Clause& k : kept) {
            stays = stays || k.body == clause.body;
        }
        if (!stays) {
            Release(p, clause.body);
        }
    }
    Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
    out.first = static_cast<std::uint32_t>(p.program->clauses.size());
    out.count = static_cast<std::uint32_t>(kept.size());
    p.program->clauses.insert(p.program->clauses.end(), kept.begin(), kept.end());
    return false;
}

// an integer or enum value whose range is one value becomes that literal (a parameter every
// call passes 65535, an element no store reaches, (/ g 9) for g in 0..8)
void FoldValue(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    const Expr& e = At(p, id);
    TypeKind kind = KindOf(*p.program, e.type);
    if (!IsIntegerKind(kind) && kind != TypeKind::Enum) {
        return;
    }
    // the range first: it is rarely one value, and the walks below are as long
    Range r = ValueRange(p, g.counters, id);
    if (r.lo != r.hi || !Quiet(p, id) || !IsPure(p, id) ||
        !Allow(p, "value decided by ranges", id, e.kind == ExprKind::VarRef ? e.ref : -1)) {
        return;
    }
    ReplaceWithLiteral(p, id, MaskToKind(kind, static_cast<std::uint32_t>(r.lo)));
    g.changes++;
}

void Visit(Ranger& g, ExprId id, bool shared, ExprId before);

// a place's index expressions and the pointer it may be behind; its root is not a value
void VisitPlace(Ranger& g, ExprId id, bool shared) {
    Pass& p = *g.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return;
    case ExprKind::Index:
        VisitPlace(g, e.a, shared);
        Visit(g, e.b, shared, -1);
        return;
    case ExprKind::Field:
        VisitPlace(g, e.a, shared);
        return;
    default:
        Visit(g, id, shared, -1);
        return;
    }
}

void VisitLoop(Ranger& g, ExprId id, bool shared, ExprId before) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    Counter counter;
    if (e.kind == ExprKind::For) {
        Visit(g, e.a, shared, -1);
        Visit(g, e.b, shared, -1);
    }
    // a for's range is worked out once, before the loop runs
    bool counted = LoopCounter(p, g.counters, id, before, counter);
    ForgetLoopWrites(g, id);
    if (counted) {
        Changed(g);
        g.counters.push_back(counter);
    }
    if (e.kind == ExprKind::While) {
        Visit(g, e.a, shared, -1);
    }
    Visit(g, e.kind == ExprKind::For ? e.c : e.b, shared, -1);
    if (counted) {
        Changed(g);
        g.counters.pop_back();
    }
}

void Visit(Ranger& g, ExprId id, bool shared, ExprId before) {
    if (id < 0) {
        return;
    }
    Pass& p = *g.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::Do: {
        // the facts this do's sets give hold to its end
        std::size_t outer = g.counters.size();
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId item = Item(p, At(p, id), i);
            Visit(g, item, shared, CounterBefore(p, id, i));
            Range type = TypeRange(p, g.written.var >= 0 ? VarOf(p, g.written.var).type : 0);
            bool narrow = g.written.var >= 0 &&
                          (g.written.range.lo > type.lo || g.written.range.hi < type.hi);
            if (g.last_write == item && narrow) {
                Changed(g);
                g.counters.push_back(g.written);
            }
            g.last_write = -1;
        }
        if (g.counters.size() != outer) {
            Changed(g);
            g.counters.resize(outer);
        }
        return;
    }
    case ExprKind::While:
    case ExprKind::For:
        VisitLoop(g, id, shared, before);
        return;
    case ExprKind::Literal:
    case ExprKind::FnRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::VarRef:
        if (!shared) {
            FoldValue(g, id);
        }
        return;
    case ExprKind::Binary:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        if (!shared) {
            FoldDivision(g, id);
        }
        if (!shared && At(p, id).kind == ExprKind::Binary) {
            FoldCompare(g, id);
        }
        if (!shared && At(p, id).kind == ExprKind::Binary) {
            FoldValue(g, id);
        }
        return;
    case ExprKind::And:
    case ExprKind::Or:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        if (!shared) {
            FoldLogic(g, id);
        }
        return;
    case ExprKind::Unary:
        Visit(g, e.a, shared, -1);
        if (!shared) {
            FoldTwice(g, id);
        }
        if (!shared && At(p, id).kind == ExprKind::Unary) {
            FoldValue(g, id);
        }
        return;
    case ExprKind::Match:
        Visit(g, e.a, shared, -1);
        if (!shared && CutClauses(g, id)) {
            Visit(g, id, shared, before); // the clause that stands there now
            return;
        }
        e = At(p, id);
        for (std::uint32_t i = 0; i < e.count; i++) {
            Visit(g, p.program->clauses[e.first + i].body, shared, -1);
        }
        return;
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Convert:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        if (!shared) {
            FoldValue(g, id);
        }
        return;
    case ExprKind::AddrOf:
        VisitPlace(g, e.a, shared);
        return;
    case ExprKind::Set:
        VisitPlace(g, e.a, shared);
        Visit(g, e.b, shared, -1);
        if (At(p, e.a).kind == ExprKind::VarRef) {
            Wrote(g, id, At(p, e.a).ref, e.b);
        }
        return;
    case ExprKind::Let:
        Visit(g, e.a, shared, -1);
        Wrote(g, id, e.ref, e.a);
        return;
    case ExprKind::Call:
    case ExprKind::CallPtr:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        ForgetCallWrites(g, e);
        return;
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
    case ExprKind::Interrupts:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        ForgetGlobals(g, false);
        return;
    case ExprKind::Load:
    case ExprKind::Store:
    case ExprKind::LoadField:
    case ExprKind::StoreField:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Reinterpret:
    case ExprKind::IoRead:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::If:
    case ExprKind::Return:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        return;
    default:
        ForgetGlobals(g, true); // not known here: it may write anything
        return;
    }
}

} // namespace

std::uint64_t FoldRanges(Pass& p) {
    Ranger g;
    g.p = &p;
    // each node's range is worked out once while what is around it stays (ValueRange)
    p.memo.on = true;
    p.memo.stamp.assign(p.program->exprs.size(), 0);
    p.memo.range.resize(p.program->exprs.size());
    for (std::size_t f = 0; f < p.program->functions.size(); f++) {
        Changed(g);
        Visit(g, p.program->functions[f].body, false, -1);
    }
    p.memo.on = false;
    return g.changes;
}

} // namespace tisple::opt
