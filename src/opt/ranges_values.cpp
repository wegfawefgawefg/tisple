// The values an expression may hold (ranges.cpp decides compares with them): from literals,
// masks, remainders and quotients, shifts, sums and products (a sum that wraps once wholly,
// as v - 23469 does for v in 11902..12071, lands in 53969..54138), widenings, const tables,
// elements no store reaches, the loop counters and sets around it, and the ranges of
// variables (ranges_vars.cpp).

#include "check/arith.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

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

namespace {

bool Within(const Range& r, const Range& of) {
    return r.lo >= of.lo && r.hi <= of.hi;
}

Range Meet(Range a, Range b) {
    return Range{std::max(a.lo, b.lo), std::min(a.hi, b.hi)};
}

// a sum or difference: in the type, or wrapped once as a whole (all of it below or above)
Range Wrapped(Range r, Range type) {
    std::int64_t span = type.hi - type.lo + 1;
    for (std::int64_t shift : {std::int64_t(0), span, -span}) {
        Range moved{r.lo + shift, r.hi + shift};
        if (Within(moved, type)) {
            return moved;
        }
    }
    return type;
}

// a signed quotient or remainder by a divisor that is never zero or negative: the
// quotient's extremes lie at the corners, the remainder keeps the dividend's sign
Range SignedDivide(Op op, Range a, Range b, Range type) {
    if (b.lo <= 0) {
        return type;
    }
    if (op == Op::Mod) {
        std::int64_t most = b.hi - 1;
        return Range{a.lo >= 0 ? 0 : std::max(a.lo, -most), a.hi <= 0 ? 0 : std::min(a.hi, most)};
    }
    std::int64_t q[4] = {a.lo / b.lo, a.lo / b.hi, a.hi / b.lo, a.hi / b.hi};
    return Range{*std::min_element(q, q + 4), *std::max_element(q, q + 4)};
}

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
    Range a = ValueRange(p, counters, e.a);
    Range b = ValueRange(p, counters, e.b);
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
        // x | y and x ^ y are at most x + y: (| x 1) for x in 0..8 is at most 9
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{std::max(a.lo, b.lo), std::min(OnesAbove(std::max(a.hi, b.hi)), a.hi + b.hi)};
        }
        break;
    case Op::Xor:
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{0, std::min(OnesAbove(std::max(a.hi, b.hi)), a.hi + b.hi)};
        }
        break;
    case Op::Add:
        return Wrapped(Range{a.lo + b.lo, a.hi + b.hi}, type);
    case Op::Sub:
        return Wrapped(Range{a.lo - b.hi, a.hi - b.lo}, type);
    case Op::Mul:
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{a.lo * b.lo, a.hi * b.hi};
        }
        break;
    case Op::Div:
        // by zero: all ones, outside these ranges
        if (a.lo >= 0 && b.lo > 0) {
            r = Range{a.lo / b.hi, a.hi / b.lo};
        } else {
            r = SignedDivide(e.op, a, b, type);
        }
        break;
    case Op::Mod:
        // by zero: the dividend
        if (a.lo >= 0 && b.lo >= 0) {
            r = Range{0, b.lo > 0 ? std::min(a.hi, b.hi - 1) : a.hi};
        } else {
            r = SignedDivide(e.op, a, b, type);
        }
        break;
    case Op::Shr:
        if (a.lo >= 0 && b.lo >= 0 && b.hi < 16) {
            r = Range{a.lo >> b.hi, a.hi >> b.lo};
        }
        break;
    case Op::Shl:
        // by the width or more: 0, as the target shifts
        if (a.lo >= 0 && b.lo >= 0 && b.lo == b.hi) {
            bool out = b.lo >= static_cast<std::int64_t>(KindBits(KindOf(*p.program, e.type)));
            r = out ? Range{0, 0} : Range{a.lo << b.lo, a.hi << b.lo};
        }
        break;
    default:
        break;
    }
    return Within(r, type) ? r : type;
}

// a variable's read: what the counters and sets around it say, and the round's ranges
Range VarRange(const Pass& p, const Counters& counters, std::int32_t var, Range type) {
    Range r = type;
    for (const Counter& counter : counters) {
        if (counter.var == var) {
            r = Meet(r, counter.range);
        }
    }
    const VarRanges& known = p.ranges;
    if (known.vars && static_cast<std::size_t>(var) < known.natural.size()) {
        r = Meet(r, known.natural[static_cast<std::size_t>(var)]);
    }
    // none: code no run reaches, where anything holds; the type says no less
    return r.lo <= r.hi ? r : type;
}

// a read of an element or a field: a const table's, or what no store reaches still holds
Range ElementRange(const Pass& p, const Counters& counters, const Expr& e, Range type) {
    if (e.kind == ExprKind::Index) {
        Range table = TableRange(p, e, type);
        if (table.lo != type.lo || table.hi != type.hi) {
            return table;
        }
    }
    ExprId index = ReadIndex(p, e);
    Range at = index >= 0 ? ValueRange(p, counters, index) : Range{0, 0};
    Range out;
    return UnstoredRead(p, e, at, out) && Within(out, type) ? out : type;
}

Range WorkOut(const Pass& p, const Counters& counters, ExprId id);

} // namespace

Range ValueRange(const Pass& p, const Counters& counters, ExprId id) {
    Pass::RangeMemo& memo = p.memo;
    std::size_t at = static_cast<std::size_t>(id);
    if (!memo.on || at >= memo.stamp.size()) {
        return WorkOut(p, counters, id);
    }
    if (memo.stamp[at] != memo.now) {
        memo.range[at] = WorkOut(p, counters, id);
        memo.stamp[at] = memo.now;
    }
    return memo.range[at];
}

namespace {

Range WorkOut(const Pass& p, const Counters& counters, ExprId id) {
    const Expr& e = At(p, id);
    Range type = TypeRange(p, e.type);
    TypeKind kind = KindOf(*p.program, e.type);
    if (!IsIntegerKind(kind) && kind != TypeKind::Bool && kind != TypeKind::Enum) {
        return type;
    }
    std::uint16_t c = 0;
    if (ConstValue(*p.program, id, c)) {
        std::int64_t v = SignedValue(kind, c);
        return Range{v, v};
    }
    switch (e.kind) {
    case ExprKind::VarRef:
        return VarRange(p, counters, e.ref, type);
    case ExprKind::Convert: {
        Range inner = ValueRange(p, counters, e.a);
        return Within(inner, type) ? inner : type;
    }
    case ExprKind::Index:
    case ExprKind::Field:
        return ElementRange(p, counters, e, type);
    case ExprKind::Binary:
        return IsIntegerKind(kind) ? BinaryRange(p, counters, e, type) : type;
    case ExprKind::If: {
        // either arm's: (if (= opaque 0) E.e0 E.e1) is e0 or e1
        if (e.c < 0) {
            return type;
        }
        Range a = ValueRange(p, counters, e.b);
        Range b = ValueRange(p, counters, e.c);
        return Range{std::min(a.lo, b.lo), std::max(a.hi, b.hi)};
    }
    default:
        return type;
    }
}

} // namespace

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
    case ExprKind::AddrOf:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return Quiet(p, e.a) && Quiet(p, e.b);
    case ExprKind::If:
        return Quiet(p, e.a) && Quiet(p, e.b) && Quiet(p, e.c);
    default:
        return false;
    }
}

namespace {

bool IsRead(const Pass& p, ExprId id, std::int32_t var) {
    const Expr& e = At(p, id);
    return e.kind == ExprKind::VarRef && e.ref == var;
}

// the range of a counting while's counter in its body, when it is one: c lies in
// [v, k - 1 + s] for (let c v) just before and (while (< c k) ... (set c (+ c s)) ...)
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

} // namespace

bool LoopCounter(const Pass& p, const Counters& counters, ExprId loop, ExprId before,
                 Counter& out) {
    const Expr& e = At(p, loop);
    if (e.kind == ExprKind::While) {
        return WhileCounter(p, loop, before, out);
    }
    Range start = ValueRange(p, counters, e.a);
    Range end = e.b >= 0 ? ValueRange(p, counters, e.b) : TypeRange(p, VarOf(p, e.ref).type);
    out.var = e.ref;
    out.range = Range{start.lo, e.b >= 0 ? end.hi - 1 : end.hi};
    return out.range.lo <= out.range.hi && !VarOf(p, e.ref).address_taken;
}

Range OtherView(TypeKind kind, Range r) {
    std::int64_t span = KindBits(kind) == 8 ? 256 : 65536;
    if (IsSignedKind(kind)) {
        if (r.lo >= 0) {
            return r;
        }
        return r.hi < 0 ? Range{r.lo + span, r.hi + span} : Range{0, span - 1};
    }
    if (r.hi < span / 2) {
        return r;
    }
    return r.lo >= span / 2 ? Range{r.lo - span, r.hi - span} : Range{-span / 2, span / 2 - 1};
}

bool RangedVar(const Pass& p, std::int32_t var) {
    const Var& v = VarOf(p, var);
    TypeKind kind = KindOf(*p.program, v.type);
    bool scalar = IsIntegerKind(kind) || kind == TypeKind::Bool || kind == TypeKind::Enum;
    return scalar && v.kind != VarKind::ConstArray && !v.address_taken &&
           !v.placement.has_address && !v.placement.in_aux && !v.placement.reserve;
}

void ShapeRange(const Pass& p, ExprId id, std::int32_t& lo, std::int32_t& hi) {
    Range r = ValueRange(p, Counters(), id);
    lo = static_cast<std::int32_t>(r.lo);
    hi = static_cast<std::int32_t>(r.hi);
}

} // namespace tisple::opt
