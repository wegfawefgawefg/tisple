#include "backend/gen.hpp"

// (if (!= (& v top) 0) (set v (^ (<< v 1) k)) (set v (<< v 1))), a CRC's step: v shifts in
// place once, the carry picks the arm, and both arms read v for (<< v 1).

namespace tisple {

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

// the variable an expression only reads: a plain local or global in memory, not a register
std::int32_t PlainVar(const Gen& g, ExprId id) {
    const Expr& e = At(g, id);
    if (e.kind != ExprKind::VarRef || e.ref == g.pin[0] || e.ref == g.pin[1]) {
        return -1;
    }
    const Var& v = g.program->vars[static_cast<std::size_t>(e.ref)];
    TypeKind kind = KindOf(*g.program, v.type);
    bool scalar = kind == TypeKind::U8 || kind == TypeKind::U16;
    if (!scalar || v.address_taken || v.placement.has_address || BoundTarget(g, id) != id ||
        g.be->aliases[static_cast<std::size_t>(e.ref)].value >= 0) {
        return -1;
    }
    for (std::int32_t r : g.reduced) {
        if (r == e.ref) {
            return -1;
        }
    }
    return e.ref;
}

// (<< var 1)
bool IsShiftOne(const Gen& g, ExprId id, std::int32_t var) {
    const Expr& e = At(g, id);
    std::uint16_t c = 0;
    return e.kind == ExprKind::Binary && e.op == Op::Shl && ConstOf(g, e.b, c) && c == 1 &&
           At(g, e.a).kind == ExprKind::VarRef && At(g, e.a).ref == var;
}

// every (<< var 1) in id into shifts; false if var is read some other way, or a call may
bool CollectShifts(const Gen& g, ExprId id, std::int32_t var, int depth,
                   std::vector<ExprId>& shifts) {
    if (id < 0) {
        return true;
    }
    if (depth > 16) {
        return false;
    }
    const Expr& e = At(g, id);
    if (IsShiftOne(g, id, var)) {
        shifts.push_back(id);
        return true;
    }
    switch (e.kind) {
    case ExprKind::VarRef:
        return e.ref != var;
    case ExprKind::Literal:
        return true;
    case ExprKind::Binary:
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Index:
    case ExprKind::Field:
        return CollectShifts(g, e.a, var, depth + 1, shifts) &&
               CollectShifts(g, e.b, var, depth + 1, shifts);
    default:
        return false;
    }
}

// cond is (!= (& var top) 0) or (= (& var top) 0) for var's top bit; set: true for !=
bool TopBitTest(const Gen& g, ExprId cond, std::int32_t& var, bool& set) {
    const Expr& e = At(g, cond);
    if (e.kind != ExprKind::Binary || (e.op != Op::Ne && e.op != Op::Eq)) {
        return false;
    }
    std::uint16_t c = 0;
    ExprId masked = -1;
    if (ConstOf(g, e.b, c) && c == 0) {
        masked = e.a;
    } else if (ConstOf(g, e.a, c) && c == 0) {
        masked = e.b;
    } else {
        return false;
    }
    const Expr& m = At(g, masked);
    if (m.kind != ExprKind::Binary || m.op != Op::And) {
        return false;
    }
    ExprId other = -1;
    if (ConstOf(g, m.b, c)) {
        other = m.a;
    } else if (ConstOf(g, m.a, c)) {
        other = m.b;
    } else {
        return false;
    }
    var = PlainVar(g, other);
    if (var < 0) {
        return false;
    }
    std::uint32_t top = ExprSize(*g.program, other) == 2 ? 0x8000 : 0x80;
    set = e.op == Op::Ne;
    return c == top;
}

// the arm is (set var value): its value, or -1
ExprId ArmValue(const Gen& g, ExprId arm, std::int32_t var) {
    const Expr& e = At(g, arm);
    if (e.kind != ExprKind::Set) {
        return -1;
    }
    const Expr& place = At(g, e.a);
    return place.kind == ExprKind::VarRef && place.ref == var ? e.b : -1;
}

} // namespace

bool GenShiftedIf(Gen& g, const Expr& e) {
    std::int32_t var = -1;
    bool set = false;
    if (e.c < 0 || !TopBitTest(g, e.a, var, set)) {
        return false;
    }
    ExprId then_value = ArmValue(g, e.b, var);
    ExprId else_value = ArmValue(g, e.c, var);
    std::vector<ExprId> shifts;
    if (then_value < 0 || else_value < 0 || !CollectShifts(g, then_value, var, 0, shifts) ||
        !CollectShifts(g, else_value, var, 0, shifts) || shifts.empty()) {
        return false;
    }
    // shift first: the bit the condition tests falls into the carry, and the arms' (<< var 1)
    // is var itself from then on
    std::uint32_t size =
        TypeOf(*g.program, g.program->vars[static_cast<std::size_t>(var)].type).size;
    Opnd v = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(var)]), size, 1);
    RmwByte(g, Mn::ASL, v, 0);
    if (size == 2) {
        RmwByte(g, Mn::ROL, v, 1);
    }
    std::int32_t other = NewLabel(g);
    std::int32_t end = NewLabel(g);
    Branch(g, set ? Mn::BCC : Mn::BCS, other);
    std::size_t mark = g.reduced_bytes.size();
    for (ExprId s : shifts) {
        ReducedByte r;
        r.expr = s;
        r.temp = v;
        g.reduced_bytes.push_back(r);
    }
    GenStmt(g, e.b);
    Jump(g, end);
    PlaceLabel(g, other);
    GenStmt(g, e.c);
    PlaceLabel(g, end);
    g.reduced_bytes.resize(mark);
    return true;
}

} // namespace tisple
