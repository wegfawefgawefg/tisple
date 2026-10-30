// Values from the values of their operands, for constant propagation (consts_walk.cpp):
// the target's arithmetic (check/arith.hpp, as the interpreter does it), const array reads,
// and what holds whatever the operands are: x * 0, x & 0, x | all ones, a shift by the
// width or more, (- x x), (^ x x), (% x x), (= x x) and the like, (+ x (~ x)).

#include "backend/analysis.hpp"
#include "check/arith.hpp"
#include "opt/consts.hpp"

namespace tisple::opt {

namespace {

// (- x x), (^ x x), (% x x) (0 even for x = 0, whose remainder is x), (= x x) and the like
bool SameOperands(const Pass& p, const Expr& e, Val& v) {
    if (!SameExpr(*p.program, e.a, e.b) || !Steady(p, e.a)) {
        return false;
    }
    switch (e.op) {
    case Op::Sub:
    case Op::Xor:
    case Op::Mod:
    case Op::Ne:
    case Op::Lt:
    case Op::Gt:
        v = ConstOf(p, e.type, 0);
        return true;
    case Op::Eq:
    case Op::Le:
    case Op::Ge:
        v = ConstOf(p, e.type, 1);
        return true;
    default:
        return false;
    }
}

// x with (~ x): (+ x (~ x)), (| x (~ x)) and (^ x (~ x)) are all ones, (& x (~ x)) is 0 (no
// bit is set in both, so the sum never carries)
bool WithComplement(const Pass& p, const Expr& e, Val& v) {
    bool add = e.op == Op::Add || e.op == Op::Or || e.op == Op::Xor;
    if ((!add && e.op != Op::And) || !Steady(p, e.a) || !Steady(p, e.b)) {
        return false;
    }
    for (int side = 0; side < 2; side++) {
        const Expr& flip = At(p, side == 0 ? e.b : e.a);
        ExprId other = side == 0 ? e.a : e.b;
        if (flip.kind == ExprKind::Unary && flip.op == Op::BitNot && flip.type == e.type &&
            SameExpr(*p.program, flip.a, other)) {
            v = ConstOf(p, e.type, add ? 0xFFFF : 0);
            return true;
        }
    }
    return false;
}

} // namespace

Val AnyVal() {
    Val v;
    v.kind = Val::Any;
    return v;
}

Val ConstOf(const Pass& p, TypeId type, std::uint32_t bits) {
    Val v;
    v.kind = Val::Const;
    v.bits = MaskToKind(KindOf(*p.program, type), bits);
    return v;
}

Val Join(Val a, Val b) {
    if (a.kind == Val::None) {
        return b;
    }
    if (b.kind == Val::None || a == b) {
        return a;
    }
    return AnyVal();
}

// worked out twice in a row, it gives the same value: pure, no loads, no :address or aux
// variables (hardware may answer differently each time)
bool Steady(const Pass& p, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
        return true;
    case ExprKind::VarRef: {
        const Var& var = VarOf(p, e.ref);
        return !var.placement.has_address && !var.placement.in_aux && !var.irq_shared;
    }
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return Steady(p, e.a) && Steady(p, e.b);
    default:
        return false;
    }
}

// (at TABLE k) for a const array and a known k
Val ElementValue(const Pass& p, const Expr& index, Val k) {
    const Expr& base = At(p, index.a);
    if (base.kind != ExprKind::VarRef || k.kind != Val::Const ||
        !ConstType(*p.program, index.type)) {
        return AnyVal();
    }
    const Var& var = VarOf(p, base.ref);
    const Type& array = TypeOf(*p.program, var.type);
    if (var.kind != VarKind::ConstArray || array.kind != TypeKind::Array || k.bits >= array.count) {
        return AnyVal();
    }
    std::uint32_t size = TypeOf(*p.program, array.elem).size;
    std::size_t at = static_cast<std::size_t>(k.bits) * size;
    if (!var.init.empty() && at + size > var.init.size()) {
        return AnyVal();
    }
    std::uint32_t bits = 0;
    for (std::uint32_t i = 0; i < size && !var.init.empty(); i++) {
        bits |= static_cast<std::uint32_t>(var.init[at + i]) << (8 * i);
    }
    return ConstOf(p, index.type, bits);
}

Val UnaryValue(const Pass& p, const Expr& e, Val a) {
    if (a.kind != Val::Const || !ConstType(*p.program, e.type)) {
        return AnyVal();
    }
    return ConstOf(p, e.type, EvalUnaryOp(e.op, KindOf(*p.program, e.type), a.bits));
}

Val ConvertValue(const Pass& p, const Expr& e, Val a) {
    TypeId from = At(p, e.a).type;
    if (a.kind != Val::Const || !ConstType(*p.program, e.type) || !ConstType(*p.program, from)) {
        return AnyVal();
    }
    TypeKind from_kind = KindOf(*p.program, from);
    return ConstOf(p, e.type, ConvertBits(from_kind, KindOf(*p.program, e.type), a.bits));
}

Val BinaryValue(const Pass& p, const Expr& e, Val a, Val b) {
    TypeId operand = At(p, e.a).type;
    if (!ConstType(*p.program, e.type) || !ConstType(*p.program, operand)) {
        return AnyVal();
    }
    TypeKind kind = KindOf(*p.program, operand);
    if (a.kind == Val::Const && b.kind == Val::Const) {
        bool unused = false;
        return ConstOf(p, e.type, EvalBinaryOp(e.op, kind, a.bits, b.bits, unused));
    }
    // x * 0, x & 0, x | all ones, x << 8 for a byte, whatever x is
    std::uint16_t ones = MaskToKind(kind, 0xFFFF);
    bool zero = (a.kind == Val::Const && a.bits == 0) || (b.kind == Val::Const && b.bits == 0);
    bool full =
        (a.kind == Val::Const && a.bits == ones) || (b.kind == Val::Const && b.bits == ones);
    if ((e.op == Op::Mul || e.op == Op::And) && zero) {
        return ConstOf(p, e.type, 0);
    }
    if (e.op == Op::Or && full) {
        return ConstOf(p, e.type, ones);
    }
    Val same;
    if (SameOperands(p, e, same) || WithComplement(p, e, same)) {
        return same;
    }
    // shifted out entirely (a signed >> keeps the sign)
    bool out = b.kind == Val::Const && b.bits >= KindBits(kind);
    if (out && (e.op == Op::Shl || (e.op == Op::Shr && !IsSignedKind(kind)))) {
        return ConstOf(p, e.type, 0);
    }
    return AnyVal();
}

} // namespace tisple::opt
