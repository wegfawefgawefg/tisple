#include "backend/gen.hpp"

#include <algorithm>

namespace tisple {

namespace {

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

std::uint32_t SizeOf(const Gen& g, TypeId type) {
    return TypeOf(*g.program, type).size;
}

bool SameByte(const ByteRef& a, const ByteRef& b) {
    return a.kind == b.kind && a.value.sym == b.value.sym && a.value.offset == b.value.offset &&
           a.value.part == b.value.part && a.y == b.y && a.y_index == b.y_index && a.reg == b.reg;
}

bool SameIndex(const Index& a, const Index& b) {
    if (a.used != b.used) {
        return false;
    }
    if (!a.used) {
        return true;
    }
    return a.is_reg == b.is_reg && a.reg == b.reg && a.value.sym == b.value.sym &&
           a.value.offset == b.value.offset && a.value.part == b.value.part && a.delta == b.delta;
}

bool SameOpnd(const Opnd& a, const Opnd& b) {
    if (a.size != b.size || a.in_a != b.in_a || !SameIndex(a.index, b.index)) {
        return false;
    }
    for (int k = 0; k < a.size; k++) {
        if (!SameByte(a.b[k], b.b[k])) {
            return false;
        }
    }
    return true;
}

bool HasIndirect(const Opnd& o) {
    for (int k = 0; k < o.size; k++) {
        if (o.b[k].kind == ByteKind::IndY) {
            return true;
        }
    }
    return false;
}

Opnd Dest16(Gen& g, const Opnd* dst) {
    return dst ? *dst : NewTemp(g, 2);
}

Opnd EvalConvert(Gen& g, const Expr& e, const Opnd* dst) {
    std::uint32_t from = ExprSize(*g.program, e.a);
    std::uint32_t to = SizeOf(g, e.type);
    if (from == 2 && to == 1) {
        Opnd o = Eval(g, e.a, nullptr);
        o.size = 1;
        return o;
    }
    if (from == to) {
        return Eval(g, e.a, to == 2 ? dst : nullptr);
    }
    if (!IsSignedKind(ExprKindOf(*g.program, e.a))) {
        Opnd o = Eval(g, e.a, nullptr);
        if (o.in_a) {
            o = SpillA(g);
        }
        o.size = 2;
        o.b[1] = ByteRef();
        o.b[1].value = Num(0);
        return o;
    }
    // sign extension: C = bit 7, then 0 + $FF + C is $00 or $FF; flip it
    EvalA(g, e.a);
    Opnd d = Dest16(g, dst);
    StoreA(g, d, 0);
    Emit(g, Mn::ASL, Mode::Acc);
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    Emit(g, Mn::ADC, Mode::Imm, Num(0xFF));
    Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
    StoreA(g, d, 1);
    return d;
}

// to the end of an if whose then arm left its value in A: after LDA #k the flags say
// whether k is zero, so a branch does it in two bytes instead of three
void JumpOverElse(Gen& g, std::int32_t end) {
    const AsmItem& last = g.out->back();
    if (last.kind == ItemKind::Ins && last.mn == Mn::LDA && last.mode == Mode::Imm &&
        last.value.sym < 0) {
        Branch(g, (last.value.offset & 0xFF) != 0 ? Mn::BNE : Mn::BEQ, end);
        return;
    }
    Jump(g, end);
}

Opnd EvalIf(Gen& g, const Expr& e, const Opnd* dst) {
    std::uint32_t size = SizeOf(g, e.type);
    std::uint16_t c = 0;
    if (ConstOf(g, e.a, c)) {
        return Eval(g, c != 0 ? e.b : e.c, dst);
    }
    std::int32_t other = NewLabel(g);
    std::int32_t end = NewLabel(g);
    GenBranch(g, e.a, false, other);
    // each arm knows how the condition came out, about what it does not change itself
    std::size_t facts = PushFacts(g, e.a, true, e.b);
    if (size == 1) {
        EvalA(g, e.b);
        PopFacts(g, facts);
        JumpOverElse(g, end);
        PlaceLabel(g, other);
        facts = PushFacts(g, e.a, false, e.c);
        EvalA(g, e.c);
        PopFacts(g, facts);
        PlaceLabel(g, end);
        return InA();
    }
    Opnd d = Dest16(g, dst);
    EvalInto(g, e.b, d);
    PopFacts(g, facts);
    Jump(g, end);
    PlaceLabel(g, other);
    facts = PushFacts(g, e.a, false, e.c);
    EvalInto(g, e.c, d);
    PopFacts(g, facts);
    PlaceLabel(g, end);
    return d;
}

Opnd EvalDo(Gen& g, const Expr& e, const Opnd* dst) {
    if (e.count == 0) {
        return ImmOpnd(0, 1);
    }
    for (std::uint32_t i = 0; i + 1 < e.count; i++) {
        ExprId item = ListItem(*g.program, e, i);
        GenStmt(g, item);
        if (At(g, item).type == kNeverType) {
            return ImmOpnd(0, SizeOf(g, e.type) == 2 ? 2 : 1);
        }
    }
    ExprId last = ListItem(*g.program, e, e.count - 1);
    SourceNote(g, At(g, last).loc);
    return Eval(g, last, dst);
}

Opnd EvalMatchValue(Gen& g, const Expr& e, const Opnd* dst) {
    if (SizeOf(g, e.type) == 1) {
        GenMatch(g, e, nullptr, true);
        return InA();
    }
    Opnd d = Dest16(g, dst);
    GenMatch(g, e, &d, true);
    return d;
}

Opnd EvalUnary(Gen& g, ExprId id, const Expr& e, const Opnd* dst) {
    if (e.op == Op::Not) {
        CondToA(g, id);
        return InA();
    }
    if (SizeOf(g, e.type) == 1) {
        EvalA(g, e.a);
        Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
        return InA();
    }
    Opnd a = Eval(g, e.a, nullptr);
    Opnd d = Dest16(g, dst);
    if (CrossAliases(g, a, d)) {
        d = NewTemp(g, 2);
    }
    for (int k = 0; k < 2; k++) {
        LoadA(g, a, k);
        Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
        StoreA(g, d, k);
    }
    return d;
}

bool IsComparison(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt ||
           op == Op::Ge;
}

} // namespace

void EvalA(Gen& g, ExprId id) {
    Opnd o = Eval(g, id, nullptr);
    if (o.size == 0) {
        return;
    }
    LoadA(g, o, 0);
}

Opnd Eval(Gen& g, ExprId id, const Opnd* dst) {
    id = ForwardTarget(g, id);
    Opnd replaced;
    if (ReadReplacedCounter(g, id, dst, replaced)) {
        return replaced;
    }
    const Expr& e = At(g, id);
    if (e.type == kNeverType || e.type == kVoidType) {
        GenStmt(g, id);
        return ImmOpnd(0, 1);
    }
    Opnd simple = TrySimple(g, id);
    if (simple.size != 0) {
        return simple;
    }
    std::uint32_t size = SizeOf(g, e.type);
    switch (e.kind) {
    case ExprKind::VarRef:
    case ExprKind::Field:
    case ExprKind::Index:
    case ExprKind::Load:
    case ExprKind::LoadField: {
        PlaceRef place;
        if (!ResolvePlace(g, id, -1, place)) {
            return ImmOpnd(0, size);
        }
        NormalizePlace(g, place, e.type);
        return PlaceOpnd(g, place, e.type);
    }
    case ExprKind::Unary:
        return EvalUnary(g, id, e, dst);
    case ExprKind::Binary:
        if (IsComparison(e.op)) {
            CondToA(g, id);
            return InA();
        }
        if (e.op == Op::Div || e.op == Op::Mod) {
            if (size == 1) {
                EvalDiv8(g, id, e);
                return InA();
            }
            return EvalDiv16(g, id, e, dst);
        }
        if (size == 1) {
            EvalArith8(g, e);
            return InA();
        }
        return EvalArith16(g, e, dst);
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return EvalArith16(g, e, dst);
    case ExprKind::And:
    case ExprKind::Or:
        CondToA(g, id);
        return InA();
    case ExprKind::Convert:
        return EvalConvert(g, e, dst);
    case ExprKind::Reinterpret:
        return Eval(g, e.a, dst);
    case ExprKind::Call:
        return EvalCall(g, id, dst);
    case ExprKind::IoRead:
        return EvalIoRead(g, e, dst);
    case ExprKind::Jsr:
        return EvalJsr(g, id);
    case ExprKind::AddrOf:
        return AddressOf(g, e.a, dst);
    case ExprKind::If:
        return EvalIf(g, e, dst);
    case ExprKind::Do:
        return EvalDo(g, e, dst);
    case ExprKind::Match:
        return EvalMatchValue(g, e, dst);
    case ExprKind::AuxSwitch:
        return EvalAuxOff(g, e, dst);
    default:
        Unsupported(g, id, "this expression");
        return ImmOpnd(0, size == 2 ? 2 : 1);
    }
}

void EvalInto(Gen& g, ExprId id, const Opnd& dst) {
    if (dst.size == 1) {
        Opnd o = Eval(g, id, nullptr);
        if (o.in_a) {
            StoreA(g, dst, 0);
            return;
        }
        CopyOpnd(g, o, dst);
        return;
    }
    // straight into dst only when no byte of it can alias another byte being read
    bool direct = !HasIndirect(dst);
    Opnd o = Eval(g, id, direct ? &dst : nullptr);
    if (!SameOpnd(o, dst)) {
        CopyOpnd(g, o, dst);
    }
}

} // namespace tisple
