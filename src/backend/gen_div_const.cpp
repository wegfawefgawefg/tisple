#include "backend/gen_div.hpp"

#include <string>

// Division by constants.
//
// - Unsigned by 3 to 127, not a power of two: long division, one step per dividend bit with
//   the divisor as an immediate, unrolled. The remainder stays below 128, so it lives in A
//   and doubling it never carries out; the first steps, before the remainder can reach the
//   divisor, are just shifts. A 16-bit dividend is its high byte divided first (nothing to
//   do when it is below the divisor), then the low byte with that remainder. The quotient
//   builds up in rt_a, the remainder ends in A. Inside a loop the steps are inline; else the
//   program has one routine per divisor and width (rt.divu8_7, rt.divu16_7). A dividend
//   below the divisor is one compare: the quotient is 0, the remainder the dividend.
// - Signed by c: by 1 the dividend, by -1 its negation, the remainder 0. A magnitude below
//   128 or a power of two: a routine per divisor (rt.divs8_7, rt.divs16_n3) divides the
//   dividend's magnitude as above, then the quotient is negated when the signs differ and
//   the remainder takes the dividend's sign.

namespace tisple {

namespace {

int BitLength(std::uint32_t v) {
    int n = 0;
    for (; v != 0; v >>= 1) {
        n++;
    }
    return n;
}

// one step of long division: the next dividend bit from q into A, then A -= c if it fits;
// the carry is the quotient bit, which the next ROL shifts into q
void DivStep(Gen& g, const AsmValue& q, std::uint32_t c) {
    std::int32_t skip = NewLabel(g);
    Emit(g, Mn::ROL, Mode::Abs, q);
    Emit(g, Mn::ROL, Mode::Acc);
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    Branch(g, Mn::BCC, skip);
    Emit(g, Mn::SBC, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    PlaceLocalLabel(g, skip);
}

// q (a byte holding the dividend) divided by c with A holding the remainder so far (below
// c): q becomes the quotient byte and A the remainder. The first `early` steps only shift,
// when A starts at 0 and cannot reach c yet. The bit the first ROL shifts in comes out of
// the last one.
void DivideByte(Gen& g, const AsmValue& q, std::uint32_t c, int early) {
    for (int i = 0; i < early; i++) {
        Emit(g, Mn::ROL, Mode::Abs, q);
        Emit(g, Mn::ROL, Mode::Acc);
    }
    for (int i = early; i < 8; i++) {
        DivStep(g, q, c);
    }
    Emit(g, Mn::ROL, Mode::Abs, q);
}

// A / c into rt_a, A % c in A
void DivideByteInA(Gen& g, std::uint32_t c) {
    AsmValue q = SymValue(g.be->rt_a);
    Emit(g, Mn::STA, Mode::Abs, q);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    DivideByte(g, q, c, BitLength(c) - 1);
}

// the same as a routine, a dividend below c taking one compare
void DivideByteRoutine(Gen& g, std::uint32_t c) {
    std::int32_t below = NewLabel(g);
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    Branch(g, Mn::BCC, below);
    DivideByteInA(g, c);
    Emit(g, Mn::RTS, Mode::Imp);
    PlaceLocalLabel(g, below);
    Emit(g, Mn::LDX, Mode::Imm, Num(0));
    Emit(g, Mn::STX, Mode::Abs, SymValue(g.be->rt_a));
}

// A = A / c or A % c inline, when no other division wants the other result: a dividend
// below c is the remainder, its quotient 0
void DivideByteAlone(Gen& g, Op op, std::uint32_t c) {
    std::int32_t done = NewLabel(g);
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    if (op == Op::Mod) {
        Branch(g, Mn::BCC, done);
        DivideByteInA(g, c);
        PlaceLocalLabel(g, done);
        return;
    }
    std::int32_t full = NewLabel(g);
    Branch(g, Mn::BCS, full);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Branch(g, Mn::BEQ, done);
    PlaceLocalLabel(g, full);
    DivideByteInA(g, c);
    Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_a));
    PlaceLocalLabel(g, done);
}

// the 16-bit dividend at rt_a divided by c, as a routine: quotient in rt_a, remainder in A.
// Below c it is one compare; else the high byte goes first, and when it is below c it is the
// remainder so far and the quotient's high byte 0.
void DivideWordRoutine(Gen& g, std::uint32_t c) {
    AsmValue q = SymValue(g.be->rt_a);
    AsmValue q_high = SymValue(g.be->rt_a, 1);
    std::int32_t high = NewLabel(g);
    std::int32_t below = NewLabel(g);
    std::int32_t low_half = NewLabel(g);
    std::int32_t full = NewLabel(g);
    Emit(g, Mn::LDA, Mode::Abs, q_high);
    Branch(g, Mn::BNE, high);
    Emit(g, Mn::LDA, Mode::Abs, q);
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    Branch(g, Mn::BCC, below);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Branch(g, Mn::BEQ, low_half);
    PlaceLocalLabel(g, below);
    Emit(g, Mn::LDX, Mode::Imm, Num(0));
    Emit(g, Mn::STX, Mode::Abs, q);
    Emit(g, Mn::RTS, Mode::Imp);
    PlaceLocalLabel(g, high);
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    Branch(g, Mn::BCS, full);
    Emit(g, Mn::LDX, Mode::Imm, Num(0));
    Emit(g, Mn::STX, Mode::Abs, q_high);
    Branch(g, Mn::BCC, low_half);
    PlaceLocalLabel(g, full);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    DivideByte(g, q_high, c, BitLength(c) - 1);
    PlaceLocalLabel(g, low_half);
    DivideByte(g, q, c, 0);
}

// the 16-bit operand a divided by c inline, the same way but leaving X and Y alone
void DivideWordInline(Gen& g, const Opnd& a, std::uint32_t c) {
    AsmValue q = SymValue(g.be->rt_a);
    AsmValue q_high = SymValue(g.be->rt_a, 1);
    std::int32_t low_half = NewLabel(g);
    LoadA(g, a, 0);
    Emit(g, Mn::STA, Mode::Abs, q);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Emit(g, Mn::STA, Mode::Abs, q_high);
    LoadA(g, a, 1);
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    Branch(g, Mn::BCC, low_half);
    Emit(g, Mn::STA, Mode::Abs, q_high);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    DivideByte(g, q_high, c, BitLength(c) - 1);
    PlaceLocalLabel(g, low_half);
    DivideByte(g, q, c, 0);
}

// the shared unsigned routine for divisor c, made on first use
std::int32_t DivRoutineSym(Gen& g, std::uint32_t c, bool wide) {
    for (const DivRoutine& r : g.be->div_routines) {
        if (!r.is_signed && r.divisor == c && r.wide == wide) {
            return r.sym;
        }
    }
    DivRoutine r;
    r.divisor = c;
    r.wide = wide;
    std::string name = (wide ? "rt.divu16_" : "rt.divu8_") + std::to_string(c);
    r.sym = NewSym(*g.be, name, SymRole::Label, -1);
    g.be->div_routines.push_back(r);
    return r.sym;
}

std::uint32_t Magnitude(std::int32_t c) {
    return static_cast<std::uint32_t>(c < 0 ? -c : c);
}

// the signed routine for divisor c (not 0, 1 or -1), made on first use
std::int32_t SignedRoutineSym(Gen& g, std::int32_t c, bool wide) {
    for (const DivRoutine& r : g.be->div_routines) {
        if (r.is_signed && r.value == c && r.wide == wide) {
            return r.sym;
        }
    }
    std::uint32_t m = Magnitude(c);
    DivRoutine r;
    r.is_signed = true;
    r.value = c;
    r.divisor = m;
    r.wide = wide;
    r.needs = PowerOfTwo(m) < 0 ? DivRoutineSym(g, m, wide) : -1;
    std::string name =
        (wide ? "rt.divs16_" : "rt.divs8_") + std::string(c < 0 ? "n" : "") + std::to_string(m);
    r.sym = NewSym(*g.be, name, SymRole::Label, -1);
    g.be->div_routines.push_back(r);
    return r.sym;
}

void Imp(Gen& g, Mn mn) {
    Emit(g, mn, Mode::Imp);
}

void NegateA(Gen& g) {
    Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
    Imp(g, Mn::CLC);
    Emit(g, Mn::ADC, Mode::Imm, Num(1));
}

// rt_a = -rt_a, keeping A (in Y)
void NegateQuotient8(Gen& g) {
    Imp(g, Mn::TAY);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Imp(g, Mn::SEC);
    Emit(g, Mn::SBC, Mode::Abs, SymValue(g.be->rt_a));
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_a));
    Imp(g, Mn::TYA);
}

void Negate16(Gen& g, std::int32_t sym) {
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Imp(g, Mn::SEC);
    Emit(g, Mn::SBC, Mode::Abs, SymValue(sym));
    Emit(g, Mn::STA, Mode::Abs, SymValue(sym));
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Emit(g, Mn::SBC, Mode::Abs, SymValue(sym, 1));
    Emit(g, Mn::STA, Mode::Abs, SymValue(sym, 1));
}

// A (an unsigned magnitude) by m: quotient in rt_a, remainder in A
void MagnitudeByte(Gen& g, const DivRoutine& r) {
    int k = PowerOfTwo(r.divisor);
    if (k < 0) {
        Emit(g, Mn::JSR, Mode::Abs, SymValue(r.needs));
        return;
    }
    Imp(g, Mn::TAY);
    for (int i = 0; i < k; i++) {
        Emit(g, Mn::LSR, Mode::Acc);
    }
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_a));
    Imp(g, Mn::TYA);
    Emit(g, Mn::AND, Mode::Imm, Num(static_cast<std::int32_t>(r.divisor - 1)));
}

// A / c into rt_a, A % c in A, signed
void SignedByteRoutine(Gen& g, const DivRoutine& r) {
    std::int32_t negative = NewLabel(g);
    Emit(g, Mn::CMP, Mode::Imm, Num(0x80));
    Branch(g, Mn::BCS, negative);
    if (r.value > 0 && r.needs >= 0) {
        Emit(g, Mn::JMP, Mode::Abs, SymValue(r.needs));
    } else {
        MagnitudeByte(g, r);
        if (r.value < 0) {
            NegateQuotient8(g);
        }
        Imp(g, Mn::RTS);
    }
    PlaceLocalLabel(g, negative);
    NegateA(g);
    MagnitudeByte(g, r);
    NegateA(g);
    if (r.value > 0) {
        NegateQuotient8(g);
    }
}

// rt_a (an unsigned magnitude) by m: quotient in rt_a, remainder in rt_r
void MagnitudeWord(Gen& g, const DivRoutine& r) {
    AsmValue q = SymValue(g.be->rt_a);
    AsmValue q_high = SymValue(g.be->rt_a, 1);
    int k = PowerOfTwo(r.divisor);
    if (k < 0) {
        Emit(g, Mn::JSR, Mode::Abs, SymValue(r.needs));
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_r));
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_r, 1));
        return;
    }
    std::uint32_t mask = r.divisor - 1;
    for (int i = 0; i < 2; i++) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_a, i));
        Emit(g, Mn::AND, Mode::Imm, Num(static_cast<std::int32_t>((mask >> (8 * i)) & 0xFF)));
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_r, i));
    }
    if (k >= 5 && k < 8) {
        // fewer shifts the other way: a << (8 - k) over three bytes, its top two the quotient
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        for (int i = k; i < 8; i++) {
            Emit(g, Mn::ASL, Mode::Abs, q);
            Emit(g, Mn::ROL, Mode::Abs, q_high);
            Emit(g, Mn::ROL, Mode::Acc);
        }
        Emit(g, Mn::LDX, Mode::Abs, q_high);
        Emit(g, Mn::STX, Mode::Abs, q);
        Emit(g, Mn::STA, Mode::Abs, q_high);
        return;
    }
    if (k < 8) {
        for (int i = 0; i < k; i++) {
            Emit(g, Mn::LSR, Mode::Abs, q_high);
            Emit(g, Mn::ROR, Mode::Abs, q);
        }
        return;
    }
    Emit(g, Mn::LDA, Mode::Abs, q_high);
    for (int i = 8; i < k; i++) {
        Emit(g, Mn::LSR, Mode::Acc);
    }
    Emit(g, Mn::STA, Mode::Abs, q);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Emit(g, Mn::STA, Mode::Abs, q_high);
}

// rt_a / c into rt_a, rt_a % c into rt_r, signed
void SignedWordRoutine(Gen& g, const DivRoutine& r) {
    std::int32_t negative = NewLabel(g);
    Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_a, 1));
    Branch(g, Mn::BMI, negative);
    MagnitudeWord(g, r);
    if (r.value < 0) {
        Negate16(g, g.be->rt_a);
    }
    Imp(g, Mn::RTS);
    PlaceLocalLabel(g, negative);
    Negate16(g, g.be->rt_a);
    MagnitudeWord(g, r);
    Negate16(g, g.be->rt_r);
    if (r.value > 0) {
        Negate16(g, g.be->rt_a);
    }
}

} // namespace

void DivSmallConst8(Gen& g, ExprId id, const Expr& e, std::uint32_t c) {
    bool pair = g.be->div_pairs[static_cast<std::size_t>(id)];
    if (g.loop_depth > 0 && !pair) {
        DivideByteAlone(g, e.op, c);
        g.div.valid = false;
        return;
    }
    if (g.loop_depth > 0) {
        DivideByteInA(g, c);
    } else {
        Emit(g, Mn::JSR, Mode::Abs, SymValue(DivRoutineSym(g, c, false)));
    }
    Remember(g, e, KeepRemainder(g, id), false);
    if (e.op == Op::Div) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_a));
    }
}

Opnd DivSmallConst16(Gen& g, ExprId id, const Expr& e, const Opnd& a, std::uint32_t c,
                     const Opnd* dst) {
    if (g.loop_depth > 0) {
        DivideWordInline(g, a, c);
    } else {
        CopyOpnd(g, a, MemOpnd(SymValue(g.be->rt_a), 2, 1));
        Emit(g, Mn::JSR, Mode::Abs, SymValue(DivRoutineSym(g, c, true)));
    }
    Remember(g, e, KeepRemainder(g, id), true);
    if (e.op == Op::Div) {
        return QuotientOut(g, dst);
    }
    return ByteRemainderOut(g, dst);
}

void DivLarge8(Gen& g, Op op, std::uint32_t c) {
    Emit(g, Mn::CMP, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    if (op == Op::Div) {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        Emit(g, Mn::ROL, Mode::Acc);
        return;
    }
    std::int32_t skip = NewLabel(g);
    Branch(g, Mn::BCC, skip);
    Emit(g, Mn::SBC, Mode::Imm, Num(static_cast<std::int32_t>(c)));
    PlaceLocalLabel(g, skip);
}

bool DivSignedConst8(Gen& g, ExprId id, const Expr& e, std::int32_t c) {
    if (c == 0) {
        return false;
    }
    if (c == 1 || c == -1) {
        if (e.op == Op::Mod) {
            if (HasEffects(*g.program, e.a)) {
                EvalA(g, e.a);
            }
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            return true;
        }
        EvalA(g, e.a);
        if (c == -1) {
            NegateA(g);
        }
        return true;
    }
    EvalA(g, e.a);
    Emit(g, Mn::JSR, Mode::Abs, SymValue(SignedRoutineSym(g, c, false)));
    Remember(g, e, KeepRemainder(g, id), false);
    if (e.op == Op::Div) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_a));
    }
    return true;
}

bool DivSignedConst16(Gen& g, ExprId, const Expr& e, std::int32_t c, const Opnd* dst, Opnd& out) {
    std::uint32_t m = Magnitude(c);
    if (c == 0 || (m >= 128 && PowerOfTwo(m) < 0)) {
        return false;
    }
    if (c == 1 || c == -1) {
        if (e.op == Op::Mod) {
            if (HasEffects(*g.program, e.a)) {
                Eval(g, e.a, nullptr);
            }
            out = ImmOpnd(0, 2);
            return true;
        }
        if (c == 1) {
            out = Eval(g, e.a, dst);
            return true;
        }
        Opnd a = Eval(g, e.a, nullptr);
        out = dst ? *dst : NewTemp(g, 2);
        Combine16(g, Op::Sub, ImmOpnd(0, 2), a, out);
        return true;
    }
    Opnd a = Eval(g, e.a, nullptr);
    CopyOpnd(g, a, MemOpnd(SymValue(g.be->rt_a), 2, 1));
    Emit(g, Mn::JSR, Mode::Abs, SymValue(SignedRoutineSym(g, c, true)));
    Remember(g, e, true, false);
    out = dst ? *dst : NewTemp(g, 2);
    AsmValue result = SymValue(e.op == Op::Div ? g.be->rt_a : g.be->rt_r);
    CopyOpnd(g, MemOpnd(result, 2, 1), out);
    return true;
}

void EmitDivRoutines(Backend& be, std::vector<AsmItem>& out) {
    Gen g;
    g.be = &be;
    g.program = be.program;
    g.out = &out;
    for (std::size_t i = 0; i < be.div_routines.size(); i++) {
        DivRoutine r = be.div_routines[i];
        PlaceLocalLabel(g, r.sym);
        if (r.is_signed && r.wide) {
            SignedWordRoutine(g, r);
        } else if (r.is_signed) {
            SignedByteRoutine(g, r);
        } else if (r.wide) {
            DivideWordRoutine(g, r.divisor);
        } else {
            DivideByteRoutine(g, r.divisor);
        }
        Emit(g, Mn::RTS, Mode::Imp);
    }
}

} // namespace tisple
