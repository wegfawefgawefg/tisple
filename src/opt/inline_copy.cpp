// Inlining's copies (inline.cpp decides which calls): a call's body as the caller's code, its
// returns made the arms they end, its parameters lets of the arguments or the arguments read
// in place, its locals new locals of the caller.

#include "opt/collapse.hpp"
#include "opt/inline.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

bool HasReturn(const Pass& p, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Return) {
        return true;
    }
    bool found = false;
    EachChild(p, e, [&](ExprId child) { found = found || HasReturn(p, child); });
    return found;
}

// every way through id ends in a return
bool AlwaysReturns(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Return:
        return true;
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (AlwaysReturns(p, Item(p, e, i))) {
                return true;
            }
        }
        return false;
    case ExprKind::If:
        return e.c >= 0 && AlwaysReturns(p, e.b) && AlwaysReturns(p, e.c);
    default:
        return false;
    }
}

// one copy of a body being made, or (build false) only looked at
struct Copier {
    Pass* p = nullptr;
    std::int32_t caller = -1;
    std::unordered_map<std::int32_t, std::int32_t> names; // callee var -> caller var
    TypeId type = kVoidType;                              // the callee's return type
    CopyMode mode = CopyMode::Value;
    std::int32_t dest = -1; // SetVar: the variable
    bool build = false;
    bool ok = true;
};

ExprId Copy(Copier& c, ExprId id) {
    return c.build ? CloneExpr(*c.p, id, c.caller, c.names) : -1;
}

// what a value at the end of the body becomes (v -1: a return with none)
void Finish(Copier& c, ExprId v, std::vector<ExprId>& out) {
    if (v < 0 || !c.build) {
        return;
    }
    ExprId copy = Copy(c, v);
    if (c.mode != CopyMode::SetVar) {
        out.push_back(copy);
        return;
    }
    Expr place;
    place.kind = ExprKind::VarRef;
    place.type = VarOf(*c.p, c.dest).type;
    place.ref = c.dest;
    place.loc = At(*c.p, v).loc;
    Expr set;
    set.kind = ExprKind::Set;
    set.type = kVoidType;
    set.a = NewExpr(*c.p, place);
    set.b = copy;
    set.loc = place.loc;
    out.push_back(NewExpr(*c.p, set));
}

TypeId ResultType(const Copier& c) {
    return c.mode == CopyMode::Value ? c.type : kVoidType;
}

ExprId MakeDo(Copier& c, const std::vector<ExprId>& out, const Loc& loc) {
    return c.build ? NewList(*c.p, ExprKind::Do, ResultType(c), out, loc) : -1;
}

// items, the rest of the body from some point, with no return left: a do of their copies
ExprId Lower(Copier& c, std::vector<ExprId> items, Loc loc) {
    const Pass& p = *c.p;
    std::vector<ExprId> out;
    for (std::size_t k = 0; k < items.size() && c.ok; k++) {
        ExprId id = items[k];
        bool last = k + 1 == items.size();
        if (!HasReturn(p, id)) {
            if (last) {
                Finish(c, id, out);
            } else if (c.build) {
                out.push_back(Copy(c, id));
            }
            continue;
        }
        Expr e = At(p, id);
        if (e.kind == ExprKind::Return) {
            Finish(c, e.a, out);
            return MakeDo(c, out, loc);
        }
        if (e.kind == ExprKind::Do) {
            // its items join these (its lets get new locals, so a longer scope does no harm)
            std::vector<ExprId> inner(p.program->lists.begin() + e.first,
                                      p.program->lists.begin() + e.first + e.count);
            items.erase(items.begin() + static_cast<std::ptrdiff_t>(k));
            items.insert(items.begin() + static_cast<std::ptrdiff_t>(k), inner.begin(),
                         inner.end());
            k--;
            continue;
        }
        if (e.kind != ExprKind::If || HasReturn(p, e.a)) {
            c.ok = false;
            return -1;
        }
        bool then_returns = AlwaysReturns(p, e.b);
        bool else_returns = e.c >= 0 && AlwaysReturns(p, e.c);
        bool then_some = HasReturn(p, e.b);
        bool else_some = e.c >= 0 && HasReturn(p, e.c);
        if ((then_some && !then_returns) || (else_some && !else_returns)) {
            c.ok = false; // an arm that may or may not return: the rest would need two copies
            return -1;
        }
        std::vector<ExprId> rest(items.begin() + static_cast<std::ptrdiff_t>(k) + 1, items.end());
        std::vector<ExprId> a = {e.b};
        std::vector<ExprId> b;
        if (e.c >= 0) {
            b.push_back(e.c);
        }
        if (!then_returns) {
            a.insert(a.end(), rest.begin(), rest.end());
        }
        if (!else_returns) {
            b.insert(b.end(), rest.begin(), rest.end());
        }
        ExprId cond = Copy(c, e.a);
        ExprId arm_a = Lower(c, a, At(p, e.b).loc);
        ExprId arm_b = Lower(c, b, e.loc);
        if (c.build && c.ok) {
            Expr branch;
            branch.kind = ExprKind::If;
            branch.type = ResultType(c);
            branch.a = cond;
            branch.b = arm_a;
            branch.c = arm_b;
            branch.loc = e.loc;
            out.push_back(NewExpr(*c.p, branch));
        }
        return MakeDo(c, out, loc);
    }
    return MakeDo(c, out, loc);
}

// an argument read where the parameter is (a few nodes copied to each read) at most this
// many nodes in all
constexpr int kMaxPutIn = 8;

int Nodes(const Pass& p, ExprId id) {
    int n = 1;
    EachChild(p, At(p, id), [&](ExprId child) { n += Nodes(p, child); });
    return n;
}

// reads of var under id; under a loop of the body they count as many
int Reads(const Pass& p, ExprId id, std::int32_t var, int loops) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && e.ref == var) {
        return loops > 0 ? kMaxPutIn + 1 : 1;
    }
    bool loop = e.kind == ExprKind::While || e.kind == ExprKind::For;
    int n = 0;
    EachChild(p, e, [&](ExprId child) { n += Reads(p, child, var, loops + (loop ? 1 : 0)); });
    return n;
}

// nothing the body does changes what id reads
bool Unchanged(const Pass& p, const Summary& s, ExprId id) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef) {
        const Var& v = VarOf(p, e.ref);
        bool global = v.kind == VarKind::Global && s.writes[static_cast<std::size_t>(e.ref)];
        return !global && !(v.address_taken && s.stores) && !Hardware(v);
    }
    bool same = true;
    EachChild(p, e, [&](ExprId child) { same = same && Unchanged(p, s, child); });
    return same;
}

// may the argument stand where the body reads the parameter, as the backend binds an
// inlined call's arguments (CanBind): the parameter never set, its address not taken; the
// argument pure and quiet, reading nothing the body or the later arguments change; a literal
// or a variable for any number of reads, else a few nodes read a few times (not in a loop)
bool PutIn(const Pass& p, std::int32_t callee, ExprId call, std::uint32_t i) {
    const Function& fn = p.program->functions[static_cast<std::size_t>(callee)];
    std::int32_t param = fn.params[i];
    ExprId arg = Item(p, At(p, call), i);
    const Summary& s = p.summaries[static_cast<std::size_t>(callee)];
    if (VarOf(p, param).address_taken || CountSets(p, fn.body, param) > 0 || s.jsr || s.unknown ||
        !IsPure(p, arg) || !Quiet(p, arg) || !Unchanged(p, s, arg)) {
        return false;
    }
    ExprKind kind = At(p, arg).kind;
    if (kind != ExprKind::Literal && kind != ExprKind::VarRef &&
        Nodes(p, arg) * Reads(p, fn.body, param, 0) > kMaxPutIn) {
        return false;
    }
    for (std::uint32_t j = i + 1; j < At(p, call).count; j++) {
        if (!IsPure(p, Item(p, At(p, call), j))) {
            return false;
        }
    }
    return true;
}

// every read of var under id becomes a copy of value
void PutValue(Pass& p, ExprId id, std::int32_t var, ExprId value) {
    Expr e = At(p, id);
    if (e.kind == ExprKind::VarRef && e.ref == var) {
        ExprId copy = CloneExpr(p, value);
        Retain(p, copy);
        ReplaceWith(p, id, copy);
        Release(p, copy);
        return;
    }
    EachChild(p, e, [&](ExprId child) { PutValue(p, child, var, value); });
}

// the locals of function that id names (reads, sets, lets and fors), into out
void Locals(const Pass& p, ExprId id, std::int32_t function, std::vector<std::int32_t>& out) {
    const Expr& e = At(p, id);
    bool names = e.kind == ExprKind::VarRef || e.kind == ExprKind::Let || e.kind == ExprKind::For;
    if (names && VarOf(p, e.ref).kind == VarKind::Local && VarOf(p, e.ref).function == function &&
        std::find(out.begin(), out.end(), e.ref) == out.end()) {
        out.push_back(e.ref);
    }
    EachChild(p, e, [&](ExprId child) { Locals(p, child, function, out); });
}

} // namespace

// may the body become a copy with no returns (looked at without building one)
bool Lowerable(Pass& p, std::int32_t callee) {
    const Function& fn = p.program->functions[static_cast<std::size_t>(callee)];
    const Expr& body = At(p, fn.body);
    if (body.kind != ExprKind::Do) {
        return false;
    }
    Copier c;
    c.p = &p;
    c.type = fn.ret;
    std::vector<ExprId> items(p.program->lists.begin() + body.first,
                              p.program->lists.begin() + body.first + body.count);
    Lower(c, items, body.loc);
    return c.ok;
}

// the copy for a call: its lets of parameters, then the body's items, into out
void CopyCall(Pass& p, std::int32_t caller, ExprId call, CopyMode mode, std::int32_t dest,
              std::vector<ExprId>& out) {
    Expr e = At(p, call);
    const Function& fn = p.program->functions[static_cast<std::size_t>(e.ref)];
    Copier c;
    c.p = &p;
    c.caller = caller;
    c.type = fn.ret;
    c.mode = mode;
    c.dest = dest;
    c.build = true;
    std::vector<std::pair<std::int32_t, ExprId>> put; // parameters read as their arguments
    for (std::uint32_t i = 0; i < e.count && i < fn.params.size(); i++) {
        std::int32_t param = fn.params[i];
        ExprId arg = Item(p, e, i);
        if (PutIn(p, e.ref, call, i)) {
            if (At(p, arg).kind == ExprKind::VarRef) {
                c.names[param] = At(p, arg).ref;
            } else {
                put.push_back({param, arg});
            }
            continue;
        }
        std::int32_t local = CopyLocal(p, caller, param);
        p.program->vars[static_cast<std::size_t>(local)].kind = VarKind::Local;
        c.names[param] = local;
        Expr let;
        let.kind = ExprKind::Let;
        let.type = kVoidType;
        let.ref = local;
        let.a = arg; // the call lets go of it once replaced
        let.loc = At(p, arg).loc;
        out.push_back(NewExpr(p, let));
    }
    // every local of the callee the body names gets one of the caller's, a set of one whose
    // let the passes took away too (in the callee's frame it would share bytes with others)
    std::vector<std::int32_t> locals;
    Locals(p, fn.body, e.ref, locals);
    for (std::int32_t local : locals) {
        if (c.names.find(local) == c.names.end()) {
            c.names[local] = CopyLocal(p, caller, local);
        }
    }
    const Expr& body = At(p, fn.body);
    std::vector<ExprId> items(p.program->lists.begin() + body.first,
                              p.program->lists.begin() + body.first + body.count);
    ExprId lowered = Lower(c, items, e.loc);
    for (const auto& [param, arg] : put) {
        PutValue(p, lowered, param, arg);
    }
    const Expr& d = At(p, lowered);
    out.insert(out.end(), p.program->lists.begin() + d.first,
               p.program->lists.begin() + d.first + d.count);
    // the do made for the items names them; out names them now
    for (ExprId item : out) {
        Retain(p, item);
    }
    Retain(p, lowered);
    Release(p, lowered);
}

} // namespace tisple::opt
