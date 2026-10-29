#include "backend/gen.hpp"

namespace tisple {

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

bool CountReady(const Opnd& count) {
    ByteKind kind = count.b[0].kind;
    return count.size != 0 && !count.in_a &&
           (kind == ByteKind::Imm || kind == ByteKind::Mem || kind == ByteKind::Reg);
}

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

void SignByteA(Gen& g) {
    // A's bit 7 into C, then 0 + $FF + C is $00 or $FF; flip it
    Emit(g, Mn::ASL, Mode::Acc);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Emit(g, Mn::ADC, Mode::Imm, Num(0xFF));
    Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
}

namespace {

// value into a 16-bit dst that RMW instructions can work on (else a temporary)
Opnd ShiftTarget(Gen& g, const Opnd* dst) {
    if (dst && dst->b[0].kind == ByteKind::Mem && dst->b[1].kind == ByteKind::Mem) {
        return *dst;
    }
    return NewTemp(g, 2);
}

// a and d are the same two plain memory bytes
bool SameMemory(const Opnd& a, const Opnd& d) {
    if (a.in_a || a.size != 2 || d.size != 2) {
        return false;
    }
    for (int k = 0; k < 2; k++) {
        const ByteRef& x = a.b[k];
        const ByteRef& y = d.b[k];
        bool same = x.kind == ByteKind::Mem && y.kind == ByteKind::Mem && x.value.sym >= 0 &&
                    x.value.sym == y.value.sym && x.value.offset == y.value.offset &&
                    x.value.part == y.value.part;
        if (!same) {
            return false;
        }
    }
    return true;
}

// a count of 8 or more in register r, with d's low byte in memory and its high byte in A,
// at the top of the loop: one byte moved and 8 off the count, then on to loop, or to done
// when that leaves 0. Below 8 it goes to loop as it is
void ByteFirst(Gen& g, Op op, bool is_signed, const Opnd& d, int r, std::int32_t loop,
               std::int32_t done) {
    Emit(g, r == 0 ? Mn::CPX : Mn::CPY, Mode::Imm, Num(8));
    Branch(g, Mn::BCC, loop);
    if (op == Op::Shl) {
        LoadA(g, d, 0);
        Emit(g, Mn::PHA, Mode::Imp);
    } else {
        StoreA(g, d, 0);
    }
    // the carry is set: count - 8
    Emit(g, r == 0 ? Mn::TXA : Mn::TYA, Mode::Imp);
    Emit(g, Mn::SBC, Mode::Imm, Num(8));
    Emit(g, r == 0 ? Mn::TAX : Mn::TAY, Mode::Imp);
    if (op == Op::Shl) {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        StoreA(g, d, 0);
        Emit(g, Mn::PLA, Mode::Imp);
    } else if (is_signed) {
        LoadA(g, d, 0);
        SignByteA(g);
    } else {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
    }
    Emit(g, r == 0 ? Mn::CPX : Mn::CPY, Mode::Imm, Num(0));
    Branch(g, Mn::BNE, loop);
    Branch(g, Mn::BEQ, done);
}

// 16-bit shift of a into d (plain memory); a constant count c, or the count operand (or with
// in_reg, the count already in the counter register) and the largest it can be
void Shift16(Gen& g, Op op, bool is_signed, const Opnd& a, const Opnd& d, bool fixed,
             std::uint16_t c, const Opnd& count, std::uint32_t count_max, bool in_reg) {
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
    bool in_place = SameMemory(a, d);
    if (in_place && fixed && left && c <= 2) {
        // ASL / ROL right where it is
        for (int i = 0; i < c; i++) {
            RmwByte(g, Mn::ASL, d, 0);
            RmwByte(g, Mn::ROL, d, 1);
        }
        return;
    }
    // (the high byte is read after the low one is stored)
    const AsmValue& high = a.b[1].value;
    const AsmValue& low = d.b[0].value;
    bool high_at_low = a.b[1].kind == ByteKind::Mem && d.b[0].kind == ByteKind::Mem &&
                       high.sym == low.sym && high.offset == low.offset && high.part == low.part;
    if (!in_place && !a.in_a && fixed && left && c == 1 && !high_at_low) {
        // both bytes through A, the carry between them
        LoadA(g, a, 0);
        Emit(g, Mn::ASL, Mode::Acc);
        StoreA(g, d, 0);
        LoadA(g, a, 1);
        Emit(g, Mn::ROL, Mode::Acc);
        StoreA(g, d, 1);
        return;
    }
    // the low byte shifts in memory, the high byte in A
    if (!in_place) {
        LoadA(g, a, 0);
        StoreA(g, d, 0);
    }
    LoadA(g, a, 1);
    std::int32_t done = -1;
    std::int32_t loop = -1;
    int r = CounterReg(g);
    int steps = fixed ? c : 1;
    if (!fixed) {
        done = NewLabel(g);
        loop = NewLabel(g);
        if (in_reg) {
            Emit(g, r == 0 ? Mn::CPX : Mn::CPY, Mode::Imm, Num(0));
        } else {
            LoadCount(g, r, count);
        }
        Branch(g, Mn::BEQ, done);
        if (count_max >= 8) {
            ByteFirst(g, op, is_signed, d, r, loop, done);
        }
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

// -1 unless byte k is a plain number
int KnownByte(const Opnd& o, int k) {
    const ByteRef& b = o.b[k];
    if (o.in_a || b.kind != ByteKind::Imm || b.value.sym >= 0) {
        return -1;
    }
    return b.value.offset & 0xFF;
}

// a constant shift with a byte of the result known to be 0 (a_max: no value of a is
// larger): that byte is the number 0 in the result, and only the other one is worked out
// in d. False if no byte is known.
bool ShiftKnown(Gen& g, Op op, bool is_signed, const Opnd& a, std::uint32_t a_max, std::uint16_t c,
                const Opnd& d, Opnd& out) {
    bool high_zero = KnownByte(a, 1) == 0;
    out = d;
    if (op == Op::Shl && c >= 8 && c < 16) {
        // the low byte, moved up
        LoadA(g, a, 0);
        for (int i = 8; i < c; i++) {
            Emit(g, Mn::ASL, Mode::Acc);
        }
        StoreA(g, d, 1);
        out.b[0] = ByteRef();
        out.b[0].value = Num(0);
        return true;
    }
    if (op == Op::Shl && c < 8 && (a_max << c) <= 0xFF) {
        LoadA(g, a, 0);
        for (int i = 0; i < c; i++) {
            Emit(g, Mn::ASL, Mode::Acc);
        }
        StoreA(g, d, 0);
        out.b[1] = ByteRef();
        out.b[1].value = Num(0);
        return true;
    }
    if (op == Op::Shl && c >= 5 && c < 8 && high_zero) {
        // [lo 00] >> (8 - c): the high byte is lo >> (8 - c), the low byte the bits out
        int steps = 8 - c;
        if (steps == 1) {
            LoadA(g, a, 0);
            Emit(g, Mn::LSR, Mode::Acc);
            StoreA(g, d, 1);
            Emit(g, Mn::LDA, Mode::Imm, Num(0));
            Emit(g, Mn::ROR, Mode::Acc);
            StoreA(g, d, 0);
            return true;
        }
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        StoreA(g, d, 0);
        LoadA(g, a, 0);
        for (int i = 0; i < steps; i++) {
            Emit(g, Mn::LSR, Mode::Acc);
            RmwByte(g, Mn::ROR, d, 0);
        }
        StoreA(g, d, 1);
        return true;
    }
    if (op == Op::Shr && !is_signed && c >= 8 && c < 16) {
        // the high byte, moved down
        LoadA(g, a, 1);
        for (int i = 8; i < c; i++) {
            Emit(g, Mn::LSR, Mode::Acc);
        }
        StoreA(g, d, 0);
        out.b[1] = ByteRef();
        out.b[1].value = Num(0);
        return true;
    }
    if (op == Op::Shr && !is_signed && high_zero && c < 8) {
        LoadA(g, a, 0);
        for (int i = 0; i < c; i++) {
            Emit(g, Mn::LSR, Mode::Acc);
        }
        StoreA(g, d, 0);
        out.b[1] = ByteRef();
        out.b[1].value = Num(0);
        return true;
    }
    return false;
}

} // namespace

Opnd ShiftConst16(Gen& g, Op op, bool is_signed, const Opnd& a, std::uint16_t count,
                  const Opnd* dst) {
    Opnd d = ShiftTarget(g, dst);
    if (Overlaps(g, a, d)) {
        d = NewTemp(g, 2);
    }
    Opnd known;
    if (ShiftKnown(g, op, is_signed, a, 0xFFFF, count, d, known)) {
        return known;
    }
    Shift16(g, op, is_signed, a, d, true, count, Opnd(), 0, false);
    return d;
}

Opnd EvalShift16(Gen& g, const Expr& e, const Opnd* dst) {
    Op op = e.op;
    Opnd a = Eval(g, e.a, nullptr);
    std::uint16_t c = 0;
    bool fixed = ConstOf(g, e.b, c);
    Opnd count;
    if (!fixed) {
        if (MayWriteOpnd(g, e.b, a)) {
            a = Snapshot(g, a);
        }
        count = TrySimple(g, e.b);
        if (!CountReady(count)) {
            EvalA(g, e.b);
            count = SpillA(g);
        }
    }
    Opnd d = ShiftTarget(g, dst);
    // a variable shifted into itself by a count whose code reads every byte before writing it
    bool in_place = fixed && (c < 5 || c >= 8) && SameMemory(a, d);
    if (Overlaps(g, a, d) && !in_place) {
        d = NewTemp(g, 2);
    }
    // a count just worked out in A goes straight into its register when nothing on the way
    // needs one
    bool in_reg =
        !fixed && count.temp && AHoldsByte(g, count, 0) && IsPlainMemory(a) && IsPlainMemory(d);
    if (in_reg) {
        Emit(g, CounterReg(g) == 0 ? Mn::TAX : Mn::TAY, Mode::Imp);
    }
    bool is_signed = IsSignedKind(ExprKindOf(*g.program, e.a));
    Opnd known;
    if (fixed && !in_place && ShiftKnown(g, op, is_signed, a, MaxOf(g, e.a), c, d, known)) {
        return known;
    }
    Shift16(g, op, is_signed, a, d, fixed, c, count, fixed ? 0 : MaxOf(g, e.b), in_reg);
    return d;
}

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

// the variable an expression only reads: a plain local or global in memory, not a register
std::int32_t PlainVar(const Gen& g, ExprId id) {
    const Expr& e = At(g, id);
    if (e.kind != ExprKind::VarRef || e.ref == g.pin[0] || e.ref == g.pin[1]) {
        return -1;
    }
    const Var& v = g.program->vars[static_cast<std::size_t>(e.ref)];
    TypeKind kind = KindOf(*g.program, v.type);
    bool scalar = kind == TypeKind::U8 || kind == TypeKind::U16;
    if (!scalar || v.address_taken || v.placement.has_address || BoundTarget(g, id) != id ||
        g.be->aliases[static_cast<std::size_t>(e.ref)].value >= 0) {
        return -1;
    }
    for (std::int32_t r : g.reduced) {
        if (r == e.ref) {
            return -1;
        }
    }
    return e.ref;
}

// (<< var 1)
bool IsShiftOne(const Gen& g, ExprId id, std::int32_t var) {
    const Expr& e = At(g, id);
    std::uint16_t c = 0;
    return e.kind == ExprKind::Binary && e.op == Op::Shl && ConstOf(g, e.b, c) && c == 1 &&
           At(g, e.a).kind == ExprKind::VarRef && At(g, e.a).ref == var;
}

// every (<< var 1) in id into shifts; false if var is read some other way, or a call may
bool CollectShifts(const Gen& g, ExprId id, std::int32_t var, int depth,
                   std::vector<ExprId>& shifts) {
    if (id < 0) {
        return true;
    }
    if (depth > 16) {
        return false;
    }
    const Expr& e = At(g, id);
    if (IsShiftOne(g, id, var)) {
        shifts.push_back(id);
        return true;
    }
    switch (e.kind) {
    case ExprKind::VarRef:
        return e.ref != var;
    case ExprKind::Literal:
        return true;
    case ExprKind::Binary:
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Index:
    case ExprKind::Field:
        return CollectShifts(g, e.a, var, depth + 1, shifts) &&
               CollectShifts(g, e.b, var, depth + 1, shifts);
    default:
        return false;
    }
}

// cond is (!= (& var top) 0) or (= (& var top) 0) for var's top bit; set: true for !=
bool TopBitTest(const Gen& g, ExprId cond, std::int32_t& var, bool& set) {
    const Expr& e = At(g, cond);
    if (e.kind != ExprKind::Binary || (e.op != Op::Ne && e.op != Op::Eq)) {
        return false;
    }
    std::uint16_t c = 0;
    ExprId masked = -1;
    if (ConstOf(g, e.b, c) && c == 0) {
        masked = e.a;
    } else if (ConstOf(g, e.a, c) && c == 0) {
        masked = e.b;
    } else {
        return false;
    }
    const Expr& m = At(g, masked);
    if (m.kind != ExprKind::Binary || m.op != Op::And) {
        return false;
    }
    ExprId other = -1;
    if (ConstOf(g, m.b, c)) {
        other = m.a;
    } else if (ConstOf(g, m.a, c)) {
        other = m.b;
    } else {
        return false;
    }
    var = PlainVar(g, other);
    if (var < 0) {
        return false;
    }
    std::uint32_t top = ExprSize(*g.program, other) == 2 ? 0x8000 : 0x80;
    set = e.op == Op::Ne;
    return c == top;
}

// the arm is (set var value): its value, or -1
ExprId ArmValue(const Gen& g, ExprId arm, std::int32_t var) {
    const Expr& e = At(g, arm);
    if (e.kind != ExprKind::Set) {
        return -1;
    }
    const Expr& place = At(g, e.a);
    return place.kind == ExprKind::VarRef && place.ref == var ? e.b : -1;
}

} // namespace

bool GenShiftedIf(Gen& g, const Expr& e) {
    std::int32_t var = -1;
    bool set = false;
    if (e.c < 0 || !TopBitTest(g, e.a, var, set)) {
        return false;
    }
    ExprId then_value = ArmValue(g, e.b, var);
    ExprId else_value = ArmValue(g, e.c, var);
    std::vector<ExprId> shifts;
    if (then_value < 0 || else_value < 0 || !CollectShifts(g, then_value, var, 0, shifts) ||
        !CollectShifts(g, else_value, var, 0, shifts) || shifts.empty()) {
        return false;
    }
    // shift first: the bit the condition tests falls into the carry, and the arms' (<< var 1)
    // is var itself from then on
    std::uint32_t size =
        TypeOf(*g.program, g.program->vars[static_cast<std::size_t>(var)].type).size;
    Opnd v = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(var)]), size, 1);
    RmwByte(g, Mn::ASL, v, 0);
    if (size == 2) {
        RmwByte(g, Mn::ROL, v, 1);
    }
    std::int32_t other = NewLabel(g);
    std::int32_t end = NewLabel(g);
    Branch(g, set ? Mn::BCC : Mn::BCS, other);
    std::size_t mark = g.reduced_bytes.size();
    for (ExprId s : shifts) {
        ReducedByte r;
        r.expr = s;
        r.temp = v;
        g.reduced_bytes.push_back(r);
    }
    GenStmt(g, e.b);
    Jump(g, end);
    PlaceLabel(g, other);
    GenStmt(g, e.c);
    PlaceLabel(g, end);
    g.reduced_bytes.resize(mark);
    return true;
}

} // namespace tisple
