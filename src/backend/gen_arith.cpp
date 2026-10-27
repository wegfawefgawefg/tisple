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

// A op= o, for one byte; carry already set up for ADC/SBC
void AluByte(Gen& g, Op op, const Opnd& o, int k) {
    OpA(g, AluMn(op), o, k);
}

// A = A op b, 8-bit, skipping identities. no_carry: the sum is known to stay below 256, so
// the ADC leaves the carry clear (and the next one needs no CLC)
void Alu8(Gen& g, Op op, const Opnd& b, bool no_carry = false) {
    int v = KnownImm(g, b, 0);
    if (v == 0 && (op == Op::Add || op == Op::Sub || op == Op::Or || op == Op::Xor)) {
        return;
    }
    if (op == Op::And && v == 0xFF) {
        return;
    }
    if (op == Op::Add && !CarryClearNow(g)) {
        Emit(g, Mn::CLC, Mode::Imp);
    } else if (op == Op::Sub) {
        Emit(g, Mn::SEC, Mode::Imp);
    }
    AluByte(g, op, b, 0);
    if (op == Op::Add && no_carry) {
        g.out->back().carry = Carry::Clear;
    }
}

// the 8-bit sum of a and b stays below 256
bool SumFits(const Gen& g, ExprId a, ExprId b) {
    return MaxOf(g, a) + MaxOf(g, b) <= 0xFF;
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
    std::uint32_t sum = MaxOf(g, leaves[first]);
    for (std::size_t i = 0; i < leaves.size(); i++) {
        if (i == first) {
            continue;
        }
        sum += MaxOf(g, leaves[i]);
        Alu8(g, e.op, TrySimple(g, leaves[i]), e.op == Op::Add && sum <= 0xFF);
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
    if (KnownImm(g, a, 1) != 0 || !IsPlainMemory(a) || KnownImm(g, a, 0) >= 0) {
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

// (op first (at table i)) with i computed: i into X, then first into A while X keeps i, then
// OP table,X, instead of first into a temporary and OP temporary. i now runs before first, so
// neither may have effects; when first's code needs X, nothing is kept and this is false
bool IndexedSecond(Gen& g, Op op, ExprId first, ExprId second, bool fits) {
    const Program& program = *g.program;
    if (g.pin[0] >= 0 || ExprOf(program, second).kind != ExprKind::Index ||
        HasEffects(program, first) || HasEffects(program, second)) {
        return false;
    }
    std::size_t mark = g.out->size();
    Gen saved = g;
    bool outer = g.violation;
    g.violation = false;
    Opnd b = Eval(g, second, nullptr);
    bool indexed = b.size == 1 && b.b[0].kind == ByteKind::MemIdx && !b.index.is_reg;
    if (indexed) {
        LoadIndexReg(g, 0, b.index);
        // X is held like a loop counter's register: code that writes it sets violation
        g.pin[0] = static_cast<std::int32_t>(program.vars.size());
        EvalA(g, first);
        g.pin[0] = -1;
    }
    if (!indexed || (g.violation && !g.failed)) {
        g.out->resize(mark);
        g = saved;
        return false;
    }
    Alu8(g, op, b, fits);
    g.violation = outer;
    return true;
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
        bool fits = op == Op::Add && SumFits(g, e.a, e.b);
        Opnd b = TrySimple(g, e.b);
        if (b.size != 0) {
            EvalA(g, e.a);
            Alu8(g, op, b, fits);
            return;
        }
        Opnd a = TrySimple(g, e.a);
        if (a.size != 0 && !MayWriteOpnd(g, e.b, a) && Commutes(op)) {
            EvalA(g, e.b);
            Alu8(g, op, a, fits);
            return;
        }
        if (IndexedSecond(g, op, e.a, e.b, fits) ||
            (Commutes(op) && IndexedSecond(g, op, e.b, e.a, fits))) {
            return;
        }
        EvalA(g, e.a);
        Opnd ta = SpillA(g);
        EvalA(g, e.b);
        if (Commutes(op)) {
            Alu8(g, op, ta, fits);
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
        // A already holds b's byte (just stored or loaded): add a to it instead
        bool flip = Commutes(op) && AHoldsByte(g, b, k) && !IsImm(b);
        const Opnd& x = flip ? b : a;
        const Opnd& y = flip ? a : b;
        int v = KnownImm(g, y, k);
        switch (op) {
        case Op::Add:
        case Op::Sub:
            if (!carry && v == 0) {
                LoadA(g, x, k);
                break;
            }
            if (!carry && op == Op::Add && KnownImm(g, x, k) == 0) {
                LoadA(g, y, k);
                break;
            }
            LoadA(g, x, k);
            if (!carry) {
                Emit(g, op == Op::Add ? Mn::CLC : Mn::SEC, Mode::Imp);
                carry = true;
            }
            AluByte(g, op, y, k);
            break;
        case Op::And:
            if (v == 0) {
                Emit(g, Mn::LDA, Mode::Imm, Num(0));
            } else {
                LoadA(g, x, k);
                if (v != 0xFF) {
                    AluByte(g, op, y, k);
                }
            }
            break;
        case Op::Or:
            if (v == 0xFF) {
                Emit(g, Mn::LDA, Mode::Imm, Num(0xFF));
            } else {
                LoadA(g, x, k);
                if (v != 0) {
                    AluByte(g, op, y, k);
                }
            }
            break;
        default:
            LoadA(g, x, k);
            if (v != 0) {
                AluByte(g, op, y, k);
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
        if (e.kind == ExprKind::PtrAdd) {
            return Add16(g, a, b, dst);
        }
        Opnd d = dst ? *dst : NewTemp(g, 2);
        Combine16(g, Op::Sub, a, b, d);
        return d;
    }
    Op op = e.op;
    if (op == Op::Shl || op == Op::Shr) {
        return EvalShift16(g, e, dst);
    }
    std::uint16_t c = 0;
    bool b_const = ConstValue(*g.program, e.b, c);
    if (op == Op::Mul && b_const && MaxOf(g, e.a) * c <= 0xFF) {
        // the product fits a byte: work it out in A, the high byte is 0
        EvalA(g, e.a);
        MulConst8A(g, c);
        Opnd t = SpillA(g);
        t.size = 2;
        t.b[1] = ByteRef();
        t.b[1].value = Num(0);
        return t;
    }
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
    Opnd a = Eval(g, e.a, nullptr);
    if (MayWriteOpnd(g, e.b, a)) {
        a = Snapshot(g, a);
    }
    Opnd b = Eval(g, e.b, nullptr);
    if (op == Op::Mul) {
        // a byte times a byte is the whole product of two bytes; with one byte, it goes
        // second, where the routine skips its zero high byte
        bool a_byte = KnownImm(g, a, 1) == 0 || MaxOf(g, e.a) <= 0xFF;
        bool b_byte = KnownImm(g, b, 1) == 0 || MaxOf(g, e.b) <= 0xFF;
        if (a_byte && !b_byte) {
            std::swap(a, b);
            std::swap(a_byte, b_byte);
        }
        Opnd d = dst ? *dst : NewTemp(g, 2);
        if (a_byte && b_byte) {
            LoadA(g, b, 0);
            Emit(g, Mn::STA, Mode::Abs, SymValue(g.be->rt_b));
            LoadA(g, a, 0);
            CallRuntime(g, Runtime::Mul8x8);
            StoreAX(g, d);
            return d;
        }
        CopyOpnd(g, a, MemOpnd(SymValue(g.be->rt_a), 2, 1));
        CopyOpnd(g, b, MemOpnd(SymValue(g.be->rt_b), 2, 1));
        CallRuntime(g, Runtime::Mul16);
        StoreAX(g, d);
        return d;
    }
    if (op == Op::Add) {
        return Add16(g, a, b, dst);
    }
    Opnd d = dst ? *dst : NewTemp(g, 2);
    Combine16(g, op, a, b, d);
    return d;
}

} // namespace tisple
