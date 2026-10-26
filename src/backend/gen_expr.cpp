#include "backend/gen.hpp"

namespace tisple {

namespace {

// functions this small (in expressions) are inlined at every call, this many deep at most
constexpr std::uint32_t kMaxInlineExprs = 24;
constexpr std::size_t kMaxInlineDepth = 4;

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
           a.value.offset == b.value.offset && a.value.part == b.value.part;
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

// an instruction's operand bytes at a code label (self-modifying I/O)
Opnd PatchOpnd(std::int32_t label) {
    return MemOpnd(SymValue(label, 1), 2, 1);
}

// io_read: exactly one absolute access per byte, low byte first
Opnd EvalIoRead(Gen& g, const Expr& e, const Opnd* dst) {
    std::uint32_t size = SizeOf(g, e.type);
    std::uint16_t address = 0;
    bool fixed = ConstValue(*g.program, e.a, address);
    std::int32_t patch[2] = {-1, -1};
    if (!fixed) {
        Opnd a = Eval(g, e.a, nullptr);
        patch[0] = NewLabel(g);
        CopyOpnd(g, a, PatchOpnd(patch[0]));
        if (size == 2) {
            patch[1] = NewLabel(g);
            Combine16(g, Op::Add, a, ImmOpnd(1, 2), PatchOpnd(patch[1]));
        }
    }
    Opnd out = size == 2 ? Dest16(g, dst) : InA();
    for (std::uint32_t k = 0; k < size; k++) {
        if (fixed) {
            Emit(g, Mn::LDA, Mode::Abs, Num(static_cast<std::uint16_t>(address + k)));
        } else {
            PlaceLabel(g, patch[k]);
            Emit(g, Mn::LDA, Mode::Abs, Num(0xFFFF));
        }
        if (size == 2) {
            StoreA(g, out, static_cast<int>(k));
        }
    }
    return out;
}

void GenIoWrite(Gen& g, const Expr& e) {
    std::uint32_t size = e.value;
    std::uint16_t address = 0;
    bool fixed = ConstValue(*g.program, e.a, address);
    std::int32_t patch[2] = {-1, -1};
    if (!fixed) {
        Opnd a = Eval(g, e.a, nullptr);
        patch[0] = NewLabel(g);
        CopyOpnd(g, a, PatchOpnd(patch[0]));
        if (size == 2) {
            patch[1] = NewLabel(g);
            Combine16(g, Op::Add, a, ImmOpnd(1, 2), PatchOpnd(patch[1]));
        }
    }
    Opnd v = Eval(g, e.b, nullptr);
    for (std::uint32_t k = 0; k < size; k++) {
        LoadA(g, v, static_cast<int>(k));
        if (fixed) {
            Emit(g, Mn::STA, Mode::Abs, Num(static_cast<std::uint16_t>(address + k)));
        } else {
            PlaceLabel(g, patch[k]);
            Emit(g, Mn::STA, Mode::Abs, Num(0xFFFF));
        }
    }
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

// the callee's body right here, its arguments already in its frame: no JSR or RTS, and a
// loop around the call can keep its counter in a register
Opnd InlineBody(Gen& g, const Function& fn, std::uint32_t size, const Opnd* dst) {
    InlineCtx ctx;
    ctx.ret_label = NewLabel(g);
    bool wide = fn.ret != kVoidType && size == 2;
    if (wide) {
        ctx.dst = Dest16(g, dst);
    }
    g.inlines.push_back(ctx);
    if (fn.ret == kVoidType || size == 0) {
        GenStmt(g, fn.body);
    } else if (size == 1) {
        EvalA(g, fn.body);
    } else {
        EvalInto(g, fn.body, ctx.dst);
    }
    g.inlines.pop_back();
    PlaceLabel(g, ctx.ret_label);
    if (fn.ret == kVoidType || size == 0) {
        return ImmOpnd(0, 1);
    }
    return wide ? ctx.dst : InA();
}

Opnd EvalIf(Gen& g, const Expr& e, const Opnd* dst) {
    std::uint32_t size = SizeOf(g, e.type);
    std::uint16_t c = 0;
    if (ConstValue(*g.program, e.a, c)) {
        return Eval(g, c != 0 ? e.b : e.c, dst);
    }
    std::int32_t other = NewLabel(g);
    std::int32_t end = NewLabel(g);
    GenBranch(g, e.a, false, other);
    if (size == 1) {
        EvalA(g, e.b);
        Jump(g, end);
        PlaceLabel(g, other);
        EvalA(g, e.c);
        PlaceLabel(g, end);
        return InA();
    }
    Opnd d = Dest16(g, dst);
    EvalInto(g, e.b, d);
    Jump(g, end);
    PlaceLabel(g, other);
    EvalInto(g, e.c, d);
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
    if (CrossAliases(a, d)) {
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
    case ExprKind::AddrOf:
        return AddressOf(g, e.a, dst);
    case ExprKind::If:
        return EvalIf(g, e, dst);
    case ExprKind::Do:
        return EvalDo(g, e, dst);
    case ExprKind::Match:
        return EvalMatchValue(g, e, dst);
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

Opnd EvalCall(Gen& g, ExprId id, const Opnd* dst) {
    const Expr& e = At(g, id);
    const Function& fn = g.program->functions[static_cast<std::size_t>(e.ref)];
    std::vector<Opnd> pending(e.count);
    std::vector<bool> later_calls(e.count, false);
    bool calls = false;
    for (std::uint32_t i = e.count; i-- > 0;) {
        later_calls[i] = calls;
        calls = calls || HasCalls(*g.program, ListItem(*g.program, e, i));
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        ExprId arg = ListItem(*g.program, e, i);
        std::int32_t param = fn.params[i];
        std::uint32_t size =
            TypeOf(*g.program, g.program->vars[static_cast<std::size_t>(param)].type).size;
        Opnd slot = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(param)]), size, 1);
        if (!later_calls[i]) {
            EvalInto(g, arg, slot);
            continue;
        }
        // a later argument calls something whose frame may share these bytes
        Opnd v = Eval(g, arg, nullptr);
        pending[i] = Snapshot(g, v);
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        if (pending[i].size == 0) {
            continue;
        }
        std::int32_t param = fn.params[i];
        Opnd slot =
            MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(param)]), pending[i].size, 1);
        CopyOpnd(g, pending[i], slot);
    }
    std::uint32_t size = SizeOf(g, fn.ret);
    if (g.be->fn_exprs[static_cast<std::size_t>(e.ref)] <= kMaxInlineExprs &&
        g.inlines.size() < kMaxInlineDepth) {
        return InlineBody(g, fn, size, dst);
    }
    Emit(g, Mn::JSR, Mode::Abs, SymValue(g.be->fn_sym[static_cast<std::size_t>(e.ref)]));
    if (fn.ret == kVoidType || size == 0) {
        return ImmOpnd(0, 1);
    }
    if (size == 1) {
        return InA();
    }
    Opnd d = Dest16(g, dst);
    StoreAX(g, d);
    return d;
}

void GenIo(Gen& g, ExprId id) {
    const Expr& e = At(g, id);
    if (e.kind == ExprKind::IoWrite) {
        GenIoWrite(g, e);
        return;
    }
    EvalIoRead(g, e, nullptr);
}

} // namespace tisple
