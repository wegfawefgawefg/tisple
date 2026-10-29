#include "backend/gen.hpp"

// 8-bit shifts, the value in A.
//
// - By a constant: one ASL, LSR or (signed) CMP #$80 / ROR a bit, except where the other way
//   round is shorter: u8 >> 7 is the top bit through the carry (ASL A / LDA #0 / ROL A), >> 6
//   three rotates through the carry and a mask, << 7 and << 6 the same mirrored. A signed >> 7
//   is the sign in every bit; a signed >> 4 to 6 is the unsigned shift with its sign bit
//   extended (EOR m / SEC / SBC m, m the shifted sign bit).
// - By a variable: a loop of the count in X (Y when X is pinned), none for 0. When the count
//   may be 8 or more, one compare first: those give 0, or the sign in every bit.

namespace tisple {

namespace {

void Rotates(Gen& g, Mn mn, int n) {
    for (int i = 0; i < n; i++) {
        Emit(g, mn, Mode::Acc);
    }
}

void UnsignedShift(Gen& g, Op op, int n) {
    bool left = op == Op::Shl;
    if (n == 7) {
        // the bit that stays, through the carry
        Emit(g, left ? Mn::LSR : Mn::ASL, Mode::Acc);
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        Emit(g, left ? Mn::ROR : Mn::ROL, Mode::Acc);
        return;
    }
    if (n == 6) {
        // nine-bit rotates: the two bits that stay come round, the carry's old bit is masked
        Rotates(g, left ? Mn::ROR : Mn::ROL, 3);
        Emit(g, Mn::AND, Mode::Imm, Num(left ? 0xC0 : 0x03));
        return;
    }
    Rotates(g, left ? Mn::ASL : Mn::LSR, n);
}

void SignedShiftRight(Gen& g, int n) {
    if (n == 7) {
        SignByteA(g);
        return;
    }
    if (n >= 4) {
        // the unsigned shift, then its bit 7 - n (the sign) copied up: (v ^ m) - m
        std::int32_t m = 0x80 >> n;
        UnsignedShift(g, Op::Shr, n);
        Emit(g, Mn::EOR, Mode::Imm, Num(m));
        Emit(g, Mn::SEC, Mode::Imp);
        Emit(g, Mn::SBC, Mode::Imm, Num(m));
        return;
    }
    for (int i = 0; i < n; i++) {
        ShiftA(g, Op::Shr, true);
    }
}

void ShiftConst8(Gen& g, Op op, bool is_signed, std::uint16_t c) {
    if (c >= 8 && (op == Op::Shl || !is_signed)) {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        return;
    }
    int n = c > 7 ? 7 : c;
    if (op == Op::Shr && is_signed) {
        SignedShiftRight(g, n);
        return;
    }
    UnsignedShift(g, op, n);
}

// A shifted by the count operand; count_max: no count is larger
void ShiftLoop8(Gen& g, Op op, bool is_signed, const Opnd& count, std::uint32_t count_max) {
    int r = CounterReg(g);
    std::int32_t loop = NewLabel(g);
    std::int32_t done = NewLabel(g);
    LoadCount(g, r, count);
    Branch(g, Mn::BEQ, done);
    if (count_max >= 8) {
        // 8 or more: every bit is out
        Emit(g, r == 0 ? Mn::CPX : Mn::CPY, Mode::Imm, Num(8));
        Branch(g, Mn::BCC, loop);
        if (op == Op::Shr && is_signed) {
            SignByteA(g);
            Jump(g, done);
        } else {
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            Branch(g, Mn::BCS, done);
        }
    }
    PlaceLabel(g, loop);
    ShiftA(g, op, is_signed);
    Emit(g, r == 0 ? Mn::DEX : Mn::DEY, Mode::Imp);
    Branch(g, Mn::BNE, loop);
    PlaceLabel(g, done);
}

} // namespace

void EvalShift8(Gen& g, const Expr& e) {
    bool is_signed = IsSignedKind(ExprKindOf(*g.program, e.a));
    std::uint16_t c = 0;
    if (ConstValue(*g.program, e.b, c)) {
        EvalA(g, e.a);
        ShiftConst8(g, e.op, is_signed, c);
        return;
    }
    std::uint32_t count_max = MaxOf(g, e.b);
    Opnd count = TrySimple(g, e.b);
    if (CountReady(count)) {
        EvalA(g, e.a);
        ShiftLoop8(g, e.op, is_signed, count, count_max);
        return;
    }
    EvalA(g, e.a);
    Opnd value = SpillA(g);
    EvalA(g, e.b);
    count = SpillA(g);
    LoadA(g, value, 0);
    ShiftLoop8(g, e.op, is_signed, count, count_max);
}

} // namespace tisple
