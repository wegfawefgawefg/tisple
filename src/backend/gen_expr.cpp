#include "backend/gen.hpp"

namespace tisple {

namespace {

// functions of at most kMaxInlineExprs expressions (analysis.hpp) are inlined at every
// call, this many deep at most
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

// does `from` call `to`, directly or not
bool CallsFunction(const Program& program, std::int32_t from, std::int32_t to) {
    std::vector<bool> seen(program.functions.size(), false);
    std::vector<std::int32_t> work = {from};
    while (!work.empty()) {
        std::int32_t f = work.back();
        work.pop_back();
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(f)].callees) {
            if (callee == to) {
                return true;
            }
            if (!seen[static_cast<std::size_t>(callee)]) {
                seen[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    return false;
}

// may a byte of dst share memory with the frame of `callee` or of what it calls: dst is in
// the frame of a function that is never live together with callee (an argument slot of
// another call), so the frames may overlap
bool SharesFrameWith(const Gen& g, const Opnd& dst, std::int32_t callee) {
    for (int k = 0; k < dst.size; k++) {
        const ByteRef& b = dst.b[k];
        if (b.kind != ByteKind::Mem || b.value.sym < 0) {
            continue;
        }
        const SymInfo& info = g.be->info[static_cast<std::size_t>(b.value.sym)];
        std::int32_t owner = -1;
        if (info.role == SymRole::Var) {
            owner = g.program->vars[static_cast<std::size_t>(info.owner)].function;
        } else if (info.role == SymRole::Temps) {
            owner = info.owner;
        }
        if (owner < 0) {
            continue;
        }
        if (owner == callee || !CallsFunction(*g.program, owner, callee)) {
            return true;
        }
    }
    return false;
}

// the callee's body right here, its arguments already in its frame: no JSR or RTS, and a
// loop around the call can keep its counter in a register
Opnd InlineBody(Gen& g, std::int32_t callee, std::uint32_t size, const Opnd* dst) {
    const Function& fn = g.program->functions[static_cast<std::size_t>(callee)];
    InlineCtx ctx;
    ctx.ret_label = NewLabel(g);
    bool wide = fn.ret != kVoidType && size == 2;
    if (dst && SharesFrameWith(g, *dst, callee)) {
        dst = nullptr;
    }
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
    if (ConstValue(*g.program, e.a, c)) {
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

// an inlined callee's parameter can read its argument where it is: a plain variable (not
// a let read in its place), or a conversion of one that is only its bytes (as u16 of an i16,
// as u8 of a u16, as u16 of a u8), the parameter never set nor its address taken, and nothing
// in the callee nor in the later arguments changes the variable
bool CanBind(const Gen& g, const Function& fn, const Expr& call, std::uint32_t i) {
    const Program& program = *g.program;
    ExprId arg = ListItem(program, call, i);
    for (int depth = 0; depth < 4; depth++) {
        const Expr& c = ExprOf(program, arg);
        if (c.kind != ExprKind::Convert && c.kind != ExprKind::Reinterpret) {
            break;
        }
        std::uint32_t from_size = ExprSize(program, c.a);
        bool bytes =
            from_size == 2 || !IsSignedKind(ExprKindOf(program, c.a)) || SizeOf(g, c.type) == 1;
        if (!bytes) {
            return false;
        }
        arg = c.a;
    }
    const Expr& a = ExprOf(program, arg);
    std::int32_t param = fn.params[i];
    if (a.kind != ExprKind::VarRef || program.vars[static_cast<std::size_t>(param)].address_taken) {
        return false;
    }
    std::int32_t var = a.ref;
    // a loop counter in a register is better copied to memory once for the callee
    if (var == g.pin[0] || var == g.pin[1] || IsReplacedCounter(g, var)) {
        return false;
    }
    if (g.be->aliases[static_cast<std::size_t>(var)].value >= 0 ||
        g.be->aliases[static_cast<std::size_t>(param)].value >= 0) {
        return false;
    }
    if (MayWriteVarIn(g, fn.body, param) || MayWriteVarIn(g, fn.body, var)) {
        return false;
    }
    for (std::uint32_t j = i + 1; j < call.count; j++) {
        if (MayWriteVarIn(g, ListItem(program, call, j), var)) {
            return false;
        }
    }
    return true;
}

Opnd EvalCall(Gen& g, ExprId id, const Opnd* dst) {
    const Expr& e = At(g, id);
    const Function& fn = g.program->functions[static_cast<std::size_t>(e.ref)];
    bool inline_it = g.be->fn_exprs[static_cast<std::size_t>(e.ref)] <= kMaxInlineExprs &&
                     g.inlines.size() < kMaxInlineDepth;
    std::vector<bool> bind(e.count, false);
    for (std::uint32_t i = 0; i < e.count && inline_it; i++) {
        bind[i] = CanBind(g, fn, e, i);
    }
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
        if (bind[i]) {
            continue;
        }
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
    if (inline_it) {
        std::size_t bound = g.bound.size();
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (bind[i]) {
                g.bound.push_back({fn.params[i], ListItem(*g.program, e, i)});
            }
        }
        Opnd out = InlineBody(g, e.ref, size, dst);
        g.bound.resize(bound);
        return out;
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
