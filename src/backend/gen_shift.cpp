#include "backend/gen.hpp"

namespace tisple {

namespace {

// a free index register for a counter; X unless pinned
int CounterReg(const Gen& g) {
    if (g.pin[0] < 0) {
        return 0;
    }
    return 1;
}

void ShiftA(Gen& g, Op op, bool is_signed) {
    if (op == Op::Shl) {
        Emit(g, Mn::ASL, Mode::Acc);
        return;
    }
    if (is_signed) {
        Emit(g, Mn::CMP, Mode::Imm, Num(0x80));
        Emit(g, Mn::ROR, Mode::Acc);
        return;
    }
    Emit(g, Mn::LSR, Mode::Acc);
}

// a count LDX/LDY can load directly
bool CountReady(const Opnd& count) {
    ByteKind kind = count.b[0].kind;
    return count.size != 0 && !count.in_a &&
           (kind == ByteKind::Imm || kind == ByteKind::Mem || kind == ByteKind::Reg);
}

// the count operand into register r (without touching A); flags show the count
void LoadCount(Gen& g, int r, const Opnd& count) {
    const ByteRef& b = count.b[0];
    if (b.kind == ByteKind::Reg && b.reg != r) {
        AsmValue scratch = SymValue(g.be->scratch);
        Emit(g, b.reg == 0 ? Mn::STX : Mn::STY, Mode::Abs, scratch);
        Emit(g, r == 0 ? Mn::LDX : Mn::LDY, Mode::Abs, scratch);
        return;
    }
    if (b.kind == ByteKind::Reg) {
        g.violation = true; // the counter would count down the loop variable
        return;
    }
    Mode mode = b.kind == ByteKind::Imm ? Mode::Imm : Mode::Abs;
    Emit(g, r == 0 ? Mn::LDX : Mn::LDY, mode, b.value);
}

// 8-bit shift with A holding the value; count in an operand
void ShiftLoop8(Gen& g, Op op, bool is_signed, const Opnd& count) {
    int r = CounterReg(g);
    std::int32_t loop = NewLabel(g);
    std::int32_t done = NewLabel(g);
    LoadCount(g, r, count);
    Branch(g, Mn::BEQ, done);
    PlaceLabel(g, loop);
    ShiftA(g, op, is_signed);
    Emit(g, r == 0 ? Mn::DEX : Mn::DEY, Mode::Imp);
    Branch(g, Mn::BNE, loop);
    PlaceLabel(g, done);
}

// value into a 16-bit dst that RMW instructions can work on (else a temporary)
Opnd ShiftTarget(Gen& g, const Opnd* dst) {
    if (dst && dst->b[0].kind == ByteKind::Mem && dst->b[1].kind == ByteKind::Mem) {
        return *dst;
    }
    return NewTemp(g, 2);
}

void SignByteA(Gen& g) {
    // A's bit 7 into C, then 0 + $FF + C is $00 or $FF; flip it
    Emit(g, Mn::ASL, Mode::Acc);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Emit(g, Mn::ADC, Mode::Imm, Num(0xFF));
    Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
}

// 16-bit shift of a into d (plain memory); a constant count c, or the count operand
void Shift16(Gen& g, Op op, bool is_signed, const Opnd& a, const Opnd& d, bool fixed,
             std::uint16_t c, const Opnd& count) {
    bool left = op == Op::Shl;
    if (fixed && c >= 8) {
        int n = c >= 16 ? 15 : c;
        if (c >= 16 && (left || !is_signed)) {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            StoreA(g, d, 0);
            StoreA(g, d, 1);
            return;
        }
        if (left) {
            LoadA(g, a, 0);
            for (int i = 8; i < n; i++) {
                ShiftA(g, Op::Shl, false);
            }
            StoreA(g, d, 1);
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            StoreA(g, d, 0);
            return;
        }
        LoadA(g, a, 1);
        for (int i = 8; i < n; i++) {
            ShiftA(g, Op::Shr, is_signed);
        }
        StoreA(g, d, 0);
        if (is_signed) {
            LoadA(g, a, 1);
            SignByteA(g);
        } else {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
        }
        StoreA(g, d, 1);
        return;
    }
    if (fixed && c >= 5) {
        // shift the other way through a third byte, 8 - c steps instead of c
        int steps = 8 - c;
        if (left) {
            // [hi lo 00] >> steps: the middle and low bytes are the result
            LoadA(g, a, 0);
            StoreA(g, d, 1);
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            StoreA(g, d, 0);
            LoadA(g, a, 1);
            for (int i = 0; i < steps; i++) {
                Emit(g, Mn::LSR, Mode::Acc);
                RmwByte(g, Mn::ROR, d, 1);
                RmwByte(g, Mn::ROR, d, 0);
            }
            return;
        }
        // [sign hi lo] << steps: the top and middle bytes are the result
        Opnd low = NewTemp(g, 1);
        LoadA(g, a, 0);
        StoreA(g, low, 0);
        LoadA(g, a, 1);
        StoreA(g, d, 0);
        if (is_signed) {
            SignByteA(g);
        } else {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
        }
        for (int i = 0; i < steps; i++) {
            RmwByte(g, Mn::ASL, low, 0);
            RmwByte(g, Mn::ROL, d, 0);
            Emit(g, Mn::ROL, Mode::Acc);
        }
        StoreA(g, d, 1);
        return;
    }
    // the low byte shifts in memory, the high byte in A
    LoadA(g, a, 0);
    StoreA(g, d, 0);
    LoadA(g, a, 1);
    std::int32_t done = -1;
    std::int32_t loop = -1;
    int r = CounterReg(g);
    int steps = fixed ? c : 1;
    if (!fixed) {
        done = NewLabel(g);
        loop = NewLabel(g);
        LoadCount(g, r, count);
        Branch(g, Mn::BEQ, done);
        PlaceLabel(g, loop);
    }
    for (int i = 0; i < steps; i++) {
        if (left) {
            RmwByte(g, Mn::ASL, d, 0);
            Emit(g, Mn::ROL, Mode::Acc);
            continue;
        }
        ShiftA(g, Op::Shr, is_signed);
        RmwByte(g, Mn::ROR, d, 0);
    }
    if (!fixed) {
        Emit(g, r == 0 ? Mn::DEX : Mn::DEY, Mode::Imp);
        Branch(g, Mn::BNE, loop);
        PlaceLabel(g, done);
    }
    StoreA(g, d, 1);
}

void Shift8(Gen& g, const Expr& e);

void Shift8(Gen& g, const Expr& e) {
    bool is_signed = IsSignedKind(ExprKindOf(*g.program, e.a));
    std::uint16_t c = 0;
    if (ConstValue(*g.program, e.b, c)) {
        EvalA(g, e.a);
        int n = c;
        if (n >= 8 && (e.op == Op::Shl || !is_signed)) {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            return;
        }
        n = n > 7 ? 7 : n;
        for (int i = 0; i < n; i++) {
            ShiftA(g, e.op, is_signed);
        }
        return;
    }
    Opnd count = TrySimple(g, e.b);
    if (CountReady(count)) {
        EvalA(g, e.a);
        ShiftLoop8(g, e.op, is_signed, count);
        return;
    }
    EvalA(g, e.a);
    Opnd value = SpillA(g);
    EvalA(g, e.b);
    count = SpillA(g);
    LoadA(g, value, 0);
    ShiftLoop8(g, e.op, is_signed, count);
}

} // namespace

void EvalShift8(Gen& g, const Expr& e) {
    Shift8(g, e);
}

Opnd ShiftConst16(Gen& g, Op op, bool is_signed, const Opnd& a, std::uint16_t count,
                  const Opnd* dst) {
    Opnd d = ShiftTarget(g, dst);
    if (Overlaps(g, a, d)) {
        d = NewTemp(g, 2);
    }
    Shift16(g, op, is_signed, a, d, true, count, Opnd());
    return d;
}

Opnd EvalShift16(Gen& g, const Expr& e, const Opnd* dst) {
    Op op = e.op;
    Opnd a = Eval(g, e.a, nullptr);
    std::uint16_t c = 0;
    bool fixed = ConstValue(*g.program, e.b, c);
    Opnd count;
    if (!fixed) {
        if (MayWriteOpnd(g, e.b, a)) {
            a = Snapshot(g, a);
        }
        count = TrySimple(g, e.b);
        if (!CountReady(count)) {
            EvalA(g, e.b);
            count = SpillA(g);
        }
    }
    Opnd d = ShiftTarget(g, dst);
    if (Overlaps(g, a, d)) {
        d = NewTemp(g, 2);
    }
    Shift16(g, op, IsSignedKind(ExprKindOf(*g.program, e.a)), a, d, fixed, c, count);
    return d;
}

} // namespace tisple
