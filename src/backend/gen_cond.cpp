#include "backend/gen.hpp"

#include <vector>

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

// the last instruction left N and Z describing index register r
bool FlagsFromReg(const Gen& g, int r) {
    if (g.out->empty()) {
        return false;
    }
    const AsmItem& item = g.out->back();
    if (item.kind != ItemKind::Ins) {
        return false;
    }
    if (r == 0) {
        return item.mn == Mn::INX || item.mn == Mn::DEX || item.mn == Mn::LDX || item.mn == Mn::TAX;
    }
    return item.mn == Mn::INY || item.mn == Mn::DEY || item.mn == Mn::LDY || item.mn == Mn::TAY;
}

// a byte in a pinned register against a number or a memory byte: CPX or CPY, A untouched.
// False if the compare is not one of those.
bool RegCompare(Gen& g, Op op, bool is_signed, const Opnd& a, const Opnd& b, std::int32_t label) {
    if (a.in_a || b.in_a || a.b[0].kind != ByteKind::Reg) {
        return false;
    }
    ByteKind kind = b.b[0].kind;
    bool equality = op == Op::Eq || op == Op::Ne;
    if ((kind != ByteKind::Imm && kind != ByteKind::Mem) || (is_signed && !equality)) {
        return false;
    }
    int r = a.b[0].reg;
    if (!(equality && IsImmZero(b) && FlagsFromReg(g, r))) {
        Emit(g, r == 0 ? Mn::CPX : Mn::CPY, kind == ByteKind::Imm ? Mode::Imm : Mode::Abs,
             b.b[0].value);
    }
    Mn mn = op == Op::Eq ? Mn::BEQ : op == Op::Ne ? Mn::BNE : op == Op::Lt ? Mn::BCC : Mn::BCS;
    Branch(g, mn, label);
    return true;
}

// a signed 16-bit a < (high, low) (or >= when !lt) by bytes, the high byte first: the high
// bytes decide unless they are equal, and only then the low bytes do, unsigned. The high
// bytes compare signed as unsigned with their top bits flipped; against 0, the high byte's
// sign decides at once
void SignedConstCompare(Gen& g, bool lt, const Opnd& a, int low, int high, std::int32_t label) {
    std::int32_t skip = NewLabel(g);
    std::int32_t less = lt ? label : skip;
    std::int32_t more = lt ? skip : label;
    LoadA(g, a, 1);
    if (high == 0) {
        Branch(g, Mn::BMI, less);
        Branch(g, Mn::BNE, more);
    } else {
        Emit(g, Mn::EOR, Mode::Imm, Num(0x80));
        Emit(g, Mn::CMP, Mode::Imm, Num(high ^ 0x80));
        Branch(g, Mn::BCC, less);
        Branch(g, Mn::BNE, more);
    }
    if (low == 0) {
        // equal high bytes: a >= high * 256
        if (!lt) {
            Jump(g, label);
        }
        PlaceLabel(g, skip);
        return;
    }
    LoadA(g, a, 0);
    Emit(g, Mn::CMP, Mode::Imm, Num(low));
    Branch(g, lt ? Mn::BCC : Mn::BCS, label);
    PlaceLabel(g, skip);
}

} // namespace

// a < b (or a >= b when !lt) of 16-bit values, the high bytes first, for when A holds a's
// high byte already: they decide unless they are equal, and then the low bytes do, unsigned.
// Unsigned, the carry of either compare decides. Signed, high bytes of the same sign compare
// as unsigned ones do, and of different signs the other way round.
void HighFirstCompare(Gen& g, bool lt, bool is_signed, const Opnd& a, const Opnd& b,
                      std::int32_t label) {
    Mn yes = lt ? Mn::BCC : Mn::BCS;
    Mn no = lt ? Mn::BCS : Mn::BCC;
    std::int32_t low = NewLabel(g);
    std::int32_t done = NewLabel(g);
    LoadA(g, a, 1);
    OpA(g, Mn::CMP, b, 1);
    if (!is_signed) {
        std::int32_t decide = NewLabel(g);
        Branch(g, Mn::BNE, decide);
        LoadA(g, a, 0);
        OpA(g, Mn::CMP, b, 0);
        PlaceLabel(g, decide);
        Branch(g, yes, label);
        return;
    }
    std::int32_t differ = NewLabel(g);
    Branch(g, Mn::BEQ, low);
    // N: the signs differ (EOR keeps the carry)
    OpA(g, Mn::EOR, b, 1);
    Branch(g, Mn::BMI, differ);
    Branch(g, yes, label);
    Branch(g, no, done);
    PlaceLabel(g, differ);
    Branch(g, no, label);
    Branch(g, yes, done);
    PlaceLabel(g, low);
    LoadA(g, a, 0);
    OpA(g, Mn::CMP, b, 0);
    Branch(g, yes, label);
    PlaceLabel(g, done);
}

void CompareJump(Gen& g, Op op, bool is_signed, Opnd a, Opnd b, std::int32_t label) {
    if (op == Op::Gt || op == Op::Le) {
        std::swap(a, b);
        op = Swapped(op);
    }
    if (b.in_a) {
        b = SpillA(g);
    }
    bool lt = op == Op::Lt;
    // (unsigned compares with 0 that always or never hold are below)
    bool trivial = !is_signed && !(op == Op::Eq || op == Op::Ne) && IsImmZero(b);
    if (a.size == 1 && !trivial && RegCompare(g, op, is_signed, a, b, label)) {
        return;
    }
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
    if (is_signed && IsImm(b) && b.b[0].value.sym < 0 && b.b[1].value.sym < 0) {
        SignedConstCompare(g, lt, a, b.b[0].value.offset & 0xFF, b.b[1].value.offset & 0xFF, label);
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
    bool plain_b = (b.b[0].kind == ByteKind::Mem || b.b[0].kind == ByteKind::Imm) &&
                   (b.b[1].kind == ByteKind::Mem || b.b[1].kind == ByteKind::Imm);
    if (plain_b && AHoldsByte(g, a, 1)) {
        HighFirstCompare(g, lt, is_signed, a, b, label);
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

// (& x c) against 0: only the bytes c has bits in, each a load and AND (or, for the top bit
// alone, a load whose N flag is the bit); the and is never worked out as a value. False (and
// nothing emitted) if id is not such an and
bool MaskTest(Gen& g, ExprId id, bool nonzero, std::int32_t label) {
    const Expr& e = At(g, ForwardTarget(g, id));
    std::uint16_t c = 0;
    if (e.kind != ExprKind::Binary || e.op != Op::And) {
        return false;
    }
    ExprId x = e.a;
    if (!ConstOf(g, e.b, c)) {
        if (!ConstOf(g, e.a, c)) {
            return false;
        }
        x = e.b;
    }
    Opnd o = TrySimple(g, x);
    std::uint32_t size = ExprSize(*g.program, id);
    if (o.size != size || o.in_a) {
        return false;
    }
    int masks[2] = {c & 0xFF, (c >> 8) & 0xFF};
    std::vector<int> bytes;
    for (int k = 0; k < static_cast<int>(size); k++) {
        if (masks[k] != 0) {
            bytes.push_back(k);
        }
    }
    if (bytes.empty()) {
        if (!nonzero) {
            Jump(g, label); // x & 0 is always 0
        }
        return true;
    }
    std::int32_t skip = NewLabel(g);
    for (std::size_t i = 0; i < bytes.size(); i++) {
        int k = bytes[i];
        bool last = i + 1 == bytes.size();
        LoadA(g, o, k);
        // the load set N and Z for A
        bool sign = masks[k] == 0x80;
        if (!sign && masks[k] != 0xFF) {
            Emit(g, Mn::AND, Mode::Imm, Num(masks[k]));
        }
        if (nonzero) {
            Branch(g, sign ? Mn::BMI : Mn::BNE, label);
        } else if (last) {
            Branch(g, sign ? Mn::BPL : Mn::BEQ, label);
        } else {
            Branch(g, sign ? Mn::BMI : Mn::BNE, skip);
        }
    }
    PlaceLabel(g, skip);
    return true;
}

namespace {

// an order compare one side's constant decides, at the smallest or largest value of the type:
// 1 always holds, 0 never, -1 neither
int DecidedOrder(const Gen& g, const Expr& e, Op op, bool is_signed, std::uint32_t size) {
    std::uint16_t max = is_signed ? (size == 1 ? 0x7F : 0x7FFF) : (size == 1 ? 0xFF : 0xFFFF);
    std::uint16_t min = is_signed ? (size == 1 ? 0x80 : 0x8000) : 0;
    std::uint16_t ca = 0;
    std::uint16_t cb = 0;
    bool a_const = ConstOf(g, e.a, ca);
    bool b_const = ConstOf(g, e.b, cb);
    bool a_max = a_const && ca == max;
    bool a_min = a_const && ca == min;
    bool b_max = b_const && cb == max;
    bool b_min = b_const && cb == min;
    switch (op) {
    case Op::Ge:
        return a_max || b_min ? 1 : -1;
    case Op::Lt:
        return a_max || b_min ? 0 : -1;
    case Op::Le:
        return a_min || b_max ? 1 : -1;
    case Op::Gt:
        return a_min || b_max ? 0 : -1;
    default:
        return -1;
    }
}

} // namespace

void GenCompare(Gen& g, const Expr& e, bool when, std::int32_t label) {
    Op op = when ? e.op : Negate(e.op);
    std::uint16_t zero = 0;
    bool equality = op == Op::Eq || op == Op::Ne;
    if (equality && ConstOf(g, e.b, zero) && zero == 0 && MaskTest(g, e.a, op == Op::Ne, label)) {
        return;
    }
    if (equality && ConstOf(g, e.a, zero) && zero == 0 && MaskTest(g, e.b, op == Op::Ne, label)) {
        return;
    }
    TypeKind kind = ExprKindOf(*g.program, e.a);
    bool is_signed = IsSignedKind(kind);
    std::uint32_t size = ExprSize(*g.program, e.a);
    std::uint16_t cb = 0;
    bool b_const = ConstOf(g, e.b, cb);
    if (is_signed && !equality && NeverNegative(g, e.a) && NeverNegative(g, e.b)) {
        // both in 0 to 127 (32767): signed and unsigned agree
        is_signed = false;
    }
    int decided = equality ? -1 : DecidedOrder(g, e, op, is_signed, size);
    if (decided >= 0) {
        // a constant at the end of the type's range: (>= 0xFFFF x) always holds
        if (HasEffects(*g.program, e.a)) {
            Eval(g, e.a, nullptr);
        }
        if (HasEffects(*g.program, e.b)) {
            Eval(g, e.b, nullptr);
        }
        if (decided == 1) {
            Jump(g, label);
        }
        return;
    }
    if (b_const && (op == Op::Gt || op == Op::Le)) {
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

// a compare of the while loop being generated against its variable plus one (gen_while.cpp):
// (> e v) and (< v e) are e >= t, with t = v + 1
bool BumpedCompare(Gen& g, ExprId id, bool when, std::int32_t label) {
    const Opnd* t = nullptr;
    for (const auto& b : g.bumped) {
        if (b.first == id) {
            t = &b.second;
        }
    }
    if (!t) {
        return false;
    }
    const Expr& e = At(g, id);
    ExprId other = e.op == Op::Gt ? e.a : e.b;
    Op op = when ? Op::Ge : Op::Lt;
    Opnd a = TrySimple(g, other);
    if (a.size == 0) {
        EvalA(g, other);
        a = InA();
    }
    CompareJump(g, op, false, a, *t, label);
    return true;
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
    if (ConstOf(g, id, c)) {
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
        // the second half only runs when the first is true (and) or false (or)
        std::size_t facts = 0;
        // and jumping on true, or jumping on false: both halves must agree
        if (is_and == when) {
            std::int32_t skip = NewLabel(g);
            GenBranch(g, e.a, !when, skip);
            facts = PushFacts(g, e.a, is_and, e.b);
            GenBranch(g, e.b, when, label);
            PopFacts(g, facts);
            PlaceLabel(g, skip);
            return;
        }
        GenBranch(g, e.a, when, label);
        facts = PushFacts(g, e.a, is_and, e.b);
        GenBranch(g, e.b, when, label);
        PopFacts(g, facts);
        return;
    }
    case ExprKind::Binary:
        if (IsComparison(e.op) && BumpedCompare(g, id, when, label)) {
            return;
        }
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
    if (ConstOf(g, id, c)) {
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
