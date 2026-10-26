// (jsr addr (a v) (x v) (y v)): machine code called with A, X and Y set; its value is A
// afterwards.
//
// - The address, then a, x and y are worked out in the order written. A constant address is
//   a plain JSR; a computed one is stored into the JSR's own operand first, as io_read does
//   with a computed address (3 cycles less than a JMP (ind) trampoline, and no page-wrap bug).
// - Each register's value ends as an immediate or a memory byte (a temporary if it took code,
//   or if a later register's code could change it), so the three loads at the end touch
//   nothing else. When x and y need no code, a goes straight into A and X and Y load after.
// - Emit(JSR) forgets what X and Y hold and marks a loop counter pinned in a register as
//   broken. Everything else that keeps the call in place lives with the other effects:
//   analysis.cpp (HasEffects, HasWrites, MayWriteVar), effects.cpp and ranges.cpp.

#include "backend/gen.hpp"

namespace tisple {

namespace {

// a value LDA, LDX or LDY reads with no index: a number or a plain memory byte
bool PlainByte(const Opnd& o) {
    if (o.size != 1 || o.in_a) {
        return false;
    }
    return o.b[0].kind == ByteKind::Imm || o.b[0].kind == ByteKind::Mem;
}

// id with no code and no index; size 0 otherwise
Opnd PlainSimple(Gen& g, ExprId id) {
    if (id < 0) {
        return Opnd();
    }
    Opnd o = TrySimple(g, id);
    return PlainByte(o) ? o : Opnd();
}

// the value as a plain byte that none of the later register values can change
Opnd PlainValue(Gen& g, ExprId id, const ExprId* later, int later_count) {
    Opnd o = Eval(g, id, nullptr);
    if (!o.in_a && !PlainByte(o)) {
        LoadA(g, o, 0);
        o = InA();
    }
    if (o.in_a) {
        return SpillA(g);
    }
    for (int i = 0; i < later_count; i++) {
        if (MayWriteOpnd(g, later[i], o)) {
            Opnd t = NewTemp(g, 1);
            CopyOpnd(g, o, t);
            return t;
        }
    }
    return o;
}

void LoadReg(Gen& g, Mn mn, const Opnd& o) {
    const ByteRef& b = o.b[0];
    Emit(g, mn, b.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs, b.value);
}

} // namespace

Opnd EvalJsr(Gen& g, ExprId id) {
    const Expr& e = ExprOf(*g.program, id);
    ExprId regs[3] = {e.b, -1, -1};
    if (e.c >= 0) {
        regs[1] = ExprOf(*g.program, e.c).a;
        regs[2] = ExprOf(*g.program, e.c).b;
    }
    std::uint16_t address = 0;
    bool fixed = ConstValue(*g.program, e.a, address);
    std::int32_t patch = -1;
    if (!fixed) {
        Opnd a = Eval(g, e.a, nullptr);
        patch = NewLabel(g);
        CopyOpnd(g, a, MemOpnd(SymValue(patch, 1), 2, 1));
    }

    Opnd values[3];
    Opnd x = PlainSimple(g, regs[1]);
    Opnd y = PlainSimple(g, regs[2]);
    // x and y are read after a's code either way, as written
    bool a_last = regs[0] >= 0 && (regs[1] < 0 || x.size != 0) && (regs[2] < 0 || y.size != 0);
    if (a_last) {
        values[1] = x;
        values[2] = y;
        EvalA(g, regs[0]);
    } else {
        for (int r = 0; r < 3; r++) {
            if (regs[r] >= 0) {
                values[r] = PlainValue(g, regs[r], regs + r + 1, 2 - r);
            }
        }
        if (values[0].size != 0) {
            LoadA(g, values[0], 0);
        }
    }
    if (values[1].size != 0) {
        LoadReg(g, Mn::LDX, values[1]);
    }
    if (values[2].size != 0) {
        LoadReg(g, Mn::LDY, values[2]);
    }
    if (fixed) {
        Emit(g, Mn::JSR, Mode::Abs, Num(address));
    } else {
        PlaceLabel(g, patch);
        Emit(g, Mn::JSR, Mode::Abs, Num(0xFFFF));
    }
    return InA();
}

} // namespace tisple
