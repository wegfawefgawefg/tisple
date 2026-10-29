#include "backend/gen.hpp"

// (load (+ (addr_of (at a i)) k)) on an array of bytes, with i + k always below 256: the
// element at i + k of a itself, the sum worked out in a byte and the access a,X, instead of a
// zero-page pointer made from a's address and i and then (p),Y with k. The race's peek and
// poke helpers do this once inlined: (load (+ p (as u16 (& k 3)))) with p bound to an
// element's address.

namespace tisple {

namespace {

// the array variable of bytes a place (at a i) indexes, and its index; -1 if it is not one
std::int32_t ByteArrayOf(const Gen& g, ExprId place, ExprId& index) {
    const Program& program = *g.program;
    const Expr& at = ExprOf(program, place);
    if (at.kind != ExprKind::Index) {
        return -1;
    }
    const Expr& array = ExprOf(program, BoundTarget(g, at.a));
    if (array.kind != ExprKind::VarRef || array.ref == g.pin[0] || array.ref == g.pin[1]) {
        return -1;
    }
    const Type& type = TypeOf(program, array.type);
    if (type.kind != TypeKind::Array || TypeOf(program, type.elem).size != 1 ||
        !IsScalarKind(KindOf(program, type.elem))) {
        return -1;
    }
    index = at.b;
    return array.ref;
}

// the low byte of a value no larger than 255 into A
void ByteIntoA(Gen& g, ExprId id) {
    const Expr& e = ExprOf(*g.program, id);
    if (ExprSize(*g.program, id) == 1) {
        EvalA(g, id);
        return;
    }
    if (e.kind == ExprKind::Convert && ExprSize(*g.program, e.a) == 1) {
        EvalA(g, e.a);
        return;
    }
    LoadA(g, Eval(g, id, nullptr), 0);
}

} // namespace

bool ArrayPlusByte(Gen& g, ExprId pointer, TypeId type, PlaceRef& out) {
    const Program& program = *g.program;
    const Expr& sum = ExprOf(program, pointer);
    if (sum.kind != ExprKind::PtrAdd || sum.value != 1 || TypeOf(program, type).size != 1) {
        return false;
    }
    const Expr& address = ExprOf(program, BoundTarget(g, sum.a));
    if (address.kind != ExprKind::AddrOf) {
        return false;
    }
    ExprId index = -1;
    std::int32_t var = ByteArrayOf(g, address.a, index);
    if (var < 0 || HasEffects(program, index) || HasEffects(program, sum.b) ||
        MaxOf(g, index) + MaxOf(g, sum.b) > 0xFF) {
        return false;
    }
    ByteIntoA(g, index);
    Opnd k = TrySimple(g, sum.b);
    bool byte = k.size != 0 && !k.in_a;
    if (!byte) {
        Opnd i = SpillA(g);
        ByteIntoA(g, sum.b);
        k = i;
    }
    Emit(g, Mn::CLC, Mode::Imp);
    OpA(g, Mn::ADC, k, 0);
    Opnd t = NewTemp(g, 1);
    StoreA(g, t, 0);
    out = PlaceRef();
    out.base = SymValue(g.be->var_sym[static_cast<std::size_t>(var)]);
    out.root = var;
    Cursor whole = VarCursor(g.be->shapes, var);
    whole.address = 0;
    TypeId array = program.vars[static_cast<std::size_t>(var)].type;
    out.cursor = ElementCursor(program, g.be->shapes, var, whole, array, 0);
    out.has_index = true;
    out.index = t;
    out.mult = 1;
    out.type = type;
    return true;
}

} // namespace tisple
