// The values an expression may hold (ranges.cpp decides compares with them): from literals,
// masks, remainders and quotients by constants, shifts right, sums and products that cannot
// wrap, widenings, const tables, and the loop counters around it.

#include "check/arith.hpp"
#include "opt/ranges.hpp"

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

} // namespace

Range ValueRange(const Pass& p, const Counters& counters, ExprId id) {
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
        Range inner = ValueRange(p, counters, e.a);
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

void ShapeRange(const Pass& p, ExprId id, std::int32_t& lo, std::int32_t& hi) {
    Range r = ValueRange(p, Counters(), id);
    lo = static_cast<std::int32_t>(r.lo);
    hi = static_cast<std::int32_t>(r.hi);
}

} // namespace tisple::opt
