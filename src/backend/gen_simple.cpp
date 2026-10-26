#include "backend/gen.hpp"

namespace tisple {

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

std::uint32_t SizeOf(const Gen& g, TypeId type) {
    return TypeOf(*g.program, type).size;
}

bool IsScalarType(const Gen& g, TypeId type) {
    return IsScalarKind(KindOf(*g.program, type));
}

} // namespace

namespace {

bool IsImmByte(const ByteRef& b, int value) {
    return b.kind == ByteKind::Imm && b.value.sym < 0 && (b.value.offset & 0xFF) == value;
}

// both operands' index needs as one (they may share one index, or only one has any)
bool MergeIndex(const Opnd& a, const Opnd& b, Index& out) {
    if (!a.index.used) {
        out = b.index;
        return true;
    }
    if (!b.index.used) {
        out = a.index;
        return true;
    }
    out = a.index;
    return a.index.is_reg == b.index.is_reg && a.index.reg == b.index.reg &&
           a.index.value.sym == b.index.value.sym && a.index.value.offset == b.index.value.offset &&
           a.index.delta == b.index.delta;
}

// 16-bit a op b with no code: every byte comes from one side because the other is a known
// 0 (or $FF for and), so no carry ever moves between bytes
Opnd SelectBytes(Gen& g, const Expr& e) {
    if (e.op != Op::Or && e.op != Op::Xor && e.op != Op::Add && e.op != Op::And) {
        return Opnd();
    }
    Opnd a = TrySimple(g, e.a);
    Opnd b = a.size ? TrySimple(g, e.b) : Opnd();
    Opnd out;
    if (b.size == 0 || !MergeIndex(a, b, out.index)) {
        return Opnd();
    }
    out.size = 2;
    out.moved = a.moved || b.moved;
    for (int k = 0; k < 2; k++) {
        if (e.op == Op::And) {
            if (IsImmByte(a.b[k], 0) || IsImmByte(b.b[k], 0)) {
                out.b[k] = ByteRef();
                out.b[k].value = Num(0);
            } else if (IsImmByte(b.b[k], 0xFF)) {
                out.b[k] = a.b[k];
            } else if (IsImmByte(a.b[k], 0xFF)) {
                out.b[k] = b.b[k];
            } else {
                return Opnd();
            }
            continue;
        }
        if (IsImmByte(a.b[k], 0)) {
            out.b[k] = b.b[k];
        } else if (IsImmByte(b.b[k], 0)) {
            out.b[k] = a.b[k];
        } else {
            return Opnd();
        }
    }
    return out;
}

// (>> x 8) and (<< x 8) of a 16-bit operand just move a byte
Opnd ByteShift(Gen& g, const Expr& e) {
    std::uint16_t c = 0;
    if ((e.op != Op::Shl && e.op != Op::Shr) || !ConstValue(*g.program, e.b, c) || c != 8 ||
        (e.op == Op::Shr && IsSignedKind(KindOf(*g.program, e.type)))) {
        return Opnd();
    }
    Opnd a = TrySimple(g, e.a);
    if (a.size != 2) {
        return Opnd();
    }
    Opnd out = a;
    out.moved = true;
    ByteRef zero;
    zero.value = Num(0);
    if (e.op == Op::Shr) {
        out.b[0] = a.b[1];
        out.b[1] = zero;
    } else {
        out.b[1] = a.b[0];
        out.b[0] = zero;
    }
    return out;
}

} // namespace

Opnd TrySimple(Gen& g, ExprId id) {
    id = ForwardTarget(g, id);
    for (const ReducedByte& r : g.reduced_bytes) {
        if (r.expr == id) {
            return r.temp;
        }
    }
    const Expr& e = At(g, id);
    std::uint16_t c = 0;
    if (IsScalarKind(KindOf(*g.program, e.type)) && ConstValue(*g.program, id, c)) {
        return ImmOpnd(c, SizeOf(g, e.type));
    }
    switch (e.kind) {
    case ExprKind::VarRef: {
        if (g.wide && g.wide->var == e.ref && g.pin[1] == e.ref) {
            Opnd o;
            o.size = 2;
            o.b[0].kind = ByteKind::Reg;
            o.b[0].reg = 1;
            o.b[1].kind = ByteKind::Mem;
            o.b[1].value = g.wide->high;
            return o;
        }
        for (int r = 0; r < 2; r++) {
            if (g.pin[r] == e.ref) {
                Opnd o;
                o.size = 1;
                o.b[0].kind = ByteKind::Reg;
                o.b[0].reg = static_cast<std::uint8_t>(r);
                return o;
            }
        }
        break;
    }
    case ExprKind::Convert:
    case ExprKind::Reinterpret: {
        TypeKind from = ExprKindOf(*g.program, e.a);
        std::uint32_t from_size = ExprSize(*g.program, e.a);
        std::uint32_t to_size = SizeOf(g, e.type);
        if (from_size == 2 || !IsSignedKind(from) || to_size == 1) {
            Opnd o = TrySimple(g, e.a);
            if (o.size == 0) {
                return o;
            }
            if (to_size == 2 && from_size == 1) {
                o.size = 2;
                o.b[1] = ByteRef();
                o.b[1].value = Num(0);
            }
            o.size = static_cast<std::uint8_t>(to_size);
            return o;
        }
        return Opnd();
    }
    case ExprKind::Binary: {
        if (SizeOf(g, e.type) != 2) {
            return Opnd();
        }
        Opnd shifted = ByteShift(g, e);
        return shifted.size != 0 ? shifted : SelectBytes(g, e);
    }
    case ExprKind::AddrOf: {
        PlaceRef place;
        if (!ResolveForRead(g, e.a, place) || place.has_index || place.pointer) {
            return Opnd();
        }
        AsmValue address = place.base;
        address.offset =
            static_cast<std::int32_t>((address.offset + place.cursor.address) & 0xFFFF);
        return SymImmOpnd(address, 2);
    }
    default:
        break;
    }
    if (e.kind != ExprKind::VarRef && e.kind != ExprKind::Field && e.kind != ExprKind::Index &&
        e.kind != ExprKind::Load && e.kind != ExprKind::LoadField) {
        return Opnd();
    }
    if (!IsScalarType(g, e.type)) {
        return Opnd();
    }
    PlaceRef place;
    if (!ResolveForRead(g, id, place)) {
        return Opnd();
    }
    if (!IsNormalPlace(g, place, e.type) && !UseLoopPointer(g, place, e.type)) {
        return Opnd();
    }
    return PlaceOpnd(g, place, e.type);
}

Opnd AddressOf(Gen& g, ExprId id, const Opnd* dst) {
    PlaceRef place;
    if (!ResolvePlace(g, id, -1, place)) {
        return ImmOpnd(0, 2);
    }
    if (place.pointer || place.has_index) {
        PlaceToPointer(g, place);
        Opnd out = MemOpnd(place.ptr, 2, 1);
        out.temp = true;
        if (dst) {
            CopyOpnd(g, out, *dst);
            return *dst;
        }
        return out;
    }
    AsmValue address = place.base;
    address.offset = static_cast<std::int32_t>((address.offset + place.cursor.address) & 0xFFFF);
    return SymImmOpnd(address, 2);
}

} // namespace tisple
