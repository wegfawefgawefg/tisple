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

// operands for a two-operand routine: both readable after any later code
void Operands8(Gen& g, ExprId a, ExprId b, Opnd& oa, Opnd& ob) {
    oa = TrySimple(g, a);
    if (oa.size == 0 || MayWriteOpnd(g, b, oa)) {
        EvalA(g, a);
        oa = SpillA(g);
    }
    ob = TrySimple(g, b);
    if (ob.size == 0) {
        EvalA(g, b);
        ob = SpillA(g);
    }
}

// the operands of a run of one associative 8-bit op: (+ (+ a b) (+ c d)) is a b c d
void ChainLeaves(Gen& g, Op op, ExprId id, std::vector<ExprId>& out) {
    const Expr& e = ExprOf(*g.program, id);
    if (e.kind == ExprKind::Binary && e.op == op && ExprSize(*g.program, id) == 1) {
        ChainLeaves(g, op, e.a, out);
        ChainLeaves(g, op, e.b, out);
        return;
    }
    out.push_back(id);
}

// a op b op c ... with nothing but reads in it, as one run through A: the leaf that needs
// code first, then every other leaf read straight from memory. The result is the same in
// any order (the op is associative and commutative at 8 bits) and no leaf writes anything.
bool EvalChain8(Gen& g, const Expr& e) {
    if (!Commutes(e.op)) {
        return false;
    }
    std::vector<ExprId> leaves;
    ChainLeaves(g, e.op, e.a, leaves);
    ChainLeaves(g, e.op, e.b, leaves);
    if (leaves.size() < 3) {
        return false;
    }
    std::size_t first = leaves.size();
    for (std::size_t i = 0; i < leaves.size(); i++) {
        if (HasEffects(*g.program, leaves[i])) {
            return false;
        }
        if (TrySimple(g, leaves[i]).size == 0) {
            if (first < leaves.size()) {
                return false;
            }
            first = i;
        }
    }
    if (first == leaves.size()) {
        first = 0;
    }
    EvalA(g, leaves[first]);
    for (std::size_t i = 0; i < leaves.size(); i++) {
        if (i != first) {
            Alu8(g, e.op, TrySimple(g, leaves[i]));
        }
    }
    return true;
}

// every byte a number or a plain memory byte, which only a write to it changes
bool IsPlainMemory(const Opnd& o) {
    if (o.in_a || o.size == 0) {
        return false;
    }
    for (int k = 0; k < o.size; k++) {
        if (o.b[k].kind != ByteKind::Imm && o.b[k].kind != ByteKind::Mem) {
            return false;
        }
    }
    return true;
}

// a byte (a's high byte a known 0) times a constant, mod 65536: the low byte shifts in A,
// the high byte in memory, and each set bit adds the byte with a carry into the high byte
bool MulByteConst16(Gen& g, const Opnd& a, std::uint32_t factor, const Opnd& dst) {
    if (ImmByte(a, 1) != 0 || !IsPlainMemory(a) || ImmByte(a, 0) >= 0) {
        return false;
    }
    Opnd high = dst;
    high.size = 1;
    high.b[0] = dst.b[1];
    if (dst.b[1].kind != ByteKind::Mem || Overlaps(g, a, dst)) {
        high = NewTemp(g, 1);
    }
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    StoreA(g, high, 0);
    LoadA(g, a, 0);
    int top = 15;
    while (((factor >> top) & 1) == 0) {
        top--;
    }
    for (int bit = top - 1; bit >= 0; bit--) {
        Emit(g, Mn::ASL, Mode::Acc);
        RmwByte(g, Mn::ROL, high, 0);
        if ((factor >> bit) & 1) {
            std::int32_t skip = NewLabel(g);
            Emit(g, Mn::CLC, Mode::Imp);
            OpA(g, Mn::ADC, a, 0);
            Branch(g, Mn::BCC, skip);
            RmwByte(g, Mn::INC, high, 0);
            PlaceLabel(g, skip);
        }
    }
    StoreA(g, dst, 0);
    if (high.b[0].kind != dst.b[1].kind || high.b[0].value.sym != dst.b[1].value.sym ||
        high.b[0].value.offset != dst.b[1].value.offset) {
        LoadA(g, high, 0);
        StoreA(g, dst, 1);
    }
    return true;
}

Runtime DivRoutine(bool is_signed, bool wide) {

    if (wide) {
        return is_signed ? Runtime::DivS16 : Runtime::DivU16;
    }
    return is_signed ? Runtime::DivS8 : Runtime::DivU8;
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
        if (EvalChain8(g, e)) {
            return;
        }
        Opnd b = TrySimple(g, e.b);
        if (b.size != 0) {
            EvalA(g, e.a);
            Alu8(g, op, b);
            return;
        }
        Opnd a = TrySimple(g, e.a);
        if (a.size != 0 && !MayWriteOpnd(g, e.b, a) && Commutes(op)) {
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
        EvalShift8(g, e);
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
    if (MulByteConst16(g, a, factor, dst)) {
        return;
    }
    // the product builds up in r, which shifts in place; x is read once per set bit
    bool plain = dst.b[0].kind == ByteKind::Mem && dst.b[1].kind == ByteKind::Mem;
    Opnd r = plain ? dst : NewTemp(g, 2);
    Opnd x = a;
    bool stable = IsPlainMemory(a) && !Overlaps(g, a, r);
    if (Popcount(factor) > 1 && !a.temp && !IsImm(a) && !stable) {
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
        if (MayWriteOpnd(g, e.b, a)) {
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
        return EvalShift16(g, e, dst);
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
        return ShiftConst16(g, Op::Shr, false, a, static_cast<std::uint16_t>(PowerOfTwo(c)), dst);
    }
    Opnd a = Eval(g, e.a, nullptr);
    if (MayWriteOpnd(g, e.b, a)) {
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
