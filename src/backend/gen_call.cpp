// Calls: the arguments into the callee's parameters (or read where they are, or kept where
// the callee hands them on, gen_forward.cpp), then a JSR, or the callee's body inlined here
// (FindInlines, analysis.hpp).

#include "backend/gen.hpp"

#include <algorithm>

namespace tisple {

namespace {

// calls are inlined (FindInlines, analysis.hpp) this many deep at most
constexpr std::size_t kMaxInlineDepth = 4;

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

std::uint32_t SizeOf(const Gen& g, TypeId type) {
    return TypeOf(*g.program, type).size;
}

Opnd Dest16(Gen& g, const Opnd* dst) {
    return dst ? *dst : NewTemp(g, 2);
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

} // namespace

// an inlined callee's parameter can read its argument where it is: a plain variable (not
// a let read in its place), or a conversion of one that is only its bytes (as u16 of an i16,
// as u8 of a u16, as u16 of a u8), the parameter never set nor its address taken, and nothing
// in the callee nor in the later arguments changes the variable
bool CanBind(const Gen& g, const Function& fn, const Expr& call, std::uint32_t i) {
    const Program& program = *g.program;
    ExprId arg = ListItem(program, call, i);
    std::int32_t param = fn.params[i];
    std::uint16_t number = 0;
    if (ConstOf(g, arg, number)) {
        // a number under the bindings (an unrolled counter): the parameter reads as it
        return !program.vars[static_cast<std::size_t>(param)].address_taken &&
               g.be->aliases[static_cast<std::size_t>(param)].value < 0 &&
               !MayWriteVarIn(g, fn.body, param);
    }
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
    InlineKind kind = g.be->inlines[static_cast<std::size_t>(e.ref)];
    bool inline_it = kind != InlineKind::None && g.inlines.size() < kMaxInlineDepth;
    if (kind == InlineKind::Once) {
        // one copy of the body at most: unrolled copies of the call and a call made again
        // jump to the function
        bool again =
            std::find(g.inlined_once.begin(), g.inlined_once.end(), e.ref) != g.inlined_once.end();
        // in a loop, a body with loops of its own keeps its call: the JSR is little next to
        // its work, and its code stays where it was
        bool loops = g.loop_depth > 0 && HasLoops(*g.program, fn.body);
        inline_it = inline_it && !again && g.unrolled_copies == 1 && !loops;
        if (inline_it) {
            g.inlined_once.push_back(e.ref);
        }
    }
    std::vector<bool> bind(e.count, false);
    std::vector<std::int32_t> slot_var(fn.params.begin(), fn.params.end());
    for (std::uint32_t i = 0; i < e.count && inline_it; i++) {
        bind[i] = CanBind(g, fn, e, i);
        std::int32_t to = bind[i] ? -1 : ForwardedParam(g, fn, i);
        slot_var[i] = to >= 0 ? to : slot_var[i];
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
        std::int32_t slot_sym = g.be->var_sym[static_cast<std::size_t>(slot_var[i])];
        Opnd slot = MemOpnd(SymValue(slot_sym), size, 1);
        // a forwarded parameter handed on: already in this slot
        const Expr& a = At(g, arg);
        bool forwarded =
            a.kind == ExprKind::VarRef &&
            std::find(g.forwarded.begin(), g.forwarded.end(), a.ref) != g.forwarded.end() &&
            g.be->var_sym[static_cast<std::size_t>(a.ref)] == slot_sym;
        if (forwarded && !later_calls[i]) {
            continue;
        }
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
        Opnd slot = MemOpnd(SymValue(g.be->var_sym[static_cast<std::size_t>(slot_var[i])]),
                            pending[i].size, 1);
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
        // a copy of a small function counts against the size budget; a function called
        // once has no other copy
        bool noted = kind == InlineKind::Always;
        std::uint32_t bound_bytes = 0;
        for (std::uint32_t i = 0; i < e.count; i++) {
            std::size_t param = static_cast<std::size_t>(fn.params[i]);
            bound_bytes += bind[i] ? TypeOf(*g.program, g.program->vars[param].type).size : 0;
        }
        std::uint64_t cost_before = g.cost + g.routine_cost;
        std::uint64_t weight = LoopWeight(g);
        std::size_t start = noted ? StartCopyNote(g, {false, e.ref}) : 0;
        // the forwarded parameters read and written where their arguments went
        std::vector<std::int32_t> own_sym(e.count, -1);
        std::size_t forwarded = g.forwarded.size();
        for (std::uint32_t i = 0; i < e.count; i++) {
            std::size_t param = static_cast<std::size_t>(fn.params[i]);
            if (slot_var[i] != fn.params[i]) {
                own_sym[i] = g.be->var_sym[param];
                g.be->var_sym[param] = g.be->var_sym[static_cast<std::size_t>(slot_var[i])];
                g.forwarded.push_back(fn.params[i]);
            }
        }
        Opnd out = InlineBody(g, e.ref, size, dst);
        for (std::uint32_t i = e.count; i-- > 0;) {
            if (own_sym[i] >= 0) {
                g.be->var_sym[static_cast<std::size_t>(fn.params[i])] = own_sym[i];
            }
        }
        g.forwarded.resize(forwarded);
        if (noted) {
            EndCallNote(g, start, id, bound_bytes, cost_before, weight);
        }
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

} // namespace tisple
