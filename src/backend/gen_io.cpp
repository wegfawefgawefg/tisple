// io_read and io_write: exactly one absolute access per byte, low byte first. At a constant
// address the instruction holds it; through an I/O base (io_bases.hpp) the instruction's
// operand is the base's symbol plus the constant, patched when the base is set; at any other
// address the code stores the address into the instruction first.

#include "backend/gen.hpp"
#include "backend/io_bases.hpp"

namespace tisple {

namespace {

// an instruction's operand bytes at a code label (self-modifying I/O)
Opnd PatchOpnd(std::int32_t label) {
    return MemOpnd(SymValue(label, 1), 2, 1);
}

// how an access reaches its address
struct IoAddress {
    bool fixed = false;
    std::uint16_t address = 0;
    std::int32_t base_sym = -1; // through an I/O base: its symbol, plus offset
    std::int32_t offset = 0;
    std::int32_t patch[2] = {-1, -1}; // a computed address: the labels of the instructions
};

// the address worked out; for a computed one, the code that stores it into the instructions.
// later: what is worked out after the address, before the access (-1 for none)
IoAddress GenIoAddress(Gen& g, ExprId addr, ExprId later, std::uint32_t size) {
    IoAddress at;
    at.fixed = ConstOf(g, addr, at.address);
    if (at.fixed) {
        return at;
    }
    std::int32_t var = -1;
    if (IoBaseAddress(*g.program, g.be->io_bases, g.be->effects, addr, later, var, at.offset)) {
        at.base_sym = g.be->io_sym[static_cast<std::size_t>(var)];
        return at;
    }
    Opnd a = Eval(g, addr, nullptr);
    at.patch[0] = NewLabel(g);
    CopyOpnd(g, a, PatchOpnd(at.patch[0]));
    if (size == 2) {
        at.patch[1] = NewLabel(g);
        Combine16(g, Op::Add, a, ImmOpnd(1, 2), PatchOpnd(at.patch[1]));
    }
    return at;
}

// byte k's access
void EmitIoAccess(Gen& g, Mn mn, const IoAddress& at, std::uint32_t k) {
    if (at.fixed) {
        Emit(g, mn, Mode::Abs, Num(static_cast<std::uint16_t>(at.address + k)));
        return;
    }
    if (at.base_sym >= 0) {
        std::int32_t offset = static_cast<std::int32_t>((at.offset + k) & 0xFFFF);
        Emit(g, mn, Mode::Abs, SymValue(at.base_sym, offset));
        return;
    }
    PlaceLabel(g, at.patch[k]);
    Emit(g, mn, Mode::Abs, Num(0xFFFF));
}

void GenIoWrite(Gen& g, const Expr& e) {
    std::uint32_t size = e.value;
    IoAddress at = GenIoAddress(g, e.a, e.b, size);
    Opnd v = Eval(g, e.b, nullptr);
    for (std::uint32_t k = 0; k < size; k++) {
        LoadA(g, v, static_cast<int>(k));
        EmitIoAccess(g, Mn::STA, at, k);
    }
}

} // namespace

Opnd EvalIoRead(Gen& g, const Expr& e, const Opnd* dst) {
    std::uint32_t size = TypeOf(*g.program, e.type).size;
    IoAddress at = GenIoAddress(g, e.a, -1, size);
    Opnd out = InA();
    if (size == 2) {
        out = dst ? *dst : NewTemp(g, 2);
    }
    for (std::uint32_t k = 0; k < size; k++) {
        EmitIoAccess(g, Mn::LDA, at, k);
        if (size == 2) {
            StoreA(g, out, static_cast<int>(k));
        }
    }
    return out;
}

void GenIo(Gen& g, ExprId id) {
    const Expr& e = ExprOf(*g.program, id);
    if (e.kind == ExprKind::IoWrite) {
        GenIoWrite(g, e);
        return;
    }
    EvalIoRead(g, e, nullptr);
}

} // namespace tisple
