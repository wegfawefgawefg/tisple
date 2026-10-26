#include "backend/gen.hpp"

#include <string>

// Division and remainder.
//
// - Unsigned by a power of two: a shift or a mask.
// - Unsigned 8-bit by a constant of 128 or more: the quotient is 0 or 1, one compare.
// - Unsigned by another constant below 128: long division, one step per dividend bit with
//   the divisor as an immediate, unrolled. The remainder stays below 128, so it lives in A
//   and doubling it never carries out; the first steps, before the remainder can reach the
//   divisor, are just shifts. A 16-bit dividend is its high byte divided first (nothing to
//   do when it is below the divisor), then the low byte with that remainder. The quotient
//   builds up in rt_a, the remainder ends in A. Inside a loop the steps are inline; else
//   the program has one routine per divisor and width (rt.divu8_7, rt.divu16_7).
// - Everything else calls the runtime (rt_a / rt_b: quotient in rt_a, remainder in rt_r).
//
// A / and a % of the same operands divide once: the last division's operands are kept
// (Gen::div), and a later division of the same operands reads rt_a or rt_r, as long as no
// label, call or write to them came in between and no statement since may have changed
// what the operands read (ForgetDivision, after every statement).

namespace tisple {

namespace {

// log2 of a power of two, else -1
int PowerOfTwo(std::uint32_t v) {
    if (v == 0 || (v & (v - 1)) != 0) {
        return -1;
    }
    int k = 0;
    while ((v >> k) != 1) {
        k++;
    }
    return k;
}

int BitLength(std::uint32_t v) {
    int n = 0;
    for (; v != 0; v >>= 1) {
        n++;
    }
    return n;
}

bool IsDivision(const Expr& e) {
    return e.kind == ExprKind::Binary && (e.op == Op::Div || e.op == Op::Mod);
}

// long division handles this divisor (unsigned, not a power of two)
bool IsSmallDivisor(std::uint32_t c) {
    return c >= 3 && c < 128 && PowerOfTwo(c) < 0;
}

// did items from `at` on touch the last division's results: a label (another way in), a
// call (the runtime keeps its own values there) or any write to rt_a or rt_r
bool ResultsTouched(const Gen& g, std::size_t at) {
    for (std::size_t i = at; i < g.out->size(); i++) {
        const AsmItem& item = (*g.out)[i];
        if (item.kind == ItemKind::Label) {
            return true;
        }
        if (item.kind != ItemKind::Ins) {
            continue;
        }
        if (item.mn == Mn::JSR) {
            return true;
        }
        bool result = item.value.sym == g.be->rt_a || item.value.sym == g.be->rt_r;
        bool read = item.mn == Mn::LDA || item.mn == Mn::LDX || item.mn == Mn::LDY;
        if (result && !read) {
            return true;
        }
    }
    return false;
}

// the last division had these operands and its results are still there
bool CanReuse(const Gen& g, const Expr& e) {
    const DivDone& d = g.div;
    if (!d.valid || !SameExpr(*g.program, d.a, e.a) || !SameExpr(*g.program, d.b, e.b)) {
        return false;
    }
    if (e.op == Op::Mod && !d.have_rem) {
        return false;
    }
    return !ResultsTouched(g, d.at);
}

void Remember(Gen& g, const Expr& e, bool have_rem, bool rem_byte) {
    if (HasEffects(*g.program, e.a) || HasEffects(*g.program, e.b)) {
        g.div.valid = false;
        return;
    }
    g.div.valid = true;
    g.div.a = e.a;
    g.div.b = e.b;
    g.div.at = g.out->size();
    g.div.have_rem = have_rem;
    g.div.rem_byte = rem_byte;
}

// the remainder in A into rt_r when another division of these operands may want it
bool KeepRemainder(Gen& g, ExprId id) {
    if (!g.be->div_pairs[static_cast<std::size_t>(id)]) {
        return false;
    }
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_r));
    return true;
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

// the 16-bit dividend at rt_a divided by c, as a routine: quotient in rt_a, remainder in A.
// The high byte goes first; when it is below c it is the remainder so far and the
// quotient's high byte is 0.
void DivideWordRoutine(Gen& g, std::uint32_t c) {
    AsmValue q = SymValue(g.be->rt_a);
    AsmValue q_high = SymValue(g.be->rt_a, 1);
    std::int32_t low_half = NewLabel(g);
    std::int32_t full = NewLabel(g);
    Emit(g, Mn::LDA, Mode::Abs, q_high);
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

// the same inline, reading the dividend from a, leaving X and Y alone
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

// the shared routine for divisor c, made on first use
std::int32_t DivRoutineSym(Gen& g, std::uint32_t c, bool wide) {
    for (const DivRoutine& r : g.be->div_routines) {
        if (r.divisor == c && r.wide == wide) {
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

// A = A / c or A % c, unsigned, c of 128 or more and not a power of two: the quotient is 0
// or 1
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

void RuntimeDiv8(Gen& g, const Expr& e, bool is_signed) {
    Opnd a = TrySimple(g, e.a);
    if (a.size == 0 || MayWriteOpnd(g, e.b, a)) {
        EvalA(g, e.a);
        a = SpillA(g);
    }
    Opnd b = TrySimple(g, e.b);
    if (b.size == 0) {
        EvalA(g, e.b);
        b = SpillA(g);
    }
    LoadA(g, b, 0);
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_b));
    LoadA(g, a, 0);
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_a));
    CallRuntime(g, is_signed ? Runtime::DivS8 : Runtime::DivU8);
    Remember(g, e, true, false);
    if (e.op == Op::Mod) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_r));
    }
}

// the quotient at rt_a into dst or a temporary
Opnd QuotientOut(Gen& g, const Opnd* dst) {
    Opnd d = dst ? *dst : NewTemp(g, 2);
    CopyOpnd(g, MemOpnd(SymValue(g.be->rt_a), 2, 1), d);
    return d;
}

// a remainder below 256 in A, as a 16-bit value with a zero high byte
Opnd ByteRemainderOut(Gen& g, const Opnd* dst) {
    if (!dst) {
        Opnd t = NewTemp(g, 1);
        StoreA(g, t, 0);
        t.size = 2;
        t.b[1] = ByteRef();
        t.b[1].value = Num(0);
        return t;
    }
    StoreA(g, *dst, 0);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    StoreA(g, *dst, 1);
    return *dst;
}

Opnd ReuseDiv16(Gen& g, const Expr& e, const Opnd* dst) {
    if (e.op == Op::Div) {
        return QuotientOut(g, dst);
    }
    if (g.div.rem_byte) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_r));
        return ByteRemainderOut(g, dst);
    }
    Opnd d = dst ? *dst : NewTemp(g, 2);
    CopyOpnd(g, MemOpnd(SymValue(g.be->rt_r), 2, 1), d);
    return d;
}

Opnd RuntimeDiv16(Gen& g, const Expr& e, bool is_signed, Opnd a, const Opnd* dst) {
    if (MayWriteOpnd(g, e.b, a)) {
        a = Snapshot(g, a);
    }
    Opnd b = Eval(g, e.b, nullptr);
    Opnd rt_a = MemOpnd(SymValue(g.be->rt_a), 2, 1);
    Opnd rt_r = MemOpnd(SymValue(g.be->rt_r), 2, 1);
    CopyOpnd(g, a, rt_a);
    CopyOpnd(g, b, MemOpnd(SymValue(g.be->rt_b), 2, 1));
    CallRuntime(g, is_signed ? Runtime::DivS16 : Runtime::DivU16);
    Remember(g, e, true, false);
    Opnd d = dst ? *dst : NewTemp(g, 2);
    CopyOpnd(g, e.op == Op::Div ? rt_a : rt_r, d);
    return d;
}

} // namespace

void PlaceLocalLabel(Gen& g, std::int32_t label) {
    g.out->push_back(MakeLabel(label));
}

void EvalDiv8(Gen& g, ExprId id, const Expr& e) {
    bool is_signed = IsSignedKind(ExprKindOf(*g.program, e.a));
    std::uint16_t c16 = 0;
    bool fixed = ConstValue(*g.program, e.b, c16);
    std::uint32_t c = c16 & 0xFF;
    int k = fixed ? PowerOfTwo(c) : -1;
    if (k >= 0 && !is_signed) {
        EvalA(g, e.a);
        if (e.op == Op::Mod) {
            Emit(g, Mn::AND, Mode::Imm, Num((1 << k) - 1));
            return;
        }
        for (int i = 0; i < k; i++) {
            Emit(g, Mn::LSR, Mode::Acc);
        }
        return;
    }
    if (CanReuse(g, e)) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(e.op == Op::Div ? g.be->rt_a : g.be->rt_r));
        return;
    }
    if (fixed && !is_signed && c >= 128) {
        EvalA(g, e.a);
        DivLarge8(g, e.op, c);
        return;
    }
    if (!fixed || is_signed || !IsSmallDivisor(c)) {
        RuntimeDiv8(g, e, is_signed);
        return;
    }
    EvalA(g, e.a);
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

Opnd EvalDiv16(Gen& g, ExprId id, const Expr& e, const Opnd* dst) {
    bool is_signed = IsSignedKind(ExprKindOf(*g.program, e.a));
    std::uint16_t c = 0;
    bool fixed = ConstValue(*g.program, e.b, c);
    if (fixed && PowerOfTwo(c) >= 0 && !is_signed) {
        Opnd a = Eval(g, e.a, nullptr);
        if (e.op == Op::Mod) {
            Opnd d = dst ? *dst : NewTemp(g, 2);
            Combine16(g, Op::And, a, ImmOpnd(static_cast<std::uint16_t>(c - 1), 2), d);
            return d;
        }
        return ShiftConst16(g, Op::Shr, false, a, static_cast<std::uint16_t>(PowerOfTwo(c)), dst);
    }
    if (CanReuse(g, e)) {
        return ReuseDiv16(g, e, dst);
    }
    Opnd a = Eval(g, e.a, nullptr);
    if (!fixed || is_signed || !IsSmallDivisor(c)) {
        return RuntimeDiv16(g, e, is_signed, a, dst);
    }
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

void EmitDivRoutines(Backend& be, std::vector<AsmItem>& out) {
    Gen g;
    g.be = &be;
    g.program = be.program;
    g.out = &out;
    for (std::size_t i = 0; i < be.div_routines.size(); i++) {
        DivRoutine r = be.div_routines[i];
        PlaceLocalLabel(g, r.sym);
        if (r.wide) {
            DivideWordRoutine(g, r.divisor);
        } else {
            DivideByteInA(g, r.divisor);
        }
        Emit(g, Mn::RTS, Mode::Imp);
    }
}

void ForgetDivision(Gen& g, ExprId stmt) {
    if (!g.div.valid) {
        return;
    }
    const Program& program = *g.program;
    for (ExprId operand : {g.div.a, g.div.b}) {
        if (HasLoads(program, operand) && HasWrites(program, stmt)) {
            g.div.valid = false;
            return;
        }
        for (std::size_t v = 0; v < program.vars.size(); v++) {
            std::int32_t var = static_cast<std::int32_t>(v);
            if (ReadsVar(program, operand, var) && MayWriteVarIn(g, stmt, var)) {
                g.div.valid = false;
                return;
            }
        }
    }
}

std::vector<bool> FindDivPairs(const Program& program) {
    std::vector<bool> out(program.exprs.size(), false);
    std::vector<ExprId> divisions;
    for (std::size_t i = 0; i < program.exprs.size(); i++) {
        if (IsDivision(program.exprs[i])) {
            divisions.push_back(static_cast<ExprId>(i));
        }
    }
    for (std::size_t i = 0; i < divisions.size(); i++) {
        const Expr& x = ExprOf(program, divisions[i]);
        for (std::size_t j = i + 1; j < divisions.size(); j++) {
            const Expr& y = ExprOf(program, divisions[j]);
            if (SameExpr(program, x.a, y.a) && SameExpr(program, x.b, y.b)) {
                out[static_cast<std::size_t>(divisions[i])] = true;
                out[static_cast<std::size_t>(divisions[j])] = true;
            }
        }
    }
    return out;
}

} // namespace tisple
