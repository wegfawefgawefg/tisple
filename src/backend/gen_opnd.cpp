#include "backend/gen.hpp"

namespace tisple {

namespace {

bool SameValue(const AsmValue& a, const AsmValue& b) {
    return a.sym == b.sym && a.offset == b.offset && a.part == b.part;
}

} // namespace

Opnd ImmOpnd(std::uint16_t value, std::uint32_t size) {
    Opnd o;
    o.size = static_cast<std::uint8_t>(size);
    o.b[0].value = Num(value & 0xFF);
    o.b[1].value = Num(value >> 8);
    return o;
}

Opnd SymImmOpnd(AsmValue address, std::uint32_t size) {
    Opnd o;
    o.size = static_cast<std::uint8_t>(size);
    o.b[0].value = address;
    o.b[0].value.part = Part::Low;
    o.b[1].value = address;
    o.b[1].value.part = Part::High;
    return o;
}

Opnd MemOpnd(AsmValue address, std::uint32_t size, std::uint32_t stride) {
    Opnd o;
    o.size = static_cast<std::uint8_t>(size);
    for (std::uint32_t k = 0; k < size; k++) {
        o.b[k].kind = ByteKind::Mem;
        o.b[k].value = address;
        o.b[k].value.offset = static_cast<std::int32_t>(address.offset + k * stride);
    }
    return o;
}

Opnd InA() {
    Opnd o;
    o.size = 1;
    o.in_a = true;
    return o;
}

bool IsImm(const Opnd& o) {
    if (o.in_a || o.size == 0) {
        return false;
    }
    for (int k = 0; k < o.size; k++) {
        if (o.b[k].kind != ByteKind::Imm) {
            return false;
        }
    }
    return true;
}

bool IsVolatile(const Opnd& o) {
    if (o.in_a) {
        return true;
    }
    if (o.temp || IsImm(o)) {
        return false;
    }
    for (int k = 0; k < o.size; k++) {
        if (o.b[k].kind != ByteKind::Imm && o.b[k].kind != ByteKind::Reg) {
            return true;
        }
    }
    return false;
}

namespace {

bool IsMemoryByte(const ByteRef& b) {
    return b.kind == ByteKind::Mem || b.kind == ByteKind::MemIdx || b.kind == ByteKind::IndY;
}

// could byte x of src and byte y of dst be the same memory (no pointer reaches a temporary)
bool MaySameByte(const Opnd& src, const ByteRef& x, const Opnd& dst, const ByteRef& y) {
    if (!IsMemoryByte(x) || !IsMemoryByte(y)) {
        return false;
    }
    if (dst.temp || src.temp) {
        return x.kind == y.kind && x.value.sym == y.value.sym && x.value.offset == y.value.offset;
    }
    if (x.kind == ByteKind::IndY || y.kind == ByteKind::IndY) {
        return src.moved;
    }
    return x.value.sym == y.value.sym && x.value.offset == y.value.offset;
}

} // namespace

bool CrossAliases(const Opnd& src, const Opnd& dst) {
    if (src.in_a || src.size < 2 || dst.size < 2) {
        return false;
    }
    if (!src.moved) {
        return false;
    }
    // dst byte 0 is written before src byte 1 is read
    return MaySameByte(src, src.b[1], dst, dst.b[0]);
}

bool Overlaps(const Opnd& a, const Opnd& d) {
    for (int i = 0; i < a.size; i++) {
        for (int k = 0; k < d.size; k++) {
            if (MaySameByte(a, a.b[i], d, d.b[k])) {
                return true;
            }
        }
    }
    return false;
}

void CopyOpnd(Gen& g, const Opnd& src, const Opnd& dst) {
    if (src.in_a) {
        StoreA(g, dst, 0);
        return;
    }
    if (CrossAliases(src, dst)) {
        // high byte first works unless dst byte 1 is also src byte 0; else go through a temp
        if (!MaySameByte(src, src.b[0], dst, dst.b[1])) {
            LoadA(g, src, 1);
            StoreA(g, dst, 1);
            LoadA(g, src, 0);
            StoreA(g, dst, 0);
            return;
        }
        Opnd t = NewTemp(g, 2);
        CopyOpnd(g, src, t);
        CopyOpnd(g, t, dst);
        return;
    }
    for (int k = 0; k < dst.size; k++) {
        bool same_imm = k > 0 && src.b[k].kind == ByteKind::Imm &&
                        src.b[k - 1].kind == ByteKind::Imm &&
                        SameValue(src.b[k].value, src.b[k - 1].value);
        if (!same_imm) {
            LoadA(g, src, k);
        }
        StoreA(g, dst, k);
    }
}

void LoadAX(Gen& g, const Opnd& o) {
    const ByteRef& high = o.b[1];
    if (high.kind == ByteKind::Imm || high.kind == ByteKind::Mem) {
        LoadA(g, o, 0);
        Emit(g, Mn::LDX, high.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs, high.value);
        return;
    }
    AsmValue scratch = SymValue(g.be->scratch);
    LoadA(g, o, 1);
    Emit(g, Mn::STA, Mode::Abs, scratch);
    LoadA(g, o, 0);
    Emit(g, Mn::LDX, Mode::Abs, scratch);
}

void StoreAX(Gen& g, const Opnd& dst) {
    if (dst.b[0].kind == ByteKind::Mem && dst.b[1].kind == ByteKind::Mem) {
        Emit(g, Mn::STA, Mode::Abs, dst.b[0].value);
        Emit(g, Mn::STX, Mode::Abs, dst.b[1].value);
        return;
    }
    AsmValue scratch = SymValue(g.be->scratch);
    Emit(g, Mn::STX, Mode::Abs, scratch);
    StoreA(g, dst, 0);
    Emit(g, Mn::LDA, Mode::Abs, scratch);
    StoreA(g, dst, 1);
}

Opnd SpillA(Gen& g) {
    Opnd t = NewTemp(g, 1);
    StoreA(g, t, 0);
    return t;
}

bool MayWriteVarIn(const Gen& g, ExprId code, std::int32_t var) {
    return CodeMayWrite(*g.program, g.be->effects, code, var);
}

namespace {

// a frame or global byte some code might write; temporaries and scratch never are
bool MayWriteSym(const Gen& g, ExprId code, const AsmValue& v) {
    if (v.sym < 0) {
        return HasWrites(*g.program, code);
    }
    const SymInfo& info = g.be->info[static_cast<std::size_t>(v.sym)];
    if (info.role != SymRole::Var) {
        return false;
    }
    return MayWriteVarIn(g, code, info.owner);
}

} // namespace

bool MayWriteOpnd(const Gen& g, ExprId code, const Opnd& o) {
    if (code < 0 || o.size == 0) {
        return false;
    }
    if (o.in_a) {
        return true;
    }
    for (int k = 0; k < o.size; k++) {
        const ByteRef& b = o.b[k];
        switch (b.kind) {
        case ByteKind::Imm:
        case ByteKind::Reg:
            break;
        case ByteKind::Mem:
        case ByteKind::MemIdx:
            if (MayWriteSym(g, code, b.value)) {
                return true;
            }
            break;
        case ByteKind::IndY:
            // the pointer, and whatever it points at
            if (MayWriteSym(g, code, b.value) || HasWrites(*g.program, code)) {
                return true;
            }
            break;
        }
    }
    return o.index.used && !o.index.is_reg && MayWriteSym(g, code, o.index.value);
}

Opnd Snapshot(Gen& g, const Opnd& o) {
    if (o.in_a) {
        return SpillA(g);
    }
    if (!IsVolatile(o)) {
        return o;
    }
    Opnd t = NewTemp(g, o.size);
    CopyOpnd(g, o, t);
    return t;
}

bool PointerInZp(const Backend& be, std::int32_t var) {
    const Var& v = be.program->vars[static_cast<std::size_t>(var)];
    if (v.placement.has_address) {
        return v.placement.address + 1 < 0x100;
    }
    return be.deref[static_cast<std::size_t>(var)];
}

} // namespace tisple
