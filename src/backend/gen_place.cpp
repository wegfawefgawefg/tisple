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

// an index operand whose high byte is known zero reads as a u8
bool IsByteIndex(const Opnd& o) {
    if (o.size == 1) {
        return true;
    }
    return o.b[1].kind == ByteKind::Imm && o.b[1].value.sym < 0 && o.b[1].value.offset == 0;
}

// byte 0 of the index as something an index register can be loaded from
bool IndexFrom(const Opnd& o, Index& out) {
    const ByteRef& b = o.b[0];
    out = Index();
    out.used = true;
    if (b.kind == ByteKind::Reg) {
        out.is_reg = true;
        out.reg = b.reg;
        return true;
    }
    if (b.kind == ByteKind::Mem) {
        out.value = b.value;
        return true;
    }
    return false;
}

// a 16-bit view of an index operand
Opnd Widen(const Opnd& o) {
    if (o.size == 2) {
        return o;
    }
    Opnd w = o;
    w.size = 2;
    w.b[1] = ByteRef();
    w.b[1].value = Num(0);
    return w;
}

// the cursor offset of the farthest byte of a value of this type at the place
std::uint32_t FarthestByte(const Gen& g, const PlaceRef& place, TypeId type) {
    const Type& t = TypeOf(*g.program, type);
    if (IsScalarKind(t.kind)) {
        return place.cursor.address + (t.size - 1) * place.cursor.stride;
    }
    std::uint32_t far = place.cursor.address;
    if (t.kind == TypeKind::Struct) {
        for (const Field& f : g.program->structs[static_cast<std::size_t>(t.decl)].fields) {
            std::uint32_t at = FarthestByte(g, FieldPlace(g, place, f.offset), f.type);
            far = at > far ? at : far;
        }
        return far;
    }
    for (std::uint32_t i = 0; i < t.count; i++) {
        std::uint32_t at = FarthestByte(g, ElementPlace(g, place, type, i), t.elem);
        far = at > far ? at : far;
    }
    return far;
}

// can every byte be reached as it is
bool IsNormal(const Gen& g, const PlaceRef& place, TypeId type) {
    bool scalar_byte = IsScalarType(g, type) && SizeOf(g, type) == 1;
    if (!place.pointer) {
        return !place.has_index || (IsByteIndex(place.index) && place.mult == 1);
    }
    if (place.has_index) {
        // (p),Y with Y the index: a single byte right at the pointer
        bool scalar_word = IsScalarType(g, type) && SizeOf(g, type) == 2 && place.ptr2.sym >= 0;
        return IsByteIndex(place.index) && place.mult == 1 && place.cursor.address == 0 &&
               (scalar_byte || scalar_word) &&
               !(place.index.b[0].kind == ByteKind::Reg && place.index.b[0].reg == 0);
    }
    return FarthestByte(g, place, type) < 0x100 || place.ptr2.sym >= 0;
}

// the place's address so far (base, offset, index) into a fresh zero-page pointer
void ToPointer(Gen& g, PlaceRef& place) {
    Opnd base;
    std::uint32_t offset = place.cursor.address;
    if (place.pointer) {
        base = MemOpnd(place.ptr, 2, 1);
    } else {
        AsmValue address = place.base;
        address.offset = static_cast<std::int32_t>((address.offset + offset) & 0xFFFF);
        base = SymImmOpnd(address, 2);
        offset = 0;
    }
    Opnd dst = MemOpnd(NewPtrTemp(g), 2, 1);
    dst.temp = true;
    if (place.has_index) {
        Opnd index = Widen(place.index);
        if (place.mult != 1) {
            Opnd scaled = NewTemp(g, 2);
            MulConst16(g, index, place.mult, scaled);
            index = scaled;
        }
        if (offset != 0) {
            Combine16(g, Op::Add, base, ImmOpnd(static_cast<std::uint16_t>(offset), 2), dst);
            base = dst;
        }
        Combine16(g, Op::Add, base, index, dst);
    } else if (offset != 0) {
        Combine16(g, Op::Add, base, ImmOpnd(static_cast<std::uint16_t>(offset), 2), dst);
    } else {
        CopyOpnd(g, base, dst);
    }
    place.pointer = true;
    place.ptr = dst.b[0].value;
    place.cursor.address = 0;
    place.has_index = false;
    place.index = Opnd();
    place.mult = 0;
}

// the pointer a load or store goes through, in zero page
bool PointerBase(Gen& g, ExprId pointer, ExprId later, bool emit, PlaceRef& out) {
    const Expr& pe = At(g, pointer);
    if (pe.kind == ExprKind::VarRef) {
        out.ptr_var = pe.ref;
    }
    std::uint16_t c = 0;
    if (ConstValue(*g.program, pointer, c)) {
        out.ptr_const = true;
        out.ptr_value = c;
    }
    if (pe.kind == ExprKind::VarRef && PointerInZp(*g.be, pe.ref) &&
        !MayWriteVarIn(g, later, pe.ref)) {
        out.ptr = SymValue(g.be->var_sym[static_cast<std::size_t>(pe.ref)]);
        return true;
    }
    if (!emit) {
        return false;
    }
    Opnd dst = MemOpnd(NewPtrTemp(g), 2, 1);
    dst.temp = true;
    EvalInto(g, pointer, dst);
    out.ptr = dst.b[0].value;
    return true;
}

// an index value as an operand byte 0 can index with (memory or a register)
bool IndexOperand(Gen& g, ExprId id, ExprId later, bool emit, Opnd& out) {
    if (!emit) {
        out = TrySimple(g, id);
        if (out.size == 0 || IsImm(out)) {
            return false;
        }
        Index index;
        return IndexFrom(out, index);
    }
    out = Eval(g, id, nullptr);
    Index index;
    bool usable =
        !out.in_a && IndexFrom(out, index) &&
        (out.size == 1 || out.b[1].kind == ByteKind::Imm || out.b[1].kind == ByteKind::Mem);
    if (usable && !MayWriteOpnd(g, later, out)) {
        return true;
    }
    // into a temporary; a known-zero high byte stays known
    Opnd t = NewTemp(g, out.size);
    for (int k = 0; k < out.size; k++) {
        if (k == 1 && out.b[1].kind == ByteKind::Imm) {
            t.b[1] = out.b[1];
            continue;
        }
        LoadA(g, out, k);
        StoreA(g, t, k);
    }
    out = t;
    return true;
}

// what a pointer points at, plus a constant offset; (load (+ p i)) keeps i as an index
bool ResolvePointer(Gen& g, ExprId pointer, bool plain, std::uint32_t offset, TypeId type,
                    ExprId later, bool emit, PlaceRef& out) {
    out = PlaceRef();
    out.pointer = true;
    out.cursor = Cursor();
    out.cursor.address = static_cast<std::uint16_t>(offset);
    out.cursor.split = false;
    out.cursor.root = false;
    out.type = type;
    const Expr& pe = At(g, pointer);
    std::uint16_t c = 0;
    if (plain && pe.kind == ExprKind::PtrAdd && !ConstValue(*g.program, pe.b, c)) {
        if (!PointerBase(g, pe.a, later, emit, out)) {
            return false;
        }
        Opnd index;
        if (!IndexOperand(g, pe.b, later, emit, index)) {
            return false;
        }
        out.has_index = true;
        out.index = index;
        out.mult = pe.value;
        return true;
    }
    return PointerBase(g, pointer, later, emit, out);
}

bool Resolve(Gen& g, ExprId id, ExprId later, bool emit, PlaceRef& out) {
    const Expr& e = At(g, id);
    switch (e.kind) {
    case ExprKind::VarRef: {
        if (e.ref == g.pin[0] || e.ref == g.pin[1]) {
            return false;
        }
        out = PlaceRef();
        out.base = SymValue(g.be->var_sym[static_cast<std::size_t>(e.ref)]);
        out.root = e.ref;
        out.cursor = VarCursor(g.be->shapes, e.ref);
        out.cursor.address = 0;
        out.type = e.type;
        return true;
    }
    case ExprKind::Field:
        if (!Resolve(g, e.a, later, emit, out)) {
            return false;
        }
        out.cursor = FieldCursor(out.cursor, e.value);
        out.type = e.type;
        return true;
    case ExprKind::Index: {
        if (!Resolve(g, e.a, later, emit, out)) {
            return false;
        }
        TypeId array = At(g, e.a).type;
        std::uint16_t c = 0;
        if (ConstValue(*g.program, e.b, c)) {
            out.cursor = ElementCursor(*g.program, g.be->shapes, out.root, out.cursor, array, c);
            out.type = e.type;
            return true;
        }
        if (out.has_index) {
            if (!emit) {
                return false;
            }
            ToPointer(g, out);
        }
        Cursor c0 = ElementCursor(*g.program, g.be->shapes, out.root, out.cursor, array, 0);
        Cursor c1 = ElementCursor(*g.program, g.be->shapes, out.root, out.cursor, array, 1);
        Opnd index;
        if (!IndexOperand(g, e.b, later, emit, index)) {
            return false;
        }
        out.cursor = c0;
        out.has_index = true;
        out.index = index;
        out.mult = (c1.address - c0.address) & 0xFFFFu;
        out.type = e.type;
        return true;
    }
    case ExprKind::Load:
    case ExprKind::LoadField:
        return ResolvePointer(g, e.a, e.kind == ExprKind::Load,
                              e.kind == ExprKind::LoadField ? e.value : 0, e.type, later, emit,
                              out);
    default:
        if (emit) {
            Unsupported(g, id, "this kind of place");
        }
        return false;
    }
}

} // namespace

bool ResolvePlace(Gen& g, ExprId id, ExprId later, PlaceRef& out) {
    return Resolve(g, id, later, true, out);
}

bool ResolveStore(Gen& g, const Expr& store, ExprId later, PlaceRef& out) {
    bool field = store.kind == ExprKind::StoreField;
    TypeId type = field ? store.ref : TypeOf(*g.program, ExprOf(*g.program, store.a).type).elem;
    return ResolvePointer(g, store.a, !field, field ? store.value : 0, type, later, true, out);
}

namespace {

// the index is the wide loop counter: low byte in Y, high byte in memory
bool IsWideCounter(const Gen& g, const Opnd& index) {
    if (!g.wide || index.size != 2) {
        return false;
    }
    const ByteRef& low = index.b[0];
    const ByteRef& high = index.b[1];
    return low.kind == ByteKind::Reg && low.reg == 1 && high.kind == ByteKind::Mem &&
           high.value.sym == g.wide->high.sym && high.value.offset == g.wide->high.offset;
}

// base + counter as (derived),Y: no address arithmetic in the loop at all
bool UseDerived(Gen& g, PlaceRef& place, TypeId type) {
    if (!place.has_index || place.mult != 1 || !IsWideCounter(g, place.index) ||
        !IsScalarType(g, type)) {
        return false;
    }
    std::uint32_t size = SizeOf(g, type);
    std::int32_t offset = static_cast<std::int32_t>(place.cursor.address);
    AsmValue base;
    std::int32_t var = -1;
    if (!place.pointer) {
        base = place.base;
    } else if (place.ptr_const) {
        base = Num(place.ptr_value);
    } else if (place.ptr_var >= 0 && !MayWriteVarIn(g, g.wide->body, place.ptr_var)) {
        var = place.ptr_var;
        base = SymValue(g.be->var_sym[static_cast<std::size_t>(var)]);
    } else {
        return false;
    }
    std::int32_t stride = static_cast<std::int32_t>(place.cursor.stride);
    place.ptr = DerivedPointer(g, base, var, offset);
    place.ptr2 = AsmValue();
    if (size == 2) {
        place.ptr2 = DerivedPointer(g, base, var, offset + stride);
    }
    place.pointer = true;
    place.cursor.address = 0;
    Opnd y;
    y.size = 1;
    y.b[0].kind = ByteKind::Reg;
    y.b[0].reg = 1;
    place.index = y;
    return true;
}

} // namespace

void NormalizePlace(Gen& g, PlaceRef& place, TypeId type) {
    if (IsNormal(g, place, type) || UseDerived(g, place, type)) {
        return;
    }
    // the whole address so far into a zero-page pointer
    ToPointer(g, place);
    if (IsNormal(g, place, type)) {
        return;
    }
    if (IsScalarType(g, type) && SizeOf(g, type) == 2) {
        // byte 1 is a plane away: give it a pointer of its own
        Opnd dst = MemOpnd(NewPtrTemp(g), 2, 1);
        dst.temp = true;
        Combine16(g, Op::Add, MemOpnd(place.ptr, 2, 1),
                  ImmOpnd(static_cast<std::uint16_t>(place.cursor.stride), 2), dst);
        place.ptr2 = dst.b[0].value;
        return;
    }
    Fail(g, Loc(),
         "the 6502 backend does not support struct values spread over more than "
         "256 bytes yet");
}

Opnd PlaceOpnd(Gen& g, const PlaceRef& place, TypeId type) {
    Opnd o;
    o.size = static_cast<std::uint8_t>(TypeOf(*g.program, type).size);
    for (int k = 0; k < o.size; k++) {
        ByteRef& b = o.b[k];
        std::uint32_t at =
            place.cursor.address + static_cast<std::uint32_t>(k) * place.cursor.stride;
        if (!place.pointer) {
            b.kind = place.has_index ? ByteKind::MemIdx : ByteKind::Mem;
            b.value = place.base;
            b.value.offset = static_cast<std::int32_t>((place.base.offset + at) & 0xFFFF);
            continue;
        }
        b.kind = ByteKind::IndY;
        if (place.has_index) {
            b.value = k == 1 && place.ptr2.sym >= 0 ? place.ptr2 : place.ptr;
            b.y_index = true;
            continue;
        }
        if (k == 1 && place.ptr2.sym >= 0) {
            b.value = place.ptr2;
            b.y = static_cast<std::uint8_t>(place.cursor.address);
            continue;
        }
        b.value = place.ptr;
        b.y = static_cast<std::uint8_t>(at);
    }
    if (place.has_index) {
        IndexFrom(place.index, o.index);
    }
    return o;
}

PlaceRef FieldPlace(const Gen& g, const PlaceRef& place, std::uint32_t offset) {
    (void)g;
    PlaceRef out = place;
    out.cursor = FieldCursor(place.cursor, offset);
    return out;
}

PlaceRef ElementPlace(const Gen& g, const PlaceRef& place, TypeId array_type, std::uint32_t i) {
    PlaceRef out = place;
    out.cursor = ElementCursor(*g.program, g.be->shapes, place.root, place.cursor, array_type, i);
    return out;
}

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
           a.index.value.sym == b.index.value.sym && a.index.value.offset == b.index.value.offset;
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
        if (!Resolve(g, e.a, -1, false, place) || place.has_index || place.pointer) {
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
    if (!Resolve(g, id, -1, false, place) || !IsNormal(g, place, e.type)) {
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
        ToPointer(g, place);
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
