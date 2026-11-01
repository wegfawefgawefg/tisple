// What the dispatch says about when each function runs: overlay_guards.hpp.

#include "backend/overlay_guards.hpp"

#include "backend/analysis.hpp"
#include "backend/frames.hpp"

#include <algorithm>
#include <iterator>
#include <set>

namespace tisple {

namespace {

using Values = std::vector<std::uint16_t>;

Values Union(const Values& a, const Values& b) {
    Values out;
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

Values Intersection(const Values& a, const Values& b) {
    Values out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

Values Difference(const Values& a, const Values& b) {
    Values out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

GuardFact* FactOf(GuardFacts& facts, std::int32_t var) {
    for (GuardFact& fact : facts) {
        if (fact.var == var) {
            return &fact;
        }
    }
    return nullptr;
}

const GuardFact* FactOf(const GuardFacts& facts, std::int32_t var) {
    for (const GuardFact& fact : facts) {
        if (fact.var == var) {
            return &fact;
        }
    }
    return nullptr;
}

// both hold: what is known of the var gets narrower
void Add(GuardFacts& facts, const GuardFact& fact) {
    GuardFact* had = FactOf(facts, fact.var);
    if (had == nullptr) {
        auto at = std::find_if(facts.begin(), facts.end(),
                               [&](const GuardFact& f) { return f.var > fact.var; });
        facts.insert(at, fact);
        return;
    }
    if (had->in && fact.in) {
        had->values = Intersection(had->values, fact.values);
    } else if (had->in) {
        had->values = Difference(had->values, fact.values);
    } else if (fact.in) {
        had->values = Difference(fact.values, had->values);
        had->in = true;
    } else {
        had->values = Union(had->values, fact.values);
    }
}

void AddAll(GuardFacts& facts, const GuardFacts& more) {
    for (const GuardFact& fact : more) {
        Add(facts, fact);
    }
}

GuardFact One(std::int32_t var, std::uint16_t value, bool in) {
    GuardFact fact;
    fact.var = var;
    fact.values.push_back(value);
    fact.in = in;
    return fact;
}

// what holds on both paths: for each var both know something of, the wider of the two
GuardFacts Common(const GuardFacts& a, const GuardFacts& b) {
    GuardFacts out;
    for (const GuardFact& x : a) {
        const GuardFact* y = FactOf(b, x.var);
        if (y == nullptr) {
            continue;
        }
        GuardFact both;
        both.var = x.var;
        if (x.in && y->in) {
            both.values = Union(x.values, y->values);
        } else if (x.in) {
            both.in = false;
            both.values = Difference(y->values, x.values);
        } else if (y->in) {
            both.in = false;
            both.values = Difference(x.values, y->values);
        } else {
            both.in = false;
            both.values = Intersection(x.values, y->values);
        }
        if (both.in || !both.values.empty()) {
            out.push_back(both);
        }
    }
    return out;
}

void Forget(GuardFacts& facts, const std::set<std::int32_t>& vars) {
    if (vars.empty()) {
        return;
    }
    GuardFacts kept;
    for (const GuardFact& fact : facts) {
        if (!vars.count(fact.var)) {
            kept.push_back(fact);
        }
    }
    facts = kept;
}

struct Walk {
    const Program* program = nullptr;
    std::vector<bool> watched;                     // by var: a global facts may be about
    std::vector<std::set<std::int32_t>> fn_writes; // by function: the watched vars it may set
    // only the facts a loop's own dispatch makes: code inside a while or for of the function
    bool loops_only = false;
    int loops = 0; // loops around the code being walked, in its function
    // by function: its facts so far (the meet of its calls seen), and whether any call was seen
    std::vector<GuardFacts> facts;
    std::vector<bool> seen;
};

const Expr& E(const Walk& w, ExprId id) {
    return ExprOf(*w.program, id);
}

// a watched global read by name
std::int32_t WatchedVar(const Walk& w, ExprId id) {
    if (id < 0) {
        return -1;
    }
    const Expr& e = E(w, id);
    if (e.kind != ExprKind::VarRef || !w.watched[static_cast<std::size_t>(e.ref)]) {
        return -1;
    }
    return e.ref;
}

template <typename F> void ForChildren(const Walk& w, const Expr& e, F f) {
    for (ExprId child : {e.a, e.b, e.c}) {
        if (child >= 0) {
            f(child);
        }
    }
    switch (e.kind) {
    case ExprKind::Call:
    case ExprKind::CallPtr:
    case ExprKind::StructLit:
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            f(ListItem(*w.program, e, i));
        }
        return;
    case ExprKind::Match:
        for (std::uint32_t i = 0; i < e.count; i++) {
            f(w.program->clauses[e.first + i].body);
        }
        return;
    default:
        return;
    }
}

// the watched vars code may set: sets of them, and what the functions it calls may set
void ExprWrites(const Walk& w, ExprId id, std::set<std::int32_t>& out) {
    if (id < 0) {
        return;
    }
    const Expr& e = E(w, id);
    if (e.kind == ExprKind::Set) {
        std::int32_t var = WatchedVar(w, e.a);
        if (var >= 0) {
            out.insert(var);
        }
    }
    if (e.kind == ExprKind::Call) {
        const std::set<std::int32_t>& more = w.fn_writes[static_cast<std::size_t>(e.ref)];
        out.insert(more.begin(), more.end());
    }
    if (e.kind == ExprKind::CallPtr) {
        for (std::int32_t f : CallTargets(*w.program, E(w, e.a).type)) {
            const std::set<std::int32_t>& more = w.fn_writes[static_cast<std::size_t>(f)];
            out.insert(more.begin(), more.end());
        }
    }
    ForChildren(w, e, [&](ExprId child) { ExprWrites(w, child, out); });
}

// what holds when cond is `when`
GuardFacts CondFacts(const Walk& w, ExprId cond, bool when) {
    GuardFacts out;
    if (cond < 0) {
        return out;
    }
    const Expr& e = E(w, cond);
    std::int32_t var = WatchedVar(w, cond);
    if (var >= 0 && KindOf(*w.program, e.type) == TypeKind::Bool) {
        Add(out, One(var, 1, when));
        return out;
    }
    if (e.kind == ExprKind::Unary && e.op == Op::Not) {
        return CondFacts(w, e.a, !when);
    }
    if (e.kind == ExprKind::And && when) {
        out = CondFacts(w, e.a, true);
        AddAll(out, CondFacts(w, e.b, true));
        return out;
    }
    if (e.kind == ExprKind::Or && !when) {
        out = CondFacts(w, e.a, false);
        AddAll(out, CondFacts(w, e.b, false));
        return out;
    }
    if (e.kind != ExprKind::Binary || (e.op != Op::Eq && e.op != Op::Ne)) {
        return out;
    }
    std::uint16_t value = 0;
    std::int32_t left = WatchedVar(w, e.a);
    std::int32_t right = WatchedVar(w, e.b);
    if (left >= 0 && ConstValue(*w.program, e.b, value)) {
        Add(out, One(left, value, (e.op == Op::Eq) == when));
    } else if (right >= 0 && ConstValue(*w.program, e.a, value)) {
        Add(out, One(right, value, (e.op == Op::Eq) == when));
    }
    return out;
}

// code after which nothing runs: a return, a break, a continue, a do ending in one, an if
// both of whose branches are such
bool NeverGoesOn(const Walk& w, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = E(w, id);
    switch (e.kind) {
    case ExprKind::Return:
    case ExprKind::Break:
    case ExprKind::Continue:
        return true;
    case ExprKind::Do:
        return e.count > 0 && NeverGoesOn(w, ListItem(*w.program, e, e.count - 1));
    case ExprKind::If:
        return NeverGoesOn(w, e.b) && NeverGoesOn(w, e.c);
    default:
        return e.type == kNeverType;
    }
}

// the facts a branch on cond makes, when the walk takes them there
GuardFacts Dispatch(const Walk& w, ExprId cond, bool when) {
    if (w.loops_only && w.loops == 0) {
        return GuardFacts();
    }
    return CondFacts(w, cond, when);
}

void Visit(Walk& w, ExprId id, GuardFacts& facts);

void Call(Walk& w, std::int32_t callee, const GuardFacts& facts) {
    std::size_t f = static_cast<std::size_t>(callee);
    w.facts[f] = w.seen[f] ? Common(w.facts[f], facts) : facts;
    w.seen[f] = true;
}

// code in its order: facts holds what is known before it, and after it on return
void Visit(Walk& w, ExprId id, GuardFacts& facts) {
    if (id < 0) {
        return;
    }
    const Expr& e = E(w, id);
    switch (e.kind) {
    case ExprKind::If: {
        Visit(w, e.a, facts);
        GuardFacts then = facts;
        AddAll(then, Dispatch(w, e.a, true));
        Visit(w, e.b, then);
        GuardFacts other = facts;
        AddAll(other, Dispatch(w, e.a, false));
        Visit(w, e.c, other);
        bool then_ends = NeverGoesOn(w, e.b);
        bool else_ends = NeverGoesOn(w, e.c);
        if (then_ends && !else_ends) {
            facts = other;
        } else if (else_ends && !then_ends) {
            facts = then;
        } else {
            facts = Common(then, other);
        }
        return;
    }
    case ExprKind::And:
    case ExprKind::Or: {
        Visit(w, e.a, facts);
        GuardFacts rest = facts;
        AddAll(rest, Dispatch(w, e.a, e.kind == ExprKind::And));
        Visit(w, e.b, rest);
        std::set<std::int32_t> writes;
        ExprWrites(w, e.b, writes);
        Forget(facts, writes);
        return;
    }
    case ExprKind::Match: {
        Visit(w, e.a, facts);
        std::int32_t var = w.loops_only && w.loops == 0 ? -1 : WatchedVar(w, e.a);
        GuardFacts after;
        bool any = false;
        for (std::uint32_t i = 0; i < e.count; i++) {
            const Clause& clause = w.program->clauses[e.first + i];
            GuardFacts arm = facts;
            if (var >= 0 && !clause.wildcard) {
                Add(arm, One(var, clause.value, true));
            }
            for (std::uint32_t j = 0; j < e.count && var >= 0 && clause.wildcard; j++) {
                const Clause& other = w.program->clauses[e.first + j];
                if (!other.wildcard) {
                    Add(arm, One(var, other.value, false));
                }
            }
            Visit(w, clause.body, arm);
            if (!NeverGoesOn(w, clause.body)) {
                after = any ? Common(after, arm) : arm;
                any = true;
            }
        }
        facts = any ? after : facts;
        return;
    }
    case ExprKind::While:
    case ExprKind::For: {
        std::set<std::int32_t> writes;
        ExprWrites(w, id, writes);
        Forget(facts, writes);
        GuardFacts body = facts;
        w.loops++;
        if (e.kind == ExprKind::While) {
            Visit(w, e.a, body);
            AddAll(body, Dispatch(w, e.a, true));
            Visit(w, e.b, body);
        } else {
            Visit(w, e.a, body);
            Visit(w, e.b, body);
            Visit(w, e.c, body);
        }
        w.loops--;
        return;
    }
    case ExprKind::Set: {
        Visit(w, e.b, facts);
        std::int32_t var = WatchedVar(w, e.a);
        if (var < 0) {
            Visit(w, e.a, facts);
            return;
        }
        Forget(facts, {var});
        return;
    }
    case ExprKind::Call: {
        for (std::uint32_t i = 0; i < e.count; i++) {
            Visit(w, ListItem(*w.program, e, i), facts);
        }
        Call(w, e.ref, facts);
        Forget(facts, w.fn_writes[static_cast<std::size_t>(e.ref)]);
        return;
    }
    case ExprKind::CallPtr: {
        ForChildren(w, e, [&](ExprId child) { Visit(w, child, facts); });
        std::set<std::int32_t> writes;
        ExprWrites(w, id, writes);
        Forget(facts, writes);
        return;
    }
    default:
        ForChildren(w, e, [&](ExprId child) { Visit(w, child, facts); });
        return;
    }
}

} // namespace

bool Contradict(const GuardFacts& a, const GuardFacts& b) {
    for (const GuardFact& x : a) {
        const GuardFact* y = FactOf(b, x.var);
        if (y == nullptr) {
            continue;
        }
        bool never = false;
        if (x.in && y->in) {
            never = Intersection(x.values, y->values).empty();
        } else if (x.in) {
            never = Difference(x.values, y->values).empty();
        } else if (y->in) {
            never = Difference(y->values, x.values).empty();
        }
        if (never) {
            return true;
        }
    }
    return false;
}

static std::vector<GuardFacts> FindFacts(const Program& program, bool loops_only) {
    std::size_t count = program.functions.size();
    Walk w;
    w.program = &program;
    w.loops_only = loops_only;
    w.watched.assign(program.vars.size(), false);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        bool scalar = IsScalarKind(KindOf(program, var.type));
        w.watched[v] = var.kind == VarKind::Global && scalar && !var.address_taken &&
                       !var.irq_shared && !var.placement.has_address;
    }
    // what each function may set, its callees first
    std::vector<std::int32_t> order = CallersFirst(program);
    w.fn_writes.assign(count, {});
    for (std::size_t i = order.size(); i-- > 0;) {
        std::size_t f = static_cast<std::size_t>(order[i]);
        ExprWrites(w, program.functions[f].body, w.fn_writes[f]);
        for (std::int32_t callee : program.functions[f].callees) {
            const std::set<std::int32_t>& more = w.fn_writes[static_cast<std::size_t>(callee)];
            w.fn_writes[f].insert(more.begin(), more.end());
        }
    }
    // each function walked with what holds at all its calls, its callers first
    w.facts.assign(count, {});
    w.seen.assign(count, false);
    std::vector<GuardFacts> out(count);
    for (std::int32_t id : order) {
        std::size_t f = static_cast<std::size_t>(id);
        const Function& fn = program.functions[f];
        bool open = id == program.main || id == program.handler || fn.address_taken || !w.seen[f];
        GuardFacts facts = open ? GuardFacts() : w.facts[f];
        out[f] = facts;
        Visit(w, fn.body, facts);
    }
    return out;
}

Guards FindGuards(const Program& program) {
    Guards guards;
    guards.functions = FindFacts(program, false);
    guards.screens = FindFacts(program, true);
    return guards;
}

} // namespace tisple
