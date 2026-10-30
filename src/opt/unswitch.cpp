// Loop unswitching (docs/midlevel-optimizer.md, slice 5): a for whose body branches on what the
// loop never changes becomes a branch with a copy of the loop in each arm, the branch item
// replaced by the arm that copy takes, so the passes of each copy can collapse or go:
//   (for i u8 0 6 (match e (E.e0 (set (at a i) 1)) (E.e1 (do))))
//   -> (match e (E.e0 (for i u8 0 6 (set (at a i) 1))) (E.e1 (for i u8 0 6 (do))))
// The branch is the body's first if or match item (a statement) whose condition is pure, reads
// no hardware or memory through a pointer, and reads nothing the loop writes: worked out once
// before the loop, it gives what it gave on every pass (and it may be worked out with no pass
// at all, which pure code may; so when the loop may make none, only a cheap one). A match with
// no wildcard gets one, its copy without the item.
// Only for loops of at most kMaxNodes nodes and at most kMaxCopies copies, at least one of
// which then collapses, or (slice 6) with no calls in the loop (copies of a call stop the
// backend inlining it) and no arm that sets a variable an index reads (the copy where it no
// longer changes works out the same address on every pass, which the backend does not keep:
// seed 1239 3056 to 5527 cycles); run with unrolling once the other passes have settled
// (opt.cpp), as a last resort for the loops left. Seed 1491 261182 to 1342 cycles.

#include "opt/collapse.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

constexpr int kMaxNodes = 120;
constexpr std::uint32_t kMaxCopies = 4;
// more copies (a match's clauses, most of them empty: their copies go) while all of them come
// to at most this many nodes
constexpr std::uint32_t kMaxMoreCopies = 6;
constexpr int kMaxCopiedNodes = 300;
// the cycles a condition may cost when the loop may make no pass, which it then costs for
// nothing (a compare of variables, not a division)
constexpr std::int32_t kMaxGamble = 30;

int Size(const Pass& p, ExprId id, int limit) {
    if (id < 0 || limit <= 0) {
        return 0;
    }
    int n = 1;
    EachChild(p, At(p, id), [&](ExprId child) {
        if (n <= limit) {
            n += Size(p, child, limit - n);
        }
    });
    return n;
}

// reads a variable the loop writes (or, when it stores through pointers, one whose address
// is taken)
bool ReadsChanged(const Pass& p, ExprId id, const LoopWrites& w) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef) {
        const Var& v = VarOf(p, e.ref);
        return w.vars[static_cast<std::size_t>(e.ref)] || (w.stores && v.address_taken);
    }
    bool reads = false;
    EachChild(p, e, [&](ExprId child) { reads = reads || ReadsChanged(p, child, w); });
    return reads;
}

// the first if or match that may move out of the loop: among body's items, or (slice 6) the
// items of the bodies of fors among them, at any depth (seed 359's match in a nest); -1 for
// none. runs: the loop and every for on the way surely make a pass, so the test costs
// nothing it would not have
ExprId FindBranch(const Pass& p, ExprId body, const LoopWrites& w, bool runs) {
    const Expr& d = At(p, body);
    if (d.kind != ExprKind::Do || Shared(p, body)) {
        return -1;
    }
    for (std::uint32_t i = 0; i < d.count; i++) {
        ExprId item = Item(p, d, i);
        const Expr& e = At(p, item);
        if (Shared(p, item)) {
            continue;
        }
        if (e.kind == ExprKind::For) {
            ExprId inner = FindBranch(p, e.c, w, runs && BodyRuns(p, item));
            if (inner >= 0) {
                return inner;
            }
            continue;
        }
        bool branch = e.kind == ExprKind::If || e.kind == ExprKind::Match;
        if (!branch || e.type != kVoidType) {
            continue;
        }
        // worked out even when the loop makes no pass: cheap, unless the loop surely runs
        bool cheap = runs || EstimatedCycles(p, e.a) <= kMaxGamble;
        if (cheap && IsPure(p, e.a) && Quiet(p, e.a) && !ReadsChanged(p, e.a, w)) {
            return item;
        }
    }
    return -1;
}

ExprId FindSwitch(const Pass& p, ExprId loop, ExprId body) {
    LoopWrites w = FindLoopWrites(p, loop);
    return w.all ? -1 : FindBranch(p, body, w, BodyRuns(p, loop));
}

bool Contains(const Pass& p, ExprId id, ExprId target) {
    if (id == target) {
        return true;
    }
    bool found = false;
    EachChild(p, At(p, id), [&](ExprId child) { found = found || Contains(p, child, target); });
    return found;
}

// a copy of id with arm (copied, or nothing when -1) in place of target; the dos and fors on
// the way to it are copied node by node, the variables lets and fors declare new locals of
// function
ExprId CopySwap(Pass& p, std::int32_t function, ExprId id, ExprId target, ExprId arm,
                std::unordered_map<std::int32_t, std::int32_t>& names) {
    if (id == target) {
        return arm >= 0 ? CloneExpr(p, arm, function, names) : NewEmptyDo(p, At(p, id).loc);
    }
    if (!Contains(p, id, target)) {
        return CloneExpr(p, id, function, names);
    }
    Expr e = At(p, id); // a Do or a For (FindBranch goes through nothing else)
    if (e.kind == ExprKind::For) {
        if (names.find(e.ref) == names.end()) {
            names[e.ref] = CopyLocal(p, function, e.ref);
        }
        e.a = CloneExpr(p, e.a, function, names);
        e.b = e.b >= 0 ? CloneExpr(p, e.b, function, names) : -1;
        e.c = CopySwap(p, function, e.c, target, arm, names);
        e.ref = names[e.ref];
        return NewExpr(p, e);
    }
    std::vector<ExprId> items;
    for (std::uint32_t i = 0; i < e.count; i++) {
        items.push_back(CopySwap(p, function, Item(p, At(p, id), i), target, arm, names));
    }
    return NewList(p, ExprKind::Do, e.type, items, e.loc);
}

// a copy of the loop with arm (copied, or nothing when -1) in place of the branch at target;
// its counter and the variables its lets and fors declare are new locals of function
std::int32_t NewCopy(Pass& p, std::int32_t function, ExprId loop, ExprId target, ExprId arm) {
    Expr e = At(p, loop);
    std::unordered_map<std::int32_t, std::int32_t> names;
    e.a = CloneExpr(p, e.a);
    e.b = e.b >= 0 ? CloneExpr(p, e.b) : -1;
    // the counter first: the bounds cannot read it, the body does
    names[e.ref] = CopyLocal(p, function, e.ref);
    e.c = CopySwap(p, function, e.c, target, arm, names);
    e.ref = names[e.ref];
    return NewExpr(p, e);
}

// calls under id (copies of a call stop the backend inlining it)
bool HasCalls(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr) {
        return true;
    }
    bool calls = false;
    EachChild(p, e, [&](ExprId child) { calls = calls || HasCalls(p, child); });
    return calls;
}

// the variables index expressions under id read, into out
void IndexVars(const Pass& p, ExprId id, bool in_index, std::vector<std::int32_t>& out) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && in_index) {
        out.push_back(e.ref);
    }
    if (e.kind == ExprKind::Index) {
        IndexVars(p, e.a, in_index, out);
        IndexVars(p, e.b, true, out);
        return;
    }
    EachChild(p, e, [&](ExprId child) { IndexVars(p, child, in_index, out); });
}

// a set or let under id of one of vars
bool SetsAny(const Pass& p, ExprId id, const std::vector<std::int32_t>& vars) {
    const Expr& e = At(p, id);
    std::int32_t var = e.kind == ExprKind::Set   ? PlaceRoot(p, e.a)
                       : e.kind == ExprKind::Let ? e.ref
                                                 : -1;
    if (var >= 0 && std::find(vars.begin(), vars.end(), var) != vars.end()) {
        return true;
    }
    bool sets = false;
    EachChild(p, e, [&](ExprId child) { sets = sets || SetsAny(p, child, vars); });
    return sets;
}

// worth copies though none collapses: the branch's test goes out of every pass; not with
// calls in the loop, nor when an arm changes an index (the copy that does not would work out
// the same address on every pass, which the backend does not keep)
bool Plain(const Pass& p, ExprId loop, const Expr& branch) {
    if (HasCalls(p, loop)) {
        return false;
    }
    std::vector<std::int32_t> indexes;
    IndexVars(p, loop, false, indexes);
    bool sets = false;
    EachChild(p, branch, [&](ExprId child) { sets = sets || SetsAny(p, child, indexes); });
    return !sets;
}

} // namespace

bool UnswitchLoop(Pass& p, MemoryFacts& m, std::int32_t function, ExprId loop,
                  std::vector<ExprId>& out) {
    Expr e = At(p, loop); // a copy: the copies move every expression
    int size = Size(p, loop, kMaxNodes + 1);
    if (e.kind != ExprKind::For || size > kMaxNodes) {
        return false;
    }
    ExprId index = FindSwitch(p, loop, e.c);
    if (index < 0) {
        return false;
    }
    Expr branch = At(p, index);
    std::uint32_t copies = branch.kind == ExprKind::If ? 2 : branch.count + 1;
    bool more = copies <= kMaxMoreCopies && static_cast<int>(copies) * size <= kMaxCopiedNodes;
    if (copies > kMaxCopies && !more) {
        return false;
    }
    // the copies, one per arm or clause (and one without the item for a match with no
    // wildcard)
    std::vector<ExprId> made;
    std::vector<Clause> clauses;
    if (branch.kind == ExprKind::If) {
        made = {NewCopy(p, function, loop, index, branch.b),
                NewCopy(p, function, loop, index, branch.c)};
    } else {
        bool wildcard = false;
        for (std::uint32_t i = 0; i < branch.count; i++) {
            Clause clause = p.program->clauses[branch.first + i];
            clause.body = NewCopy(p, function, loop, index, clause.body);
            wildcard = wildcard || clause.wildcard;
            clauses.push_back(clause);
            made.push_back(clause.body);
        }
        if (!wildcard) {
            Clause rest;
            rest.wildcard = true;
            rest.body = NewCopy(p, function, loop, index, -1);
            rest.loc = branch.loc;
            clauses.push_back(rest);
            made.push_back(rest.body);
        }
    }
    // worth it when a copy's passes then all do the same (collapse runs it once), or when the
    // test alone leaves every pass (Plain)
    bool collapses = false;
    for (ExprId copy : made) {
        const Expr& c = At(p, copy);
        collapses = collapses || PlanBody(p, m, c.c, c.ref, -1).same;
    }
    if ((!collapses && !Plain(p, loop, branch)) || !Allow(p, "loop unswitched", loop, e.ref)) {
        for (ExprId copy : made) {
            Retain(p, copy);
            Release(p, copy); // named by nobody: gives up its parts
        }
        return false;
    }
    Expr outer = branch;
    outer.a = CloneExpr(p, branch.a);
    if (branch.kind == ExprKind::If) {
        outer.b = made[0];
        outer.c = made[1];
    } else {
        outer.first = static_cast<std::uint32_t>(p.program->clauses.size());
        outer.count = static_cast<std::uint32_t>(clauses.size());
        p.program->clauses.insert(p.program->clauses.end(), clauses.begin(), clauses.end());
    }
    out.push_back(NewExpr(p, outer));
    return true;
}

} // namespace tisple::opt
