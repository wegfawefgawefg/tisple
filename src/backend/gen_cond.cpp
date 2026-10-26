#include "backend/gen.hpp"

namespace tisple {

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

Op Negate(Op op) {
    switch (op) {
    case Op::Eq:
        return Op::Ne;
    case Op::Ne:
        return Op::Eq;
    case Op::Lt:
        return Op::Ge;
    case Op::Ge:
        return Op::Lt;
    case Op::Gt:
        return Op::Le;
    default:
        return Op::Gt;
    }
}

// a op b == b swapped(op) a
Op Swapped(Op op) {
    switch (op) {
    case Op::Lt:
        return Op::Gt;
    case Op::Gt:
        return Op::Lt;
    case Op::Le:
        return Op::Ge;
    case Op::Ge:
        return Op::Le;
    default:
        return op;
    }
}

bool IsImmZero(const Opnd& o) {
    for (int k = 0; k < o.size; k++) {
        const ByteRef& b = o.b[k];
        if (o.in_a || b.kind != ByteKind::Imm || b.value.sym >= 0 || (b.value.offset & 0xFF) != 0) {
            return false;
        }
    }
    return o.size != 0;
}

// the last instruction left N and Z describing A
bool FlagsFromA(const Gen& g) {
    if (g.out->empty()) {
        return false;
    }
    const AsmItem& item = g.out->back();
    if (item.kind != ItemKind::Ins) {
        return false;
    }
    switch (item.mn) {
    case Mn::LDA:
    case Mn::ADC:
    case Mn::SBC:
    case Mn::AND:
    case Mn::ORA:
    case Mn::EOR:
    case Mn::TXA:
    case Mn::TYA:
    case Mn::PLA:
        return true;
    case Mn::ASL:
    case Mn::LSR:
    case Mn::ROL:
    case Mn::ROR:
        return item.mode == Mode::Acc;
    default:
        return false;
    }
}

void EnsureFlagsFromA(Gen& g) {
    if (!FlagsFromA(g)) {
        Emit(g, Mn::CMP, Mode::Imm, Num(0));
    }
}

// N = (A < b) signed, after A - b
void SignedLess(Gen& g, const Opnd& b, int k, bool first) {
    if (first) {
        Emit(g, Mn::SEC, Mode::Imp);
    }
    OpA(g, Mn::SBC, b, k);
    std::int32_t skip = NewLabel(g);
    Branch(g, Mn::BVC, skip);
    Emit(g, Mn::EOR, Mode::Imm, Num(0x80));
    PlaceLabel(g, skip);
}

} // namespace

void CompareJump(Gen& g, Op op, bool is_signed, Opnd a, Opnd b, std::int32_t label) {
    if (op == Op::Gt || op == Op::Le) {
        std::swap(a, b);
        op = Swapped(op);
    }
    if (b.in_a) {
        b = SpillA(g);
    }
    bool lt = op == Op::Lt;
    if (a.size == 1) {
        if (op == Op::Eq || op == Op::Ne) {
            LoadA(g, a, 0);
            if (IsImmZero(b)) {
                EnsureFlagsFromA(g);
            } else {
                OpA(g, Mn::CMP, b, 0);
            }
            Branch(g, op == Op::Eq ? Mn::BEQ : Mn::BNE, label);
            return;
        }
        if (!is_signed) {
            if (IsImmZero(b)) {
                if (!lt) {
                    Jump(g, label);
                }
                return;
            }
            LoadA(g, a, 0);
            OpA(g, Mn::CMP, b, 0);
            Branch(g, lt ? Mn::BCC : Mn::BCS, label);
            return;
        }
        LoadA(g, a, 0);
        if (IsImmZero(b)) {
            EnsureFlagsFromA(g);
        } else {
            SignedLess(g, b, 0, true);
        }
        Branch(g, lt ? Mn::BMI : Mn::BPL, label);
        return;
    }
    if (op == Op::Eq || op == Op::Ne) {
        if (IsImmZero(b)) {
            LoadA(g, a, 0);
            OpA(g, Mn::ORA, a, 1);
            Branch(g, op == Op::Eq ? Mn::BEQ : Mn::BNE, label);
            return;
        }
        std::int32_t skip = NewLabel(g);
        LoadA(g, a, 0);
        OpA(g, Mn::CMP, b, 0);
        Branch(g, Mn::BNE, op == Op::Eq ? skip : label);
        LoadA(g, a, 1);
        OpA(g, Mn::CMP, b, 1);
        Branch(g, op == Op::Eq ? Mn::BEQ : Mn::BNE, label);
        PlaceLabel(g, skip);
        return;
    }
    if (!is_signed && IsImmZero(b)) {
        if (!lt) {
            Jump(g, label);
        }
        return;
    }
    if (is_signed && IsImmZero(b)) {
        LoadA(g, a, 1);
        EnsureFlagsFromA(g);
        Branch(g, lt ? Mn::BMI : Mn::BPL, label);
        return;
    }
    bool low_zero =
        b.b[0].kind == ByteKind::Imm && b.b[0].value.sym < 0 && (b.b[0].value.offset & 0xFF) == 0;
    if (!is_signed && low_zero) {
        // a >= hi * 256 exactly when a's high byte >= hi
        LoadA(g, a, 1);
        OpA(g, Mn::CMP, b, 1);
        Branch(g, lt ? Mn::BCC : Mn::BCS, label);
        return;
    }
    LoadA(g, a, 0);
    OpA(g, Mn::CMP, b, 0);
    LoadA(g, a, 1);
    if (is_signed) {
        SignedLess(g, b, 1, false);
        Branch(g, lt ? Mn::BMI : Mn::BPL, label);
        return;
    }
    OpA(g, Mn::SBC, b, 1);
    Branch(g, lt ? Mn::BCC : Mn::BCS, label);
}

namespace {

void GenCompare(Gen& g, const Expr& e, bool when, std::int32_t label) {
    Op op = when ? e.op : Negate(e.op);
    TypeKind kind = ExprKindOf(*g.program, e.a);
    bool is_signed = IsSignedKind(kind);
    std::uint32_t size = ExprSize(*g.program, e.a);
    std::uint16_t cb = 0;
    bool b_const = ConstValue(*g.program, e.b, cb);
    if (b_const && (op == Op::Gt || op == Op::Le)) {
        std::uint16_t max = is_signed ? (size == 1 ? 0x7F : 0x7FFF) : (size == 1 ? 0xFF : 0xFFFF);
        if (cb == max) {
            // a > max never holds, a <= max always does
            if (HasEffects(*g.program, e.a)) {
                Eval(g, e.a, nullptr);
            }
            if (op == Op::Le) {
                Jump(g, label);
            }
            return;
        }
        cb = static_cast<std::uint16_t>((cb + 1) & (size == 1 ? 0xFF : 0xFFFF));
        op = op == Op::Gt ? Op::Ge : Op::Lt;
    }
    if (size == 2) {
        Opnd a = Eval(g, e.a, nullptr);
        if (MayWriteOpnd(g, e.b, a)) {
            a = Snapshot(g, a);
        }
        Opnd b = b_const ? ImmOpnd(cb, 2) : Eval(g, e.b, nullptr);
        CompareJump(g, op, is_signed, a, b, label);
        return;
    }
    Opnd b = b_const ? ImmOpnd(cb, 1) : TrySimple(g, e.b);
    bool swaps = op == Op::Gt || op == Op::Le;
    if (b.size != 0) {
        Opnd a = TrySimple(g, e.a);
        if (a.size == 0) {
            EvalA(g, e.a);
            a = swaps ? SpillA(g) : InA();
        }
        CompareJump(g, op, is_signed, a, b, label);
        return;
    }
    Opnd a = TrySimple(g, e.a);
    if (a.size == 0 || MayWriteOpnd(g, e.b, a)) {
        EvalA(g, e.a);
        a = SpillA(g);
    }
    EvalA(g, e.b);
    if (swaps) {
        // b is in A already, which is what the swapped compare wants
        CompareJump(g, Swapped(op), is_signed, InA(), a, label);
        return;
    }
    b = SpillA(g);
    CompareJump(g, op, is_signed, a, b, label);
}

bool IsComparison(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt ||
           op == Op::Ge;
}

} // namespace

void GenBranch(Gen& g, ExprId id, bool when, std::int32_t label) {
    id = ForwardTarget(g, id);
    const Expr& e = At(g, id);
    std::uint16_t c = 0;
    if (ConstValue(*g.program, id, c)) {
        if ((c != 0) == when) {
            Jump(g, label);
        }
        return;
    }
    switch (e.kind) {
    case ExprKind::Unary:
        if (e.op == Op::Not) {
            GenBranch(g, e.a, !when, label);
            return;
        }
        break;
    case ExprKind::And:
    case ExprKind::Or: {
        bool is_and = e.kind == ExprKind::And;
        // and jumping on true, or jumping on false: both halves must agree
        if (is_and == when) {
            std::int32_t skip = NewLabel(g);
            GenBranch(g, e.a, !when, skip);
            GenBranch(g, e.b, when, label);
            PlaceLabel(g, skip);
            return;
        }
        GenBranch(g, e.a, when, label);
        GenBranch(g, e.b, when, label);
        return;
    }
    case ExprKind::Binary:
        if (IsComparison(e.op)) {
            GenCompare(g, e, when, label);
            return;
        }
        break;
    default:
        break;
    }
    EvalA(g, id);
    EnsureFlagsFromA(g);
    Branch(g, when ? Mn::BNE : Mn::BEQ, label);
}

void CondToA(Gen& g, ExprId id) {
    id = ForwardTarget(g, id);
    std::uint16_t c = 0;
    if (ConstValue(*g.program, id, c)) {
        Emit(g, Mn::LDA, Mode::Imm, Num(c != 0 ? 1 : 0));
        return;
    }
    const Expr& e = At(g, id);
    if (e.kind == ExprKind::Unary && e.op == Op::Not) {
        const Expr& inner = At(g, e.a);
        bool plain = inner.kind != ExprKind::Binary && inner.kind != ExprKind::And &&
                     inner.kind != ExprKind::Or && inner.kind != ExprKind::Unary;
        if (plain) {
            EvalA(g, e.a);
            Emit(g, Mn::EOR, Mode::Imm, Num(1));
            return;
        }
    }
    std::int32_t no = NewLabel(g);
    std::int32_t end = NewLabel(g);
    GenBranch(g, id, false, no);
    Emit(g, Mn::LDA, Mode::Imm, Num(1));
    Branch(g, Mn::BNE, end);
    PlaceLabel(g, no);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    PlaceLabel(g, end);
}

} // namespace tisple
