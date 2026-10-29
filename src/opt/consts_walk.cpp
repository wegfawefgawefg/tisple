// The values of one function's expressions: a forward walk of the tree carrying, for each
// tracked scalar (live.cpp's variables, integers, bools and enums only), whether it holds one
// known value. Conditional in the tree's terms: an if, match, and/or or while whose condition
// the walk has decided runs only what it picks, so writes in arms that never run do not spoil
// what follows. Loops run to a fixed point (a variable only goes None, Const, Any, so a few
// passes do).
//
// - A function starts with its params and every tracked global unknown.
// - A call makes the globals its summary writes unknown; jsr, I/O in it, io_write, and nodes
//   this file does not know make every global unknown. An unknown node makes every variable
//   unknown, and may break or continue to any loop around it.
// - Stores through pointers never reach a tracked variable (pointer provenance).
// - A const array read at a known index is known.
// - Every value is masked to its type, as the interpreter and the 6502 hold it.

#include "check/arith.hpp"
#include "opt/consts.hpp"

namespace tisple::opt {

namespace {

struct Env {
    bool reached = false;
    std::vector<Val> vals; // by bit
};

struct LoopExits {
    Env exit; // where break goes
    Env next; // where continue goes
};

struct Walker {
    const Pass* p = nullptr;
    ConstFacts* facts = nullptr;
    std::vector<std::int32_t> bit; // by var: its bit, or -1 when not followed
    std::int32_t count = 0;
    std::vector<std::int32_t> globals; // the followed globals' vars
    std::vector<LoopExits> loops;
};

void JoinEnv(Env& into, const Env& from) {
    if (!from.reached) {
        return;
    }
    if (!into.reached) {
        into = from;
        return;
    }
    for (std::size_t i = 0; i < into.vals.size(); i++) {
        into.vals[i] = Join(into.vals[i], from.vals[i]);
    }
}

bool SameEnv(const Env& a, const Env& b) {
    return a.reached == b.reached && (!a.reached || a.vals == b.vals);
}

void Note(Walker& w, ExprId id, Val v) {
    std::vector<Val>& values = w.facts->values;
    if (static_cast<std::size_t>(id) < values.size()) {
        values[static_cast<std::size_t>(id)] = Join(values[static_cast<std::size_t>(id)], v);
    }
}

void ForgetGlobals(Walker& w, Env& env) {
    for (std::int32_t var : w.globals) {
        env.vals[static_cast<std::size_t>(w.bit[static_cast<std::size_t>(var)])] = AnyVal();
    }
}

// a node not known here: anything may change, and it may leave through any loop around it
void ForgetEverything(Walker& w, Env& env) {
    for (Val& v : env.vals) {
        v = AnyVal();
    }
    for (LoopExits& loop : w.loops) {
        JoinEnv(loop.exit, env);
        JoinEnv(loop.next, env);
    }
}

void Assign(Walker& w, Env& env, std::int32_t var, Val v) {
    std::int32_t bit = w.bit[static_cast<std::size_t>(var)];
    if (bit >= 0 && env.reached) {
        env.vals[static_cast<std::size_t>(bit)] = v.kind == Val::None ? AnyVal() : v;
    }
}

Val Walk(Walker& w, ExprId id, Env& env);

// the index expressions of a place (its root is neither read nor written here)
void WalkPlace(Walker& w, ExprId id, Env& env) {
    const Expr& e = At(*w.p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return;
    case ExprKind::Index:
        WalkPlace(w, e.a, env);
        Walk(w, e.b, env);
        return;
    case ExprKind::Field:
        WalkPlace(w, e.a, env);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        Walk(w, e.a, env);
        return;
    default:
        Walk(w, id, env);
        return;
    }
}

Val WalkBinary(Walker& w, const Expr& e, Env& env) {
    Val a = Walk(w, e.a, env);
    Val b = Walk(w, e.b, env);
    return BinaryValue(*w.p, e, a, b);
}

Val WalkShortCircuit(Walker& w, const Expr& e, Env& env) {
    Val a = Walk(w, e.a, env);
    if (!env.reached) {
        return Val();
    }
    bool is_and = e.kind == ExprKind::And;
    if (a.kind == Val::Const) {
        bool decided = is_and ? a.bits == 0 : a.bits != 0;
        if (decided) {
            return ConstOf(*w.p, e.type, is_and ? 0 : 1);
        }
        return Walk(w, e.b, env);
    }
    Env other = env;
    Walk(w, e.b, other);
    JoinEnv(env, other);
    return AnyVal();
}

Val WalkIf(Walker& w, const Expr& e, Env& env) {
    Val cond = Walk(w, e.a, env);
    if (!env.reached) {
        return Val();
    }
    if (cond.kind == Val::Const) {
        ExprId arm = cond.bits != 0 ? e.b : e.c;
        Val v = arm >= 0 ? Walk(w, arm, env) : AnyVal();
        return e.c >= 0 ? v : AnyVal();
    }
    Env other = env;
    Val then = Walk(w, e.b, env);
    Val other_value = e.c >= 0 ? Walk(w, e.c, other) : AnyVal();
    JoinEnv(env, other);
    return e.c >= 0 ? Join(then, other_value) : AnyVal();
}

Val WalkMatch(Walker& w, const Expr& e, Env& env) {
    const Program& program = *w.p->program;
    Val scrutinee = Walk(w, e.a, env);
    if (!env.reached) {
        return Val();
    }
    TypeKind kind = KindOf(program, At(*w.p, e.a).type);
    if (scrutinee.kind == Val::Const) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            const Clause& clause = program.clauses[e.first + i];
            if (clause.wildcard || MaskToKind(kind, clause.value) == scrutinee.bits) {
                return Walk(w, clause.body, env);
            }
        }
        return AnyVal();
    }
    Env out;
    Val value;
    bool wildcard = false;
    for (std::uint32_t i = 0; i < e.count; i++) {
        const Clause& clause = program.clauses[e.first + i];
        Env arm = env;
        value = Join(value, Walk(w, clause.body, arm));
        JoinEnv(out, arm);
        wildcard = wildcard || clause.wildcard;
    }
    if (!wildcard) {
        JoinEnv(out, env); // no clause taken
    }
    env = out;
    return wildcard ? value : AnyVal();
}

// passes before the walk gives up and calls everything unknown
std::int32_t PassLimit(const Walker& w) {
    return 3 * w.count + 8;
}

void Unknown(Env& env) {
    for (Val& v : env.vals) {
        v = AnyVal();
    }
}

void WalkWhile(Walker& w, const Expr& e, Env& env) {
    Env entry = env;
    Env head = entry;
    for (std::int32_t pass = 0;; pass++) {
        Env h = head;
        Val cond = Walk(w, e.a, h);
        bool runs = h.reached && !(cond.kind == Val::Const && cond.bits == 0);
        bool leaves = h.reached && !(cond.kind == Val::Const && cond.bits != 0);
        w.loops.push_back(LoopExits());
        Env body = h;
        body.reached = runs;
        if (runs) {
            Walk(w, e.b, body);
        }
        LoopExits exits = w.loops.back();
        w.loops.pop_back();
        JoinEnv(body, exits.next);
        Env next = entry;
        JoinEnv(next, body);
        if (pass > PassLimit(w)) {
            Unknown(next);
        }
        if (SameEnv(next, head)) {
            env = Env();
            env.vals = entry.vals;
            if (leaves) {
                env = h;
            }
            JoinEnv(env, exits.exit);
            return;
        }
        head = next;
    }
}

void WalkFor(Walker& w, const Expr& e, Env& env) {
    Walk(w, e.a, env);
    Walk(w, e.b, env);
    if (!env.reached) {
        return;
    }
    // the counter is set at each pass from a range worked out once
    Env entry = env;
    Assign(w, entry, e.ref, AnyVal());
    Env head = entry;
    for (std::int32_t pass = 0;; pass++) {
        w.loops.push_back(LoopExits());
        Env body = head;
        Walk(w, e.c, body);
        LoopExits exits = w.loops.back();
        w.loops.pop_back();
        JoinEnv(body, exits.next);
        Assign(w, body, e.ref, AnyVal());
        Env next = entry;
        JoinEnv(next, body);
        if (pass > PassLimit(w)) {
            Unknown(next);
        }
        if (SameEnv(next, head)) {
            env = head;
            JoinEnv(env, exits.exit);
            return;
        }
        head = next;
    }
}

Val WalkCall(Walker& w, const Expr& e, Env& env) {
    for (std::uint32_t i = 0; i < e.count; i++) {
        Walk(w, Item(*w.p, e, i), env);
    }
    if (!env.reached) {
        return Val();
    }
    const Summary& s = w.p->summaries[static_cast<std::size_t>(e.ref)];
    if (s.io || s.jsr || s.unknown) {
        ForgetGlobals(w, env);
        return AnyVal();
    }
    for (std::int32_t var : w.globals) {
        if (static_cast<std::size_t>(var) < s.writes.size() &&
            s.writes[static_cast<std::size_t>(var)]) {
            Assign(w, env, var, AnyVal());
        }
    }
    return AnyVal();
}

void WalkChildren(Walker& w, const Expr& e, Env& env) {
    Walk(w, e.a, env);
    Walk(w, e.b, env);
    Walk(w, e.c, env);
}

Val WalkNode(Walker& w, const Expr& e, Env& env) {
    const Pass& p = *w.p;
    switch (e.kind) {
    case ExprKind::Literal:
        return ConstType(*p.program, e.type) ? ConstOf(p, e.type, e.value) : AnyVal();
    case ExprKind::VarRef: {
        std::int32_t bit = w.bit[static_cast<std::size_t>(e.ref)];
        if (bit < 0 || env.vals[static_cast<std::size_t>(bit)].kind == Val::None) {
            return AnyVal();
        }
        return env.vals[static_cast<std::size_t>(bit)];
    }
    case ExprKind::Index: {
        WalkPlace(w, e.a, env);
        Val k = Walk(w, e.b, env);
        return ElementValue(p, e, k);
    }
    case ExprKind::Field:
    case ExprKind::AddrOf:
        WalkPlace(w, e.a, env);
        return AnyVal();
    case ExprKind::Unary:
        return UnaryValue(p, e, Walk(w, e.a, env));
    case ExprKind::Convert:
        return ConvertValue(p, e, Walk(w, e.a, env));
    case ExprKind::Binary:
        return WalkBinary(w, e, env);
    case ExprKind::And:
    case ExprKind::Or:
        return WalkShortCircuit(w, e, env);
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Store:
    case ExprKind::StoreField:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Reinterpret:
    case ExprKind::IoRead:
    case ExprKind::JsrRegs:
        WalkChildren(w, e, env);
        return AnyVal();
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
        WalkChildren(w, e, env);
        if (env.reached) {
            ForgetGlobals(w, env);
        }
        return AnyVal();
    case ExprKind::Call:
        return WalkCall(w, e, env);
    case ExprKind::StructLit:
        for (std::uint32_t i = 0; i < e.count; i++) {
            Walk(w, Item(p, e, i), env);
        }
        return AnyVal();
    case ExprKind::If:
        return WalkIf(w, e, env);
    case ExprKind::Match:
        return WalkMatch(w, e, env);
    case ExprKind::Do: {
        Val v = AnyVal();
        for (std::uint32_t i = 0; i < e.count && env.reached; i++) {
            v = Walk(w, Item(p, e, i), env);
        }
        return v;
    }
    case ExprKind::While:
        WalkWhile(w, e, env);
        return AnyVal();
    case ExprKind::For:
        WalkFor(w, e, env);
        return AnyVal();
    case ExprKind::Return:
        Walk(w, e.a, env);
        env.reached = false;
        return Val();
    case ExprKind::Break:
    case ExprKind::Continue:
        if (!w.loops.empty()) {
            LoopExits& loop = w.loops.back();
            JoinEnv(e.kind == ExprKind::Break ? loop.exit : loop.next, env);
        }
        env.reached = false;
        return Val();
    case ExprKind::Let:
        Assign(w, env, e.ref, Walk(w, e.a, env));
        return AnyVal();
    case ExprKind::Set: {
        WalkPlace(w, e.a, env);
        Val v = Walk(w, e.b, env);
        const Expr& place = At(p, e.a);
        if (place.kind == ExprKind::VarRef) {
            Assign(w, env, place.ref, v);
        }
        return AnyVal();
    }
    default:
        ForgetEverything(w, env);
        return AnyVal();
    }
}

Val Walk(Walker& w, ExprId id, Env& env) {
    if (id < 0 || !env.reached) {
        return Val();
    }
    // a copy: the walk never edits the tree, but the vector may not move under a reference
    Expr e = At(*w.p, id);
    Val v = WalkNode(w, e, env);
    if (!env.reached) {
        v = Val();
    }
    Note(w, id, v);
    return v;
}

} // namespace

bool ConstType(const Program& program, TypeId type) {
    TypeKind kind = KindOf(program, type);
    return IsIntegerKind(kind) || kind == TypeKind::Bool || kind == TypeKind::Enum;
}

Val ValueOf(const ConstFacts& facts, ExprId id) {
    if (id < 0 || static_cast<std::size_t>(id) >= facts.values.size()) {
        return Val();
    }
    return facts.values[static_cast<std::size_t>(id)];
}

void FindConstants(const Pass& p, std::int32_t function, ConstFacts& facts) {
    const Program& program = *p.program;
    Walker w;
    w.p = &p;
    w.facts = &facts;
    w.bit.assign(program.vars.size(), -1);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (TrackedVar(var, function) && ConstType(program, var.type)) {
            w.bit[v] = w.count++;
            if (var.kind == VarKind::Global) {
                w.globals.push_back(static_cast<std::int32_t>(v));
            }
        }
    }
    Env env;
    env.reached = true;
    env.vals.assign(static_cast<std::size_t>(w.count), AnyVal());
    Walk(w, program.functions[static_cast<std::size_t>(function)].body, env);
}

} // namespace tisple::opt
