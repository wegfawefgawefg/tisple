// What a loop body reaches through pointers, for loop collapse, under the provenance rule
// (docs/language.md): a pointer made by addr_of reaches only that variable, arithmetic on it
// stays inside, one made from a number reaches only memory no variable uses. A pointer is
// followed through a local that one let sets and nothing else writes, and a parameter nothing
// in its function writes to what its calls pass, and a call to what its function returns; any
// other pointer (a global, one loaded from memory) may reach anything.
// Worked out on demand: only for the pointers and functions of loops that need it.

#include "opt/collapse.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

// how deep a pointer is followed through locals before it counts as reaching anything
constexpr int kMaxFollow = 8;

void Join(Reach& to, const Reach& from) {
    to.any = to.any || from.any;
    to.undeclared = to.undeclared || from.undeclared;
    for (std::int32_t var : from.vars) {
        if (std::find(to.vars.begin(), to.vars.end(), var) == to.vars.end()) {
            to.vars.push_back(var);
        }
    }
}

Reach Anything() {
    Reach r;
    r.any = true;
    r.undeclared = true;
    return r;
}

// the one let of local var in its function, when nothing else writes it; -1 otherwise
void FindLet(const Pass& p, ExprId id, std::int32_t var, ExprId& let, int& writes) {
    if (id < 0 || writes > 1) {
        return;
    }
    const Expr& e = At(p, id);
    if ((e.kind == ExprKind::Let || e.kind == ExprKind::For) && e.ref == var) {
        writes++;
        let = e.kind == ExprKind::Let ? id : -1;
    }
    if (e.kind == ExprKind::Set && PlaceRoot(p, e.a) == var) {
        writes += 2;
    }
    EachChild(p, e, [&](ExprId child) { FindLet(p, child, var, let, writes); });
}

Reach PointerReach(const Pass& p, MemoryFacts& m, ExprId id, int depth);
Reach ReturnReach(const Pass& p, MemoryFacts& m, std::int32_t function, int depth);

// the calls of each function, found once a round when a parameter is first followed
void FindCalls(const Pass& p, MemoryFacts& m) {
    if (!m.calls.empty()) {
        return;
    }
    m.calls.assign(p.program->functions.size(), {});
    for (std::size_t id = 0; id < p.program->exprs.size(); id++) {
        const Expr& e = At(p, static_cast<ExprId>(id));
        if (e.kind == ExprKind::Call && p.refs[id] > 0) {
            m.calls[static_cast<std::size_t>(e.ref)].push_back(static_cast<ExprId>(id));
        }
    }
}

// a parameter nothing in its function writes: what every call passes it may reach
Reach ParamReach(const Pass& p, MemoryFacts& m, std::int32_t var, int depth) {
    const Var& v = VarOf(p, var);
    const Function& fn = p.program->functions[static_cast<std::size_t>(v.function)];
    ExprId let = -1;
    int writes = 0;
    FindLet(p, fn.body, var, let, writes);
    FindCalls(p, m);
    const std::vector<ExprId>& calls = m.calls[static_cast<std::size_t>(v.function)];
    std::size_t index = 0;
    while (index < fn.params.size() && fn.params[index] != var) {
        index++;
    }
    // a function nothing calls is called from outside, with any pointer
    if (writes != 0 || calls.empty()) {
        return Anything();
    }
    Reach r;
    for (ExprId call : calls) {
        const Expr& e = At(p, call);
        if (index >= e.count) {
            return Anything();
        }
        Join(r, PointerReach(p, m, Item(p, e, static_cast<std::uint32_t>(index)), depth + 1));
    }
    return r;
}

Reach LocalReach(const Pass& p, MemoryFacts& m, std::int32_t var, int depth) {
    const Var& v = VarOf(p, var);
    bool param = v.kind == VarKind::Param;
    if ((v.kind != VarKind::Local && !param) || v.address_taken || depth >= kMaxFollow) {
        return Anything();
    }
    auto known = m.locals.find(var);
    if (known != m.locals.end()) {
        return known->second;
    }
    Reach r;
    if (param) {
        r = ParamReach(p, m, var, depth);
    } else {
        ExprId let = -1;
        int writes = 0;
        FindLet(p, p.program->functions[static_cast<std::size_t>(v.function)].body, var, let,
                writes);
        r = writes == 1 && let >= 0 ? PointerReach(p, m, At(p, let).a, depth + 1) : Anything();
    }
    m.locals[var] = r;
    return r;
}

Reach PointerReach(const Pass& p, MemoryFacts& m, ExprId id, int depth) {
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::AddrOf: {
        std::int32_t root = PlaceRoot(p, e.a);
        if (root < 0) {
            return Anything(); // into what a pointer reaches
        }
        Reach r;
        r.vars.push_back(root);
        return r;
    }
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return PointerReach(p, m, e.a, depth);
    case ExprKind::VarRef:
        return LocalReach(p, m, e.ref, depth);
    case ExprKind::Literal:
    case ExprKind::Reinterpret: {
        Reach r;
        r.undeclared = true;
        return r;
    }
    case ExprKind::If: {
        Reach r = PointerReach(p, m, e.b, depth);
        Join(r, e.c >= 0 ? PointerReach(p, m, e.c, depth) : Anything());
        return r;
    }
    case ExprKind::Do:
        return e.count == 0 ? Anything() : PointerReach(p, m, Item(p, e, e.count - 1), depth);
    case ExprKind::Call:
        return ReturnReach(p, m, e.ref, depth);
    default:
        return Anything();
    }
}

// the returns under id, joined into r
void JoinReturns(const Pass& p, MemoryFacts& m, ExprId id, int depth, Reach& r) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Return) {
        Join(r, e.a >= 0 ? PointerReach(p, m, e.a, depth) : Anything());
    }
    EachChild(p, e, [&](ExprId child) { JoinReturns(p, m, child, depth, r); });
}

// what a function's pointer value may reach: its body's value and its returns
Reach ReturnReach(const Pass& p, MemoryFacts& m, std::int32_t function, int depth) {
    if (depth >= kMaxFollow) {
        return Anything();
    }
    auto known = m.returns.find(function);
    if (known != m.returns.end()) {
        return known->second;
    }
    ExprId body = p.program->functions[static_cast<std::size_t>(function)].body;
    Reach r = PointerReach(p, m, body, depth + 1);
    JoinReturns(p, m, body, depth + 1, r);
    m.returns[function] = r;
    return r;
}

void Gather(const Pass& p, MemoryFacts& m, std::int32_t function);

// the loads and stores under id into a function's facts
void GatherExpr(const Pass& p, MemoryFacts& m, Reach& loads, Reach& stores, ExprId id) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Load:
    case ExprKind::LoadField:
        Join(loads, PointerReach(p, m, e.a, 0));
        break;
    case ExprKind::Store:
    case ExprKind::StoreField:
        Join(stores, PointerReach(p, m, e.a, 0));
        break;
    case ExprKind::Set:
        // a place behind a pointer, (set (at (load p) i) v), stores through p
        for (ExprId at = e.a; at >= 0;) {
            const Expr& place = At(p, at);
            if (place.kind == ExprKind::Load || place.kind == ExprKind::LoadField) {
                Join(stores, PointerReach(p, m, place.a, 0));
                break;
            }
            if (place.kind != ExprKind::Index && place.kind != ExprKind::Field) {
                break;
            }
            at = place.a;
        }
        break;
    case ExprKind::Call: {
        Gather(p, m, e.ref);
        std::size_t callee = static_cast<std::size_t>(e.ref);
        Join(loads, m.loads[callee]);
        Join(stores, m.stores[callee]);
        break;
    }
    default:
        break;
    }
    EachChild(p, e, [&](ExprId child) { GatherExpr(p, m, loads, stores, child); });
}

void Gather(const Pass& p, MemoryFacts& m, std::int32_t function) {
    std::size_t f = static_cast<std::size_t>(function);
    if (m.loads.empty()) {
        m.loads.assign(p.program->functions.size(), Reach());
        m.stores.assign(p.program->functions.size(), Reach());
        m.done.assign(p.program->functions.size(), false);
    }
    if (m.done[f]) {
        return;
    }
    // no recursion: a call graph with a cycle never comes back here before done is set
    m.done[f] = true;
    Reach loads;
    Reach stores;
    GatherExpr(p, m, loads, stores, p.program->functions[f].body);
    m.loads[f] = loads;
    m.stores[f] = stores;
}

bool Reaches(const Pass& p, const Reach& r, std::int32_t var) {
    return std::find(r.vars.begin(), r.vars.end(), var) != r.vars.end() ||
           (r.any && VarOf(p, var).address_taken);
}

bool Meet(const Pass& p, const Reach& x, const Reach& y) {
    if (x.any && y.any) {
        return true;
    }
    for (std::int32_t var : x.vars) {
        if (Reaches(p, y, var)) {
            return true;
        }
    }
    for (std::int32_t var : y.vars) {
        if (Reaches(p, x, var)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool MemoryClash(const Pass& p, MemoryFacts& m, const MemoryUse& use) {
    Reach loads;
    Reach stores;
    for (ExprId ptr : use.load_ptrs) {
        Join(loads, PointerReach(p, m, ptr, 0));
    }
    for (ExprId ptr : use.store_ptrs) {
        Join(stores, PointerReach(p, m, ptr, 0));
    }
    for (std::int32_t f : use.calls) {
        Gather(p, m, f);
        Join(loads, m.loads[static_cast<std::size_t>(f)]);
        Join(stores, m.stores[static_cast<std::size_t>(f)]);
    }
    if (loads.undeclared || stores.undeclared || Meet(p, loads, stores)) {
        return true;
    }
    for (std::int32_t var : use.written) {
        if (Reaches(p, loads, var)) {
            return true;
        }
    }
    for (std::int32_t var : use.read) {
        if (Reaches(p, stores, var)) {
            return true;
        }
    }
    return false;
}

} // namespace tisple::opt
