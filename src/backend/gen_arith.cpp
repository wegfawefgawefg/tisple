#include "backend/gen.hpp"

namespace tisple {

namespace {

bool Commutes(Op op) {
    return op == Op::Add || op == Op::And || op == Op::Or || op == Op::Xor;
}

Mn AluMn(Op op) {
    switch (op) {
    case Op::Add:
        return Mn::ADC;
    case Op::Sub:
        return Mn::SBC;
    case Op::And:
        return Mn::AND;
    case Op::Or:
        return Mn::ORA;
    default:
        return Mn::EOR;
    }
}

// -1 unless byte k is a plain number
int ImmByte(const Opnd& o, int k) {
    const ByteRef& b = o.b[k];
    if (b.kind != ByteKind::Imm || b.value.sym >= 0) {
        return -1;
    }
    return b.value.offset & 0xFF;
}

// A op= o, for one byte; carry already set up for ADC/SBC
void AluByte(Gen& g, Op op, const Opnd& o, int k) {
    OpA(g, AluMn(op), o, k);
}

// A = A op b, 8-bit, skipping identities
void Alu8(Gen& g, Op op, const Opnd& b) {
    int v = ImmByte(b, 0);
    if (v == 0 && (op == Op::Add || op == Op::Sub || op == Op::Or || op == Op::Xor)) {
        return;
    }
    if (op == Op::And && v == 0xFF) {
        return;
    }
    if (op == Op::Add) {
        Emit(g, Mn::CLC, Mode::Imp);
    } else if (op == Op::Sub) {
        Emit(g, Mn::SEC, Mode::Imp);
    }
    AluByte(g, op, b, 0);
}

// multiplying by a constant with at most this many set bits is inlined as shifts and adds
constexpr int kMaxInlineMulBits = 4;

int Popcount(std::uint32_t v) {
    int n = 0;
    for (; v != 0; v &= v - 1) {
        n++;
    }
    return n;
}

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

// A = A * c, top-down shift and add with the multiplicand in a temporary
void MulConst8A(Gen& g, std::uint32_t c) {
    c &= 0xFF;
    if (c == 0) {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        return;
    }
    Opnd x;
    if (Popcount(c) > 1) {
        x = SpillA(g);
    }
    int top = 7;
    while (((c >> top) & 1) == 0) {
        top--;
    }
    for (int bit = top - 1; bit >= 0; bit--) {
        Emit(g, Mn::ASL, Mode::Acc);
        if ((c >> bit) & 1) {
            Emit(g, Mn::CLC, Mode::Imp);
            OpA(g, Mn::ADC, x, 0);
        }
    }
}

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

// operands for a two-operand routine: both readable after any later code
void Operands8(Gen& g, ExprId a, ExprId b, Opnd& oa, Opnd& ob) {
    oa = TrySimple(g, a);
    if (oa.size == 0 || (IsVolatile(oa) && HasWrites(*g.program, b))) {
        EvalA(g, a);
        oa = SpillA(g);
    }
    ob = TrySimple(g, b);
    if (ob.size == 0) {
        EvalA(g, b);
        ob = SpillA(g);
    }
}

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

Runtime DivRoutine(bool is_signed, bool wide) {
    if (wide) {
        return is_signed ? Runtime::DivS16 : Runtime::DivU16;
    }
    return is_signed ? Runtime::DivS8 : Runtime::DivU8;
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

} // namespace

void CallRuntime(Gen& g, Runtime routine) {
    g.be->rt_used[static_cast<int>(routine)] = true;
    Emit(g, Mn::JSR, Mode::Abs, SymValue(g.be->rt_sym[static_cast<int>(routine)]));
}

void EvalArith8(Gen& g, const Expr& e) {
    Op op = e.op;
    switch (op) {
    case Op::Add:
    case Op::Sub:
    case Op::And:
    case Op::Or:
    case Op::Xor: {
        Opnd b = TrySimple(g, e.b);
        if (b.size != 0) {
            EvalA(g, e.a);
            Alu8(g, op, b);
            return;
        }
        Opnd a = TrySimple(g, e.a);
        if (a.size != 0 && !HasWrites(*g.program, e.b) && Commutes(op)) {
            EvalA(g, e.b);
            Alu8(g, op, a);
            return;
        }
        EvalA(g, e.a);
        Opnd ta = SpillA(g);
        EvalA(g, e.b);
        if (Commutes(op)) {
            Alu8(g, op, ta);
            return;
        }
        Opnd tb = SpillA(g);
        LoadA(g, ta, 0);
        Alu8(g, op, tb);
        return;
    }
    case Op::Shl:
    case Op::Shr:
        Shift8(g, e);
        return;
    case Op::Mul: {
        std::uint16_t c = 0;
        if (ConstValue(*g.program, e.b, c) && Popcount(c & 0xFF) <= kMaxInlineMulBits) {
            EvalA(g, e.a);
            MulConst8A(g, c);
            return;
        }
        if (ConstValue(*g.program, e.a, c) && Popcount(c & 0xFF) <= kMaxInlineMulBits) {
            EvalA(g, e.b);
            MulConst8A(g, c);
            return;
        }
        Opnd a;
        Opnd b;
        Operands8(g, e.a, e.b, a, b);
        LoadA(g, b, 0);
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_b));
        LoadA(g, a, 0);
        CallRuntime(g, Runtime::Mul8);
        return;
    }
    case Op::Div:
    case Op::Mod: {
        std::uint16_t c = 0;
        int k = ConstValue(*g.program, e.b, c) ? PowerOfTwo(c & 0xFF) : -1;
        if (k >= 0 && !IsSignedKind(ExprKindOf(*g.program, e.a))) {
            // unsigned by 2^k: a shift or a mask
            EvalA(g, e.a);
            if (op == Op::Mod) {
                Emit(g, Mn::AND, Mode::Imm, Num((1 << k) - 1));
                return;
            }
            for (int i = 0; i < k; i++) {
                Emit(g, Mn::LSR, Mode::Acc);
            }
            return;
        }
        Opnd a;
        Opnd b;
        Operands8(g, e.a, e.b, a, b);
        LoadA(g, b, 0);
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_b));
        LoadA(g, a, 0);
        Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_a));
        CallRuntime(g, DivRoutine(IsSignedKind(ExprKindOf(*g.program, e.a)), false));
        if (op == Op::Mod) {
            Emit(g, Mn::LDA, Mode::Abs, SymValue(g.be->rt_r));
        }
        return;
    }
    default:
        Fail(g, e.loc, "internal: not an 8-bit arithmetic operator");
        return;
    }
}

void Combine16(Gen& g, Op op, const Opnd& a_in, const Opnd& b_in, const Opnd& dst) {
    if (CrossAliases(g, a_in, dst) || CrossAliases(g, b_in, dst)) {
        Opnd t = NewTemp(g, 2);
        Combine16(g, op, a_in, b_in, t);
        CopyOpnd(g, t, dst);
        return;
    }
    Opnd a = a_in;
    Opnd b = b_in;
    if (Commutes(op) && IsImm(a) && !IsImm(b)) {
        std::swap(a, b);
    }
    bool carry = false;
    for (int k = 0; k < 2; k++) {
        int v = ImmByte(b, k);
        switch (op) {
        case Op::Add:
        case Op::Sub:
            if (!carry && v == 0) {
                LoadA(g, a, k);
                break;
            }
            LoadA(g, a, k);
            if (!carry) {
                Emit(g, op == Op::Add ? Mn::CLC : Mn::SEC, Mode::Imp);
                carry = true;
            }
            AluByte(g, op, b, k);
            break;
        case Op::And:
            if (v == 0) {
                Emit(g, Mn::LDA, Mode::Imm, Num(0));
            } else {
                LoadA(g, a, k);
                if (v != 0xFF) {
                    AluByte(g, op, b, k);
                }
            }
            break;
        case Op::Or:
            if (v == 0xFF) {
                Emit(g, Mn::LDA, Mode::Imm, Num(0xFF));
            } else {
                LoadA(g, a, k);
                if (v != 0) {
                    AluByte(g, op, b, k);
                }
            }
            break;
        default:
            LoadA(g, a, k);
            if (v != 0) {
                AluByte(g, op, b, k);
            }
            break;
        }
        StoreA(g, dst, k);
    }
}

void MulConst16(Gen& g, const Opnd& a, std::uint32_t factor, const Opnd& dst) {
    factor &= 0xFFFF;
    if (factor == 0) {
        CopyOpnd(g, ImmOpnd(0, 2), dst);
        return;
    }
    // the product builds up in r, which shifts in place; x is read once per set bit
    bool plain = dst.b[0].kind == ByteKind::Mem && dst.b[1].kind == ByteKind::Mem;
    Opnd r = plain ? dst : NewTemp(g, 2);
    Opnd x = a;
    if (Popcount(factor) > 1 && !a.temp && !IsImm(a)) {
        x = NewTemp(g, 2);
        CopyOpnd(g, a, x);
    }
    CopyOpnd(g, x, r);
    int top = 15;
    while (((factor >> top) & 1) == 0) {
        top--;
    }
    for (int bit = top - 1; bit >= 0; bit--) {
        RmwByte(g, Mn::ASL, r, 0);
        RmwByte(g, Mn::ROL, r, 1);
        if ((factor >> bit) & 1) {
            Combine16(g, Op::Add, r, x, r);
        }
    }
    if (!plain) {
        CopyOpnd(g, r, dst);
    }
}

Opnd EvalArith16(Gen& g, const Expr& e, const Opnd* dst) {
    if (e.kind == ExprKind::PtrAdd || e.kind == ExprKind::PtrSub) {
        Opnd a = Eval(g, e.a, nullptr);
        if (HasWrites(*g.program, e.b)) {
            a = Snapshot(g, a);
        }
        Opnd b = Eval(g, e.b, nullptr);
        if (e.value != 1) {
            Opnd scaled = NewTemp(g, 2);
            MulConst16(g, b, e.value, scaled);
            b = scaled;
        }
        Opnd d = dst ? *dst : NewTemp(g, 2);
        Combine16(g, e.kind == ExprKind::PtrAdd ? Op::Add : Op::Sub, a, b, d);
        return d;
    }
    Op op = e.op;
    if (op == Op::Shl || op == Op::Shr) {
        Opnd a = Eval(g, e.a, nullptr);
        std::uint16_t c = 0;
        bool fixed = ConstValue(*g.program, e.b, c);
        Opnd count;
        if (!fixed) {
            if (HasWrites(*g.program, e.b)) {
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
    std::uint16_t c = 0;
    bool b_const = ConstValue(*g.program, e.b, c);
    bool is_signed = IsSignedKind(ExprKindOf(*g.program, e.a));
    if (op == Op::Mul && b_const && Popcount(c) <= kMaxInlineMulBits) {
        Opnd a = Eval(g, e.a, nullptr);
        Opnd d = dst ? *dst : NewTemp(g, 2);
        MulConst16(g, a, c, d);
        return d;
    }
    if (op == Op::Mul && ConstValue(*g.program, e.a, c) && Popcount(c) <= kMaxInlineMulBits) {
        Opnd b = Eval(g, e.b, nullptr);
        Opnd d = dst ? *dst : NewTemp(g, 2);
        MulConst16(g, b, c, d);
        return d;
    }
    if ((op == Op::Div || op == Op::Mod) && b_const && PowerOfTwo(c) >= 0 && !is_signed) {
        Opnd a = Eval(g, e.a, nullptr);
        if (op == Op::Mod) {
            Opnd d = dst ? *dst : NewTemp(g, 2);
            Combine16(g, Op::And, a, ImmOpnd(static_cast<std::uint16_t>(c - 1), 2), d);
            return d;
        }
        Opnd d = ShiftTarget(g, dst);
        Shift16(g, Op::Shr, false, a, d, true, static_cast<std::uint16_t>(PowerOfTwo(c)), Opnd());
        return d;
    }
    Opnd a = Eval(g, e.a, nullptr);
    if (HasWrites(*g.program, e.b)) {
        a = Snapshot(g, a);
    }
    Opnd b = Eval(g, e.b, nullptr);
    if (op == Op::Mul || op == Op::Div || op == Op::Mod) {
        Opnd rt_a = MemOpnd(SymValue(g.be->rt_a), 2, 1);
        Opnd rt_b = MemOpnd(SymValue(g.be->rt_b), 2, 1);
        CopyOpnd(g, a, rt_a);
        CopyOpnd(g, b, rt_b);
        Opnd d = dst ? *dst : NewTemp(g, 2);
        if (op == Op::Mul) {
            CallRuntime(g, Runtime::Mul16);
            StoreAX(g, d);
            return d;
        }
        CallRuntime(g, DivRoutine(IsSignedKind(ExprKindOf(*g.program, e.a)), true));
        CopyOpnd(g, op == Op::Div ? rt_a : MemOpnd(SymValue(g.be->rt_r), 2, 1), d);
        return d;
    }
    Opnd d = dst ? *dst : NewTemp(g, 2);
    Combine16(g, op, a, b, d);
    return d;
}

} // namespace tisple
