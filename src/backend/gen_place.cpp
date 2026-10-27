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

// the index register can hold the index: a pinned register as it is, memory plus a few
// INX/DEX or INY/DEY
bool IsIndexReady(const PlaceRef& place) {
    if (place.delta == 0) {
        return true;
    }
    return place.index.b[0].kind == ByteKind::Mem && IsSmallDelta(place.delta);
}

// can every byte be reached as it is
bool IsNormal(const Gen& g, const PlaceRef& place, TypeId type) {
    bool scalar_byte = IsScalarType(g, type) && SizeOf(g, type) == 1;
    if (!place.pointer) {
        return !place.has_index ||
               (IsByteIndex(place.index) && place.mult == 1 && IsIndexReady(place));
    }
    if (place.has_index) {
        // (p),Y with Y the index: a single byte right at the pointer
        bool scalar_word = IsScalarType(g, type) && SizeOf(g, type) == 2 && place.ptr2.sym >= 0;
        return IsByteIndex(place.index) && place.mult == 1 && place.cursor.address == 0 &&
               (scalar_byte || scalar_word) && IsIndexReady(place) &&
               !(place.index.b[0].kind == ByteKind::Reg && place.index.b[0].reg == 0);
    }
    return FarthestByte(g, place, type) < 0x100 || place.ptr2.sym >= 0;
}

// a byte index with its delta added, into a temporary
void AddDelta(Gen& g, PlaceRef& place) {
    if (!place.has_index || place.delta == 0) {
        return;
    }
    Opnd t = NewTemp(g, 1);
    LoadA(g, place.index, 0);
    Emit(g, Mn::CLC, Mode::Imp);
    Emit(g, Mn::ADC, Mode::Imm, Num(place.delta));
    StoreA(g, t, 0);
    place.index = t;
    place.delta = 0;
    place.counter = -1;
}

// a u16 index as x plus a constant: (+ x c), (+ c x), (- x c), nested; the constant comes back
// in `add` (mod 2^16) and the rest is returned. An index stays inside its array, so x + c never
// wraps and the constant can move into the address.
ExprId PeelConstant(const Gen& g, ExprId id, std::uint16_t& add) {
    add = 0;
    for (int depth = 0; depth < 8; depth++) {
        id = ForwardTarget(g, id);
        const Expr& e = At(g, id);
        if (ExprSize(*g.program, id) != 2 || e.kind != ExprKind::Binary ||
            (e.op != Op::Add && e.op != Op::Sub)) {
            return id;
        }
        std::uint16_t c = 0;
        if (ConstValue(*g.program, e.b, c)) {
            add = static_cast<std::uint16_t>(e.op == Op::Add ? add + c : add - c);
            id = e.a;
        } else if (e.op == Op::Add && ConstValue(*g.program, e.a, c)) {
            add = static_cast<std::uint16_t>(add + c);
            id = e.b;
        } else {
            return id;
        }
    }
    return id;
}

// the place's address so far (base, offset, index) into a fresh zero-page pointer, or into
// `into` (2 bytes of plain memory) when given and it holds none of what is read; a step
// pointer when the index is a stepped variable (gen_step.cpp)
void ToPointer(Gen& g, PlaceRef& place, const Opnd* into = nullptr) {
    AddDelta(g, place);
    if (UseStepPointer(g, place)) {
        return;
    }
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
    bool direct =
        into && !(place.has_index && Overlaps(g, place.index, *into)) && !Overlaps(g, base, *into);
    Opnd dst = direct ? *into : MemOpnd(NewPtrTemp(g), 2, 1);
    dst.temp = !direct;
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
    place.counter = -1;
}

// the pointer a load or store goes through, in zero page
bool PointerBase(Gen& g, ExprId pointer, ExprId later, bool emit, PlaceRef& out) {
    pointer = BoundTarget(g, pointer);
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

// the index of a place: an operand byte 0 can index with (memory or a register), plus the
// delta of a byte index
bool IndexOperand(Gen& g, ExprId id, ExprId later, bool emit, PlaceRef& place) {
    id = AliasTarget(g, id);
    place.delta = 0;
    place.counter = -1;
    ByteIndex bi;
    if (ByteIndexOf(g, id, bi) && !IsImm(bi.base) && !MayWriteOpnd(g, later, bi.base)) {
        place.index = bi.base;
        place.delta = bi.delta;
        place.counter = bi.counter;
        return true;
    }
    Opnd& out = place.index;
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
    if (plain && pe.kind == ExprKind::PtrAdd && ConstValue(*g.program, pe.b, c)) {
        // a constant count is an offset: LDY #offset
        if (!PointerBase(g, pe.a, later, emit, out)) {
            return false;
        }
        out.cursor.address = static_cast<std::uint16_t>((offset + c * pe.value) & 0xFFFF);
        return true;
    }
    if (plain && pe.kind == ExprKind::PtrAdd) {
        if (!PointerBase(g, pe.a, later, emit, out)) {
            return false;
        }
        if (!IndexOperand(g, pe.b, later, emit, out)) {
            return false;
        }
        out.has_index = true;
        out.mult = pe.value;
        return true;
    }
    return PointerBase(g, pointer, later, emit, out);
}

bool Resolve(Gen& g, ExprId id, ExprId later, bool emit, PlaceRef& out) {
    const Expr& e = At(g, id);
    switch (e.kind) {
    case ExprKind::VarRef: {
        if (BoundTarget(g, id) != id) {
            return Resolve(g, BoundTarget(g, id), later, emit, out);
        }
        if (e.ref == g.pin[0] || e.ref == g.pin[1] || IsReplacedCounter(g, e.ref)) {
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
        // (at a (+ c x)) with a u16 index is a + c indexed by x
        std::uint16_t add = 0;
        ExprId index = PeelConstant(g, e.b, add);
        if (!IndexOperand(g, index, later, emit, out)) {
            return false;
        }
        out.cursor = c0;
        out.has_index = true;
        out.mult = (c1.address - c0.address) & 0xFFFFu;
        out.cursor.address = static_cast<std::uint16_t>((c0.address + add * out.mult) & 0xFFFF);
        out.type = e.type;
        out.site = id;
        if (out.delta != 0 && DeltaFits(g, out.counter, out.delta)) {
            // (at a (+ i k)) is a + k indexed by i: the sum never wraps
            std::int32_t k = out.delta < 0x80 ? out.delta : out.delta - 0x100;
            out.cursor.address = static_cast<std::uint16_t>(
                (c0.address + k * static_cast<std::int32_t>(out.mult)) & 0xFFFF);
            out.delta = 0;
        }
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

void NormalizePlace(Gen& g, PlaceRef& place, TypeId type) {
    if (IsNormal(g, place, type) || UseLoopPointer(g, place, type) ||
        UsePagePointer(g, place, type)) {
        return;
    }
    if (place.has_index && place.delta != 0) {
        AddDelta(g, place);
        if (IsNormal(g, place, type)) {
            return;
        }
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
        o.index.delta = place.delta;
    }
    return o;
}

void PinPlace(Gen& g, PlaceRef& place, TypeId type) {
    if (SizeOf(g, type) < 2) {
        return;
    }
    if (place.has_index && place.index.b[0].kind == ByteKind::Mem && !place.index.temp) {
        const AsmValue& v = place.index.b[0].value;
        bool hit = place.pointer ? PointerMayReach(g, v) : v.sym == place.base.sym;
        if (hit) {
            Opnd t = NewTemp(g, 1);
            LoadA(g, place.index, 0);
            StoreA(g, t, 0);
            Opnd w = place.index;
            w.b[0] = t.b[0];
            w.temp = place.index.size == 1 || w.b[1].kind == ByteKind::Imm;
            place.index = w;
        }
    }
    if (!place.pointer) {
        return;
    }
    AsmValue* ptrs[2] = {&place.ptr, &place.ptr2};
    for (AsmValue* ptr : ptrs) {
        if (ptr->sym < 0) {
            continue;
        }
        AsmValue high = *ptr;
        high.offset++;
        if (!PointerMayReach(g, *ptr) && !PointerMayReach(g, high)) {
            continue;
        }
        Opnd copy = MemOpnd(NewPtrTemp(g), 2, 1);
        copy.temp = true;
        CopyOpnd(g, MemOpnd(*ptr, 2, 1), copy);
        *ptr = copy.b[0].value;
    }
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

bool ResolveForRead(Gen& g, ExprId id, PlaceRef& out) {
    return Resolve(g, id, -1, false, out);
}

bool IsNormalPlace(const Gen& g, const PlaceRef& place, TypeId type) {
    return IsNormal(g, place, type);
}

void PlaceToPointer(Gen& g, PlaceRef& place, const Opnd* into) {
    ToPointer(g, place, into);
}

} // namespace tisple
