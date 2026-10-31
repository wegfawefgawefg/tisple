// The switches of with_aux followed through the tree (aux_mode.hpp): a forward walk in
// evaluation order whose state is the switches that may be on, joined where control flow
// meets and run to a fixed point around loops; a return, break or continue ends a path. Each
// call passes its state on to its callee, so the walks go round until no function's entry
// state grows (the call graph has no cycles, so callers-first order settles in a round or two).

#include "check/aux_mode.hpp"

namespace tisple {

namespace {

struct Flow {
    bool live = true; // false: no path gets here
    std::uint8_t bits = 0;
};

Flow Dead() {
    Flow f;
    f.live = false;
    return f;
}

Flow Join(Flow a, Flow b) {
    if (!a.live) {
        return b;
    }
    if (!b.live) {
        return a;
    }
    Flow f;
    f.bits = static_cast<std::uint8_t>(a.bits | b.bits);
    return f;
}

bool Same(Flow a, Flow b) {
    return a.live == b.live && a.bits == b.bits;
}

struct LoopExits {
    Flow breaks = Dead();
    Flow continues = Dead();
};

struct Walker {
    const Program* program = nullptr;
    AuxModes* modes = nullptr;
    std::int32_t function = -1;
    std::vector<LoopExits> loops;
    bool changed = false;
};

const Expr& At(const Walker& w, ExprId id) {
    return w.program->exprs[static_cast<std::size_t>(id)];
}

ExprId Item(const Walker& w, const Expr& e, std::uint32_t i) {
    return w.program->lists[e.first + i];
}

Flow Walk(Walker& w, ExprId id, Flow in);

// a callee runs with the state its call is made in
void Calls(Walker& w, std::int32_t callee, ExprId site, Flow at) {
    if (!at.live || at.bits == 0 || callee < 0) {
        return;
    }
    std::size_t c = static_cast<std::size_t>(callee);
    std::uint8_t now = static_cast<std::uint8_t>(w.modes->entry[c] | at.bits);
    if (now != w.modes->entry[c]) {
        w.modes->entry[c] = now;
        w.changed = true;
    }
    if (w.modes->entry_site[c] < 0) {
        w.modes->entry_site[c] = site;
    }
}

Flow WalkItems(Walker& w, const Expr& e, Flow in) {
    for (std::uint32_t i = 0; i < e.count; i++) {
        in = Walk(w, Item(w, e, i), in);
    }
    return in;
}

// a loop body run from `start` until the state at its top stops growing; `cond` is the while
// test worked out at the top of each pass (-1 for a for). Gives the state after the loop
Flow WalkLoop(Walker& w, ExprId cond, ExprId body, Flow start) {
    Flow top = start;
    Flow after = Dead();
    for (int round = 0; round < 8; round++) {
        w.loops.emplace_back();
        Flow tested = cond >= 0 ? Walk(w, cond, top) : top;
        Flow end = Walk(w, body, tested);
        LoopExits exits = w.loops.back();
        w.loops.pop_back();
        after = Join(tested, exits.breaks);
        Flow next = Join(start, Join(end, exits.continues));
        if (Same(next, top)) {
            break;
        }
        top = next;
    }
    return after;
}

Flow WalkSwitch(Walker& w, const Expr& e, Flow in) {
    Flow x = Walk(w, e.a, in);
    if (e.value == 0) {
        x.bits = 0;
        return x;
    }
    x = Walk(w, e.b, x);
    std::uint8_t bits = AuxOnBits(*w.program, e);
    std::size_t f = static_cast<std::size_t>(w.function);
    w.modes->owns[f] = static_cast<std::uint8_t>(w.modes->owns[f] | bits);
    w.modes->any = true;
    x.bits = bits;
    return x;
}

Flow Walk(Walker& w, ExprId id, Flow in) {
    if (id < 0 || !in.live) {
        return in;
    }
    std::uint8_t& state = w.modes->state[static_cast<std::size_t>(id)];
    state = static_cast<std::uint8_t>(state | in.bits);
    const Expr& e = At(w, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::FnRef:
        return in;
    case ExprKind::Do:
        return WalkItems(w, e, in);
    case ExprKind::If: {
        Flow c = Walk(w, e.a, in);
        Flow then = Walk(w, e.b, c);
        Flow other = e.c >= 0 ? Walk(w, e.c, c) : c;
        return Join(then, other);
    }
    case ExprKind::Match: {
        Flow s = Walk(w, e.a, in);
        Flow out = Dead();
        bool wildcard = false;
        for (std::uint32_t i = 0; i < e.count; i++) {
            const Clause& clause = w.program->clauses[e.first + i];
            wildcard = wildcard || clause.wildcard;
            out = Join(out, Walk(w, clause.body, s));
        }
        return wildcard ? out : Join(out, s);
    }
    case ExprKind::And:
    case ExprKind::Or: {
        Flow x = Walk(w, e.a, in);
        return Join(x, Walk(w, e.b, x));
    }
    case ExprKind::While:
        return WalkLoop(w, e.a, e.b, in);
    case ExprKind::For: {
        Flow x = Walk(w, e.a, in);
        x = Walk(w, e.b, x);
        return WalkLoop(w, -1, e.c, x);
    }
    case ExprKind::Return:
        Walk(w, e.a, in);
        return Dead();
    case ExprKind::Break:
        if (!w.loops.empty()) {
            w.loops.back().breaks = Join(w.loops.back().breaks, in);
        }
        return Dead();
    case ExprKind::Continue:
        if (!w.loops.empty()) {
            w.loops.back().continues = Join(w.loops.back().continues, in);
        }
        return Dead();
    case ExprKind::Call: {
        Flow x = WalkItems(w, e, in);
        Calls(w, e.ref, id, x);
        return x;
    }
    case ExprKind::CallPtr: {
        Flow x = Walk(w, e.a, in);
        x = WalkItems(w, e, x);
        for (std::int32_t target : CallTargets(*w.program, At(w, e.a).type)) {
            Calls(w, target, id, x);
        }
        return x;
    }
    case ExprKind::AuxSwitch:
        return WalkSwitch(w, e, in);
    default: {
        Flow x = Walk(w, e.a, in);
        x = Walk(w, e.b, x);
        x = Walk(w, e.c, x);
        if (e.kind == ExprKind::StructLit) {
            x = WalkItems(w, e, x);
        }
        return x;
    }
    }
}

// functions with every caller (by the callees lists) before them; the rest after
std::vector<std::int32_t> CallersFirst(const Program& program) {
    std::size_t n = program.functions.size();
    std::vector<std::uint32_t> callers(n, 0);
    for (const Function& fn : program.functions) {
        for (std::int32_t callee : fn.callees) {
            callers[static_cast<std::size_t>(callee)]++;
        }
    }
    std::vector<std::int32_t> order;
    std::vector<bool> placed(n, false);
    for (std::size_t f = 0; f < n; f++) {
        if (callers[f] == 0) {
            order.push_back(static_cast<std::int32_t>(f));
            placed[f] = true;
        }
    }
    for (std::size_t i = 0; i < order.size(); i++) {
        for (std::int32_t callee : program.functions[static_cast<std::size_t>(order[i])].callees) {
            std::size_t c = static_cast<std::size_t>(callee);
            if (--callers[c] == 0 && !placed[c]) {
                order.push_back(callee);
                placed[c] = true;
            }
        }
    }
    for (std::size_t f = 0; f < n; f++) {
        if (!placed[f]) {
            order.push_back(static_cast<std::int32_t>(f));
        }
    }
    return order;
}

} // namespace

std::uint8_t AuxOnBits(const Program& program, const Expr& on) {
    std::uint8_t bits = 0;
    std::uint16_t value = 0;
    if (!(ConstValue(program, on.a, value) && value == 0)) {
        bits |= kAuxReads;
    }
    if (!(ConstValue(program, on.b, value) && value == 0)) {
        bits |= kAuxWrites;
    }
    return bits;
}

AuxModes FindAuxModes(const Program& program) {
    AuxModes modes;
    std::size_t n = program.functions.size();
    modes.entry.assign(n, 0);
    modes.entry_site.assign(n, -1);
    modes.owns.assign(n, 0);
    modes.state.assign(program.exprs.size(), 0);
    std::vector<std::int32_t> order = CallersFirst(program);
    Walker w;
    w.program = &program;
    w.modes = &modes;
    for (int round = 0; round < 64; round++) {
        w.changed = false;
        for (std::int32_t f : order) {
            const Function& fn = program.functions[static_cast<std::size_t>(f)];
            if (fn.body < 0) {
                continue;
            }
            w.function = f;
            w.loops.clear();
            Flow in;
            in.bits = modes.entry[static_cast<std::size_t>(f)];
            Walk(w, fn.body, in);
        }
        if (!w.changed) {
            break;
        }
    }
    return modes;
}

bool HasAuxSwitch(const Program& program, std::int32_t function) {
    const Function& fn = program.functions[static_cast<std::size_t>(function)];
    std::vector<ExprId> work;
    if (fn.body >= 0) {
        work.push_back(fn.body);
    }
    while (!work.empty()) {
        ExprId id = work.back();
        work.pop_back();
        const Expr& e = program.exprs[static_cast<std::size_t>(id)];
        if (e.kind == ExprKind::AuxSwitch && e.value != 0) {
            return true;
        }
        for (ExprId child : {e.a, e.b, e.c}) {
            if (child >= 0) {
                work.push_back(child);
            }
        }
        bool items = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ||
                     e.kind == ExprKind::StructLit || e.kind == ExprKind::Do;
        for (std::uint32_t i = 0; items && i < e.count; i++) {
            work.push_back(program.lists[e.first + i]);
        }
        for (std::uint32_t i = 0; e.kind == ExprKind::Match && i < e.count; i++) {
            work.push_back(program.clauses[e.first + i].body);
        }
    }
    return false;
}

} // namespace tisple
