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

// a multiply by a routine: the shift-and-add one for now, with how often this copy of it may
// run (its loops' weights times its function's runs); the link switches every multiply to the
// quarter-square routines when one of them runs more than once (UpgradeMultiplies)
void CallMultiply(Gen& g, const Expr& e, Runtime routine) {
    std::size_t id = static_cast<std::size_t>(&e - g.program->exprs.data());
    std::uint64_t fn_runs = g.be->fn_runs[static_cast<std::size_t>(g.function)];
    // both at most 2^32: the product fits
    std::uint64_t runs =
        LoopWeight(g) *
        std::min<std::uint64_t>(std::max<std::uint64_t>(fn_runs, 1), std::uint64_t(1) << 31);
    g.multiplies.push_back({static_cast<ExprId>(id), runs});
    CallRuntime(g, routine);
}

} // namespace

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
        if (op == Op::Sub && a.size != 0 && !MayWriteOpnd(g, e.b, a)) {
            // a - b as ~b + 1 + a: b in A first, a read where it is
            EvalA(g, e.b);
            Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
            Emit(g, Mn::SEC, Mode::Imp);
            AluByte(g, Op::Add, a, 0);
            return;
        }
        if (op == Op::Sub && !HasEffects(*g.program, e.a) && !HasEffects(*g.program, e.b)) {
            // neither has effects, so b may go first: only b needs a temporary
            EvalA(g, e.b);
            Opnd tb = SpillA(g);
            EvalA(g, e.a);
            Alu8(g, op, tb);
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
        if (ConstValue(*g.program, e.b, c)) {
            EvalA(g, e.a);
            MulConst8A(g, c);
            return;
        }
        if (ConstValue(*g.program, e.a, c)) {
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
        CallMultiply(g, e, Runtime::Mul8);
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
    if (op == Op::Mul && b_const && InlineMulConst16(c)) {
        Opnd a = Eval(g, e.a, nullptr);
        Opnd d = dst ? *dst : NewTemp(g, 2);
        MulConst16(g, a, c, d);
        return d;
    }
    if (op == Op::Mul && ConstValue(*g.program, e.a, c) && InlineMulConst16(c)) {
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
            CallMultiply(g, e, Runtime::Mul8x8);
            StoreAX(g, d);
            return d;
        }
        CopyOpnd(g, a, MemOpnd(SymValue(g.be->rt_a), 2, 1));
        CopyOpnd(g, b, MemOpnd(SymValue(g.be->rt_b), 2, 1));
        CallMultiply(g, e, Runtime::Mul16);
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
