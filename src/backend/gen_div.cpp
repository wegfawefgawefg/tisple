#include "backend/gen_div.hpp"

#include "check/arith.hpp"

// Division and remainder.
//
// - Unsigned by a power of two: a shift or a mask.
// - Unsigned 8-bit by a constant of 128 or more: the quotient is 0 or 1, one compare.
// - Unsigned by another constant below 128: long division with the divisor as an immediate
//   (gen_div_const.cpp).
// - Signed by a constant: 1 and -1 need no division; a divisor whose magnitude is below 128
//   or a power of two divides the dividend's magnitude in a routine of its own
//   (gen_div_const.cpp).
// - Everything else calls the runtime (rt_a / rt_b: quotient in rt_a, remainder in rt_r;
//   runtime_div.cpp), the divisor in rt_b even when it is a constant.
//
// A / and a % of the same operands divide once: the last division's operands are kept
// (Gen::div), and a later division of the same operands reads rt_a or rt_r, as long as no
// label, call or write to them came in between and no statement since may have changed
// what the operands read (ForgetDivision, after every statement).

namespace tisple {

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

namespace {

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

// The operands that need code are worked out straight into rt_a and rt_b, not into
// temporaries copied there: the divisor always (nothing runs between it and the call but the
// dividend's load), the dividend when the divisor cannot call a routine that sets rt_a.
// Unsigned, a dividend below the divisor needs no call (the quotient is 0, the remainder the
// dividend): the compare is made here, and the routine entered past its own (rt.divu8.ge).
void RuntimeDiv8(Gen& g, ExprId id, const Expr& e, bool is_signed) {
    AsmValue rt_a = SymValue(g.be->rt_a);
    Opnd a = TrySimple(g, e.a);
    bool a_placed = false;
    if (a.size == 0 || MayWriteOpnd(g, e.b, a)) {
        EvalA(g, e.a);
        if (UsesRuntime(*g.program, e.b, g.be->aliases)) {
            a = SpillA(g);
        } else {
            Emit(g, Mn::STA, Mode::Abs, rt_a);
            a_placed = true;
        }
    }
    Opnd b = TrySimple(g, e.b);
    if (b.size == 0) {
        EvalA(g, e.b);
    } else {
        LoadA(g, b, 0);
    }
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_b));
    if (!a_placed) {
        LoadA(g, a, 0);
        Emit(g, Mn::STA, Mode::Abs, rt_a);
    }
    if (is_signed) {
        CallRuntime(g, Runtime::DivS8);
        Remember(g, e, true, false);
        if (e.op == Op::Mod) {
            Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_r));
        }
        return;
    }
    if (a_placed) {
        Emit(g, Mn::LDA, Mode::Abs, rt_a);
    }
    Emit(g, Mn::CMP, Mode::Abs, SymValue(g.be->rt_b));
    std::int32_t done = NewLabel(g);
    // a later division of the same operands reads the results where the routine leaves them:
    // then the way past the call leaves them there too
    bool pair = g.be->div_pairs[static_cast<std::size_t>(id)];
    if (!pair && e.op == Op::Mod) {
        Branch(g, Mn::BCC, done);
        Emit(g, Mn::JSR, Mode::Abs, SymValue(g.be->divu8_ge));
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_r));
        PlaceLabel(g, done);
        return;
    }
    std::int32_t divide = NewLabel(g);
    Branch(g, Mn::BCS, divide);
    if (pair) {
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_r));
        Emit(g, Mn::LDX, Mode::Imm, Num(0));
        Emit(g, Mn::STX, Mode::Abs, rt_a);
        if (e.op == Op::Div) {
            Emit(g, Mn::TXA, Mode::Imp);
        }
    } else {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
    }
    // the 0 just loaded set Z
    Branch(g, Mn::BEQ, done);
    PlaceLabel(g, divide);
    Emit(g, Mn::JSR, Mode::Abs, SymValue(g.be->divu8_ge));
    if (e.op == Op::Mod) {
        Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_r));
    }
    PlaceLabel(g, done);
    if (pair) {
        Remember(g, e, true, false);
    }
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

// unsigned, when no later division reads the results: a dividend below the divisor is quotient
// 0 and remainder the dividend without a call; else rt.divu16 past its own compare
Opnd InlineBelow16(Gen& g, const Expr& e, const Opnd* dst) {
    AsmValue a = SymValue(g.be->rt_a);
    AsmValue b = SymValue(g.be->rt_b);
    std::int32_t below = NewLabel(g);
    std::int32_t divide = NewLabel(g);
    std::int32_t done = NewLabel(g);
    Emit(g, Mn::LDA, Mode::Abs, SymValue(a.sym, 1));
    Emit(g, Mn::CMP, Mode::Abs, SymValue(b.sym, 1));
    Branch(g, Mn::BCC, below);
    Branch(g, Mn::BNE, divide);
    Emit(g, Mn::LDA, Mode::Abs, a);
    Emit(g, Mn::CMP, Mode::Abs, b);
    Branch(g, Mn::BCS, divide);
    PlaceLabel(g, below);
    Opnd d = dst ? *dst : NewTemp(g, 2);
    if (e.op == Op::Div) {
        CopyOpnd(g, ImmOpnd(0, 2), d);
    } else {
        CopyOpnd(g, MemOpnd(a, 2, 1), d);
    }
    Jump(g, done);
    PlaceLabel(g, divide);
    Emit(g, Mn::JSR, Mode::Abs, SymValue(g.be->divu16_ge));
    CopyOpnd(g, MemOpnd(e.op == Op::Div ? a : SymValue(g.be->rt_r), 2, 1), d);
    PlaceLabel(g, done);
    return d;
}

// the dividend is a (EvalDiv16 worked it out into rt_a when neither side can call a
// routine); the divisor goes straight into rt_b when it cannot either
Opnd RuntimeDiv16(Gen& g, ExprId id, const Expr& e, bool is_signed, Opnd a, const Opnd* dst) {
    if (MayWriteOpnd(g, e.b, a)) {
        a = Snapshot(g, a);
    }
    Opnd rt_a = MemOpnd(SymValue(g.be->rt_a), 2, 1);
    Opnd rt_b = MemOpnd(SymValue(g.be->rt_b), 2, 1);
    Opnd rt_r = MemOpnd(SymValue(g.be->rt_r), 2, 1);
    if (UsesRuntime(*g.program, e.b, g.be->aliases)) {
        Opnd b = Eval(g, e.b, nullptr);
        CopyOpnd(g, a, rt_a);
        CopyOpnd(g, b, rt_b);
    } else {
        EvalInto(g, e.b, rt_b);
        CopyOpnd(g, a, rt_a);
    }
    if (!is_signed && !g.be->div_pairs[static_cast<std::size_t>(id)]) {
        return InlineBelow16(g, e, dst);
    }
    CallRuntime(g, is_signed ? Runtime::DivS16 : Runtime::DivU16);
    Remember(g, e, true, false);
    Opnd d = dst ? *dst : NewTemp(g, 2);
    CopyOpnd(g, e.op == Op::Div ? rt_a : rt_r, d);
    return d;
}

} // namespace

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

bool KeepRemainder(Gen& g, ExprId id) {
    if (!g.be->div_pairs[static_cast<std::size_t>(id)]) {
        return false;
    }
    Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_r));
    return true;
}

Opnd QuotientOut(Gen& g, const Opnd* dst) {
    Opnd d = dst ? *dst : NewTemp(g, 2);
    CopyOpnd(g, MemOpnd(SymValue(g.be->rt_a), 2, 1), d);
    return d;
}

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

void PlaceLocalLabel(Gen& g, std::int32_t label) {
    g.out->push_back(MakeLabel(label));
}

void EvalDiv8(Gen& g, ExprId id, const Expr& e) {
    TypeKind kind = ExprKindOf(*g.program, e.a);
    bool is_signed = IsSignedKind(kind);
    std::uint16_t c16 = 0;
    bool fixed = ConstOf(g, e.b, c16);
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
    if (fixed && is_signed && DivSignedConst8(g, id, e, SignedValue(kind, c16))) {
        return;
    }
    if (fixed && !is_signed && c >= 128) {
        EvalA(g, e.a);
        DivLarge8(g, e.op, c);
        return;
    }
    if (!fixed || is_signed || !IsSmallDivisor(c)) {
        RuntimeDiv8(g, id, e, is_signed);
        return;
    }
    EvalA(g, e.a);
    DivSmallConst8(g, id, e, c);
}

Opnd EvalDiv16(Gen& g, ExprId id, const Expr& e, const Opnd* dst) {
    TypeKind kind = ExprKindOf(*g.program, e.a);
    bool is_signed = IsSignedKind(kind);
    std::uint16_t c = 0;
    bool fixed = ConstOf(g, e.b, c);
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
    Opnd out;
    if (fixed && is_signed && DivSignedConst16(g, id, e, SignedValue(kind, c), dst, out)) {
        return out;
    }
    if (!fixed || is_signed || !IsSmallDivisor(c)) {
        Opnd rt_a = MemOpnd(SymValue(g.be->rt_a), 2, 1);
        // a dividend that calls a routine itself may set rt_a before it is done
        bool direct = !UsesRuntime(*g.program, e.a, g.be->aliases) &&
                      !UsesRuntime(*g.program, e.b, g.be->aliases);
        Opnd a = Eval(g, e.a, direct ? &rt_a : nullptr);
        return RuntimeDiv16(g, id, e, is_signed, a, dst);
    }
    Opnd a = Eval(g, e.a, nullptr);
    return DivSmallConst16(g, id, e, a, c, dst);
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
