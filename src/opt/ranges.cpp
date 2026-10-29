// Value ranges (docs/midlevel-optimizer.md, pass 8): the values an expression may hold, from
// its shape and the loop counters around it, decide compares and cut match clauses.
//
// - Shapes: literals, (& x m) with m not negative, remainders and quotients by a constant,
//   shifts right, sums and products that cannot wrap, widenings, reads of const tables.
// - Counters: in a for's body its variable lies in the range (the body cannot set it); in a
//   counting while, (let c v) just before and (while (< c k) ... (set c (+ c s)) ...) with
//   that step its one write, c lies in [v, k - 1 + s].
// - A compare whose operands' ranges decide it becomes a literal; a match clause no value in
//   the range reaches goes, and a match left with only its wildcard becomes that clause; an
//   and or or with a literal on its right is decided by it.
//   Only when the operands are pure and read no hardware (:address, aux, loads), which may
//   answer differently or change when read.

#include "check/arith.hpp"
#include "opt/licm.hpp"
#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

struct Range {
    std::int64_t lo = 0;
    std::int64_t hi = 0;
};

struct Counter {
    std::int32_t var = -1;
    Range range;
};

using Counters = std::vector<Counter>; // the loops around a node, innermost last

struct Ranger {
    Pass* p = nullptr;
    Counters counters;
    std::uint64_t changes = 0;
};

Range TypeRange(const Pass& p, TypeId type) {
    TypeKind kind = KindOf(*p.program, type);
    if (kind == TypeKind::Bool) {
        return Range{0, 1};
    }
    std::int32_t span = KindBits(kind) == 8 ? 256 : 65536;
    if (IsSignedKind(kind)) {
        return Range{-span / 2, span / 2 - 1};
    }
    return Range{0, span - 1};
}

bool Within(const Range& r, const Range& of) {
    return r.lo >= of.lo && r.hi <= of.hi;
}

Range Of(const Pass& p, const Counters& counters, ExprId id);

// the smallest all-ones value at or above v (v not negative)
std::int64_t OnesAbove(std::int64_t v) {
    std::int64_t ones = 0;
    while (ones < v) {
        ones = ones * 2 + 1;
    }
    return ones;
}

// the values of a const table's integer elements
Range TableRange(const Pass& p, const Expr& index, Range type) {
    const Expr& base = At(p, index.a);
    if (base.kind != ExprKind::VarRef || VarOf(p, base.ref).kind != VarKind::ConstArray) {
        return type;
    }
    const Var& var = VarOf(p, base.ref);
    const Type& array = TypeOf(*p.program, var.type);
    std::uint32_t size = TypeOf(*p.program, array.elem).size;
    TypeKind kind = KindOf(*p.program, array.elem);
    if (!IsIntegerKind(kind) || var.init.size() < array.count * size || array.count == 0) {
        return type;
    }
    Range r{type.hi, type.lo};
    for (std::uint32_t i = 0; i < array.count; i++) {
        std::uint32_t bits = 0;
        for (std::uint32_t b = 0; b < size; b++) {
            bits |= static_cast<std::uint32_t>(var.init[i * size + b]) << (8 * b);
        }
        std::int64_t v = SignedValue(kind, static_cast<std::uint16_t>(bits));
        r.lo = std::min(r.lo, v);
        r.hi = std::max(r.hi, v);
    }
    return r;
}

Range BinaryRange(const Pass& p, const Counters& counters, const Expr& e, Range type) {
    Range a = Of(p, counters, e.a);
    Range b = Of(p, counters, e.b);
    Range r = type;
    switch (e.op) {
    case Op::And:
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{0, std::min(a.hi, b.hi)};
        } else if (a.lo >= 0 || b.lo >= 0) {
            r = Range{0, a.lo >= 0 ? a.hi : b.hi};
        }
        return r;
    case Op::Or:
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{std::max(a.lo, b.lo), OnesAbove(std::max(a.hi, b.hi))};
        }
        break;
    case Op::Xor:
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{0, OnesAbove(std::max(a.hi, b.hi))};
        }
        break;
    case Op::Add:
        r = Range{a.lo + b.lo, a.hi + b.hi};
        break;
    case Op::Sub:
        r = Range{a.lo - b.hi, a.hi - b.lo};
        break;
    case Op::Mul:
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{a.lo * b.lo, a.hi * b.hi};
        }
        break;
    case Op::Div:
        // by zero: all ones, outside these ranges
        if (a.lo >= 0 && b.lo > 0) {
            r = Range{a.lo / b.hi, a.hi / b.lo};
        }
        break;
    case Op::Mod:
        // by zero: the dividend
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{0, b.lo > 0 ? std::min(a.hi, b.hi - 1) : a.hi};
        }
        break;
    case Op::Shr:
        if (a.lo >= 0 && b.lo >= 0 && b.hi < 16) {
            r = Range{a.lo >> b.hi, a.hi >> b.lo};
        }
        break;
    default:
        break;
    }
    return Within(r, type) ? r : type;
}

Range Of(const Pass& p, const Counters& counters, ExprId id) {
    const Expr& e = At(p, id);
    Range type = TypeRange(p, e.type);
    TypeKind kind = KindOf(*p.program, e.type);
    if (!IsIntegerKind(kind) && kind != TypeKind::Bool) {
        return type;
    }
    std::uint16_t c = 0;
    if (ConstValue(*p.program, id, c)) {
        std::int64_t v = SignedValue(kind, c);
        return Range{v, v};
    }
    switch (e.kind) {
    case ExprKind::VarRef:
        for (const Counter& counter : counters) {
            if (counter.var == e.ref) {
                return counter.range;
            }
        }
        return type;
    case ExprKind::Convert: {
        Range inner = Of(p, counters, e.a);
        return Within(inner, type) ? inner : type;
    }
    case ExprKind::Index:
        return TableRange(p, e, type);
    case ExprKind::Binary:
        return IsIntegerKind(kind) ? BinaryRange(p, counters, e, type) : type;
    default:
        return type;
    }
}

// pure, and reads no hardware: safe to work out never
bool Quiet(const Pass& p, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
        return true;
    case ExprKind::VarRef: {
        const Var& v = VarOf(p, e.ref);
        return !v.placement.has_address && !v.placement.in_aux && !v.placement.reserve;
    }
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::And:
    case ExprKind::Or:
        return Quiet(p, e.a) && Quiet(p, e.b);
    case ExprKind::If:
        return Quiet(p, e.a) && Quiet(p, e.b) && Quiet(p, e.c);
    default:
        return false;
    }
}

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

void FoldCompare(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    const Expr& e = At(p, id);
    TypeKind operand = KindOf(*p.program, At(p, e.a).type);
    if (!IsIntegerKind(operand) || !Quiet(p, e.a) || !Quiet(p, e.b)) {
        return;
    }
    int known = Decide(e.op, Of(p, g.counters, e.a), Of(p, g.counters, e.b));
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

// clauses no value of the scrutinee reaches go; a match left with its wildcard alone becomes
// it (true then)
bool CutClauses(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    TypeKind kind = KindOf(*p.program, At(p, e.a).type);
    if (!IsIntegerKind(kind) || !Quiet(p, e.a)) {
        return false;
    }
    Range r = Of(p, g.counters, e.a);
    std::vector<Clause> kept;
    bool wildcard = false;
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = p.program->clauses[e.first + i];
        std::int64_t v = SignedValue(kind, clause.value);
        if (clause.wildcard || (v >= r.lo && v <= r.hi)) {
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

void Visit(Ranger& g, ExprId id, bool shared, ExprId before);

bool IsRead(const Pass& p, ExprId id, std::int32_t var) {
    const Expr& e = At(p, id);
    return e.kind == ExprKind::VarRef && e.ref == var;
}

// the range of a counting while's counter in its body, when it is one
bool WhileCounter(const Pass& p, ExprId loop, ExprId before, Counter& out) {
    if (before < 0 || KnownTrips(p, loop, before) < 0) {
        return false;
    }
    const Expr& cond = At(p, At(p, loop).a);
    std::int32_t c = At(p, cond.a).ref;
    const Expr& body = At(p, At(p, loop).b);
    if (VarOf(p, c).address_taken || body.kind != ExprKind::Do) {
        return false;
    }
    TypeKind kind = KindOf(*p.program, VarOf(p, c).type);
    std::uint16_t start = 0;
    std::uint16_t end = 0;
    ConstValue(*p.program, At(p, before).a, start);
    ConstValue(*p.program, cond.b, end);
    for (std::uint32_t i = 0; i < body.count; i++) {
        const Expr& set = At(p, Item(p, body, i));
        if (set.kind != ExprKind::Set || !IsRead(p, set.a, c)) {
            continue;
        }
        std::uint16_t step = 0;
        ConstValue(*p.program, At(p, set.b).b, step);
        std::int64_t from = SignedValue(kind, start);
        std::int64_t to = SignedValue(kind, end);
        out.var = c;
        out.range = Range{std::min(from, to - 1), to - 1 + SignedValue(kind, step)};
        return true;
    }
    return false;
}

void VisitLoop(Ranger& g, ExprId id, bool shared, ExprId before) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    Counter counter;
    bool counted = false;
    if (e.kind == ExprKind::For) {
        Visit(g, e.a, shared, -1);
        Visit(g, e.b, shared, -1);
        Range start = Of(p, g.counters, e.a);
        Range end = e.b >= 0 ? Of(p, g.counters, e.b) : TypeRange(p, VarOf(p, e.ref).type);
        counter.var = e.ref;
        counter.range = Range{start.lo, e.b >= 0 ? end.hi - 1 : end.hi};
        counted = counter.range.lo <= counter.range.hi && !VarOf(p, e.ref).address_taken;
    } else {
        counted = WhileCounter(p, id, before, counter);
    }
    if (counted) {
        g.counters.push_back(counter);
    }
    if (e.kind == ExprKind::While) {
        Visit(g, e.a, shared, -1);
    }
    Visit(g, e.kind == ExprKind::For ? e.c : e.b, shared, -1);
    if (counted) {
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
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            Visit(g, Item(p, e, i), shared, i > 0 ? Item(p, e, i - 1) : -1);
        }
        return;
    case ExprKind::While:
    case ExprKind::For:
        VisitLoop(g, id, shared, before);
        return;
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::Binary:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        if (!shared) {
            FoldCompare(g, id);
        }
        return;
    case ExprKind::And:
    case ExprKind::Or:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        if (!shared) {
            FoldLogic(g, id);
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
    case ExprKind::Load:
    case ExprKind::Store:
    case ExprKind::LoadField:
    case ExprKind::StoreField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Call:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::If:
    case ExprKind::Return:
    case ExprKind::Let:
    case ExprKind::Set:
        EachChild(p, e, [&](ExprId child) { Visit(g, child, shared, -1); });
        return;
    default:
        return; // not known here
    }
}

} // namespace

void ShapeRange(const Pass& p, ExprId id, std::int32_t& lo, std::int32_t& hi) {
    Range r = Of(p, Counters(), id);
    lo = static_cast<std::int32_t>(r.lo);
    hi = static_cast<std::int32_t>(r.hi);
}

std::uint64_t FoldRanges(Pass& p) {
    Ranger g;
    g.p = &p;
    for (std::size_t f = 0; f < p.program->functions.size(); f++) {
        Visit(g, p.program->functions[f].body, false, -1);
    }
    return g.changes;
}

} // namespace tisple::opt
