#include "backend/gen.hpp"

#include <algorithm>

// Byte indices: (+ p (as u16 (- x 1))) reads through (p),Y with Y = x - 1, which is LDY x /
// DEY, or nothing at all when the loop over x keeps x in Y and p - 1 in a loop pointer.

namespace tisple {

namespace {

// alias chains and nested + and - stop here
constexpr int kMaxIndexDepth = 8;
constexpr int kMaxIndexSteps = 2;

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

bool IsImmZero(const ByteRef& b) {
    return b.kind == ByteKind::Imm && b.value.sym < 0 && (b.value.offset & 0xFF) == 0;
}

// a byte operand an index register can be loaded from, or a number
bool IsIndexBase(const Opnd& o) {
    if (o.in_a || o.index.used || o.moved) {
        return false;
    }
    ByteKind kind = o.b[0].kind;
    return kind == ByteKind::Mem || kind == ByteKind::Reg || kind == ByteKind::Imm;
}

std::int32_t CounterOf(const Gen& g, ExprId id, const Opnd& base) {
    if (base.b[0].kind == ByteKind::Reg) {
        return g.pin[base.b[0].reg];
    }
    const Expr& e = At(g, id);
    if (e.kind == ExprKind::VarRef && g.program->vars[static_cast<std::size_t>(e.ref)].loop_var) {
        return e.ref;
    }
    return -1;
}

int Signed(std::uint8_t delta) {
    return delta < 0x80 ? delta : delta - 0x100;
}

bool ByteIndexAt(Gen& g, ExprId id, int depth, ByteIndex& out) {
    if (depth > kMaxIndexDepth) {
        return false;
    }
    id = AliasTarget(g, id);
    const Expr& e = At(g, id);
    std::uint32_t size = ExprSize(*g.program, id);
    TypeKind kind = ExprKindOf(*g.program, id);
    std::uint16_t c = 0;
    if (IsScalarKind(kind) && ConstValue(*g.program, id, c)) {
        if (size == 2 && c > 0xFF) {
            return false;
        }
        out = ByteIndex();
        out.base = ImmOpnd(c & 0xFF, 1);
        return true;
    }
    if (size == 2) {
        if (e.kind == ExprKind::Convert && ExprKindOf(*g.program, e.a) == TypeKind::U8) {
            return ByteIndexAt(g, e.a, depth + 1, out);
        }
        // a u16 operand whose high byte is a known zero
        Opnd o = TrySimple(g, id);
        if (o.size != 2 || !IsImmZero(o.b[1]) || !IsIndexBase(o)) {
            return false;
        }
        o.size = 1;
        out = ByteIndex();
        out.base = o;
        out.counter = CounterOf(g, id, o);
        return true;
    }
    if (size != 1 || (kind != TypeKind::U8 && kind != TypeKind::I8)) {
        return false;
    }
    if ((e.kind == ExprKind::Convert || e.kind == ExprKind::Reinterpret) &&
        ExprSize(*g.program, e.a) == 1) {
        return ByteIndexAt(g, e.a, depth + 1, out);
    }
    if (e.kind == ExprKind::Binary && (e.op == Op::Add || e.op == Op::Sub)) {
        ExprId rest = -1;
        if (e.op == Op::Add && ConstValue(*g.program, e.a, c)) {
            rest = e.b;
        } else if (ConstValue(*g.program, e.b, c)) {
            rest = e.a;
            c = e.op == Op::Sub ? static_cast<std::uint16_t>(-c) : c;
        }
        if (rest < 0 || !ByteIndexAt(g, rest, depth + 1, out)) {
            return false;
        }
        out.delta = static_cast<std::uint8_t>(out.delta + c);
        return true;
    }
    Opnd o = TrySimple(g, id);
    if (o.size != 1 || !IsIndexBase(o)) {
        return false;
    }
    out = ByteIndex();
    out.base = o;
    out.counter = CounterOf(g, id, o);
    return true;
}

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

// base + wide counter as (derived),Y
bool UseWideDerived(Gen& g, PlaceRef& place, TypeId type) {
    if (!place.has_index || place.mult != 1 || !IsWideCounter(g, place.index) ||
        !IsScalarKind(KindOf(*g.program, type))) {
        return false;
    }
    std::uint32_t size = TypeOf(*g.program, type).size;
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
    std::int32_t loop = g.wide->var;
    place.ptr = DerivedPointer(g, loop, base, var, offset);
    place.ptr2 = AsmValue();
    if (size == 2) {
        place.ptr2 = DerivedPointer(g, loop, base, var, offset + stride);
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

// p + (counter + delta) through a pointer p the loop never changes, with the u8 counter in
// Y: (p + delta),Y, since the sum never wraps
bool UseCounterDerived(Gen& g, PlaceRef& place, TypeId type) {
    std::int32_t loop = g.pin[1];
    if (loop < 0 || (g.wide && g.wide->var == loop) || !place.pointer || !place.has_index ||
        place.mult != 1 || place.ptr_var < 0) {
        return false;
    }
    const ByteRef& b = place.index.b[0];
    if (place.index.size != 1 || b.kind != ByteKind::Reg || b.reg != 1) {
        return false;
    }
    if (!IsScalarKind(KindOf(*g.program, type)) || TypeOf(*g.program, type).size != 1) {
        return false;
    }
    if (place.delta == 0 && place.cursor.address == 0) {
        return false;
    }
    if (!DeltaFits(g, loop, place.delta) || MayWriteVarIn(g, g.pin_body[1], place.ptr_var)) {
        return false;
    }
    std::int32_t offset = Signed(place.delta) + static_cast<std::int32_t>(place.cursor.address);
    AsmValue base = SymValue(g.be->var_sym[static_cast<std::size_t>(place.ptr_var)]);
    place.ptr = DerivedPointer(g, loop, base, place.ptr_var, offset);
    place.ptr2 = AsmValue();
    place.cursor.address = 0;
    place.delta = 0;
    return true;
}

} // namespace

ExprId AliasTarget(const Gen& g, ExprId id) {
    for (int depth = 0; depth < kMaxIndexDepth; depth++) {
        const Expr& e = At(g, id);
        if (e.kind != ExprKind::VarRef) {
            return id;
        }
        ExprId value = g.be->aliases[static_cast<std::size_t>(e.ref)].value;
        if (value < 0) {
            return id;
        }
        id = value;
    }
    return id;
}

bool ByteIndexOf(Gen& g, ExprId id, ByteIndex& out) {
    return ByteIndexAt(g, id, 0, out);
}

bool IsSmallDelta(std::uint8_t delta) {
    int d = Signed(delta);
    return d >= -kMaxIndexSteps && d <= kMaxIndexSteps;
}

bool DeltaFits(const Gen& g, std::int32_t counter, std::uint8_t delta) {
    if (counter < 0) {
        return false;
    }
    for (const CounterRange& r : g.ranges) {
        if (r.var == counter) {
            int d = Signed(delta);
            return r.lo + d >= 0 && r.hi + d <= 0xFF;
        }
    }
    return false;
}

bool UseLoopPointer(Gen& g, PlaceRef& place, TypeId type) {
    return UseWideDerived(g, place, type) || UseCounterDerived(g, place, type);
}

AsmValue DerivedPointer(Gen& g, std::int32_t loop, AsmValue base, std::int32_t var,
                        std::int32_t offset) {
    for (const Derived& d : g.derived) {
        bool same = d.loop == loop && d.var == var && d.offset == offset &&
                    d.base.sym == base.sym && d.base.offset == base.offset &&
                    d.base.part == base.part;
        if (same) {
            return d.ptr;
        }
    }
    Derived d;
    d.loop = loop;
    d.base = base;
    d.var = var;
    d.offset = offset;
    std::uint32_t at = g.lptr_top;
    g.lptr_top += 2;
    std::uint32_t& max = g.be->lptr_size[static_cast<std::size_t>(g.function)];
    max = g.lptr_top > max ? g.lptr_top : max;
    d.ptr = SymValue(g.be->lptr_sym[static_cast<std::size_t>(g.function)],
                     static_cast<std::int32_t>(at));
    g.derived.push_back(d);
    return d.ptr;
}

std::vector<Derived> TakeDerived(Gen& g, std::int32_t loop, std::size_t first) {
    std::vector<Derived> used;
    std::vector<Derived> rest;
    for (const Derived& d : g.derived) {
        if (d.loop != loop) {
            rest.push_back(d);
            continue;
        }
        // one only a discarded trial or an unused operand asked for is not kept up
        bool referenced = false;
        for (std::size_t i = first; i < g.out->size() && !referenced; i++) {
            const AsmItem& item = (*g.out)[i];
            referenced = item.kind == ItemKind::Ins && item.value.sym == d.ptr.sym &&
                         item.value.offset == d.ptr.offset;
        }
        if (referenced) {
            used.push_back(d);
        }
    }
    g.derived = rest;
    return used;
}

namespace {

// T = base + offset (+ high << 8): only A and the carry
void SetDerived(Gen& g, const Derived& d, const AsmValue& high) {
    Opnd t = MemOpnd(d.ptr, 2, 1);
    Opnd base;
    if (d.var >= 0) {
        base = MemOpnd(d.base, 2, 1);
    } else {
        AsmValue address = d.base;
        address.offset = (address.offset + d.offset) & 0xFFFF;
        base = d.base.sym >= 0 ? SymImmOpnd(address, 2)
                               : ImmOpnd(static_cast<std::uint16_t>(address.offset), 2);
    }
    if (d.var >= 0 && d.offset != 0) {
        Combine16(g, Op::Add, base, ImmOpnd(static_cast<std::uint16_t>(d.offset & 0xFFFF), 2), t);
        base = t;
    }
    if (high.sym < 0) {
        if (!(d.var >= 0 && d.offset != 0)) {
            CopyOpnd(g, base, t);
        }
        return;
    }
    LoadA(g, base, 0);
    StoreA(g, t, 0);
    LoadA(g, base, 1);
    Emit(g, Mn::CLC, Mode::Imp);
    Emit(g, Mn::ADC, Mode::Abs, high);
    StoreA(g, t, 1);
}

} // namespace

void InitDerived(Gen& g, const std::vector<Derived>& derived, std::size_t at, AsmValue high) {
    std::size_t before = g.out->size();
    // runs once, before the loop
    g.loop_depth--;
    for (const Derived& d : derived) {
        SetDerived(g, d, high);
    }
    g.loop_depth++;
    std::rotate(g.out->begin() + static_cast<std::ptrdiff_t>(at),
                g.out->begin() + static_cast<std::ptrdiff_t>(before), g.out->end());
}

} // namespace tisple
