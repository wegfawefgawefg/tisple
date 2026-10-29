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
// which then collapses (else the copies only cost bytes: an address the loop no longer
// changes is still worked out on every pass); run with unrolling once the other passes have
// settled (opt.cpp), as a last resort for the loops left.

#include "opt/collapse.hpp"
#include "opt/licm.hpp"
#include "opt/ranges.hpp"

namespace tisple::opt {

namespace {

constexpr int kMaxNodes = 120;
constexpr std::uint32_t kMaxCopies = 4;
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

// the first if or match among the body's items that may move out, or -1
std::int32_t FindSwitch(const Pass& p, ExprId loop, ExprId body) {
    const Expr& d = At(p, body);
    if (d.kind != ExprKind::Do || Shared(p, body)) {
        return -1;
    }
    LoopWrites w = FindLoopWrites(p, loop);
    if (w.all) {
        return -1;
    }
    for (std::uint32_t i = 0; i < d.count; i++) {
        ExprId item = Item(p, d, i);
        const Expr& e = At(p, item);
        bool branch = e.kind == ExprKind::If || e.kind == ExprKind::Match;
        if (!branch || e.type != kVoidType || Shared(p, item)) {
            continue;
        }
        // worked out even when the loop makes no pass: cheap, unless the loop surely runs
        bool cheap = BodyRuns(p, loop) || EstimatedCycles(p, e.a) <= kMaxGamble;
        if (cheap && IsPure(p, e.a) && Quiet(p, e.a) && !ReadsChanged(p, e.a, w)) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

// a copy of the loop whose body has arm (copied, or nothing when -1) in place of item at; its
// counter and the variables its lets declare are new locals of function
std::int32_t NewCopy(Pass& p, std::int32_t function, ExprId loop, std::uint32_t at, ExprId arm) {
    Expr e = At(p, loop);
    std::unordered_map<std::int32_t, std::int32_t> names;
    e.a = CloneExpr(p, e.a);
    e.b = e.b >= 0 ? CloneExpr(p, e.b) : -1;
    // the counter first: the bounds cannot read it, the body does
    names[e.ref] = CopyLocal(p, function, e.ref);
    Expr d = At(p, e.c); // a copy: the clones move every expression
    std::vector<ExprId> items;
    for (std::uint32_t i = 0; i < d.count; i++) {
        ExprId item = Item(p, d, i);
        if (i != at) {
            items.push_back(CloneExpr(p, item, function, names));
        } else if (arm >= 0) {
            items.push_back(CloneExpr(p, arm, function, names));
        }
    }
    e.ref = names[e.ref];
    e.c = NewList(p, ExprKind::Do, kVoidType, items, d.loc);
    return NewExpr(p, e);
}

} // namespace

bool UnswitchLoop(Pass& p, MemoryFacts& m, std::int32_t function, ExprId loop,
                  std::vector<ExprId>& out) {
    Expr e = At(p, loop); // a copy: the copies move every expression
    if (e.kind != ExprKind::For || Size(p, loop, kMaxNodes + 1) > kMaxNodes) {
        return false;
    }
    std::int32_t at = FindSwitch(p, loop, e.c);
    if (at < 0) {
        return false;
    }
    std::uint32_t index = static_cast<std::uint32_t>(at);
    Expr branch = At(p, Item(p, At(p, e.c), index));
    std::uint32_t copies = branch.kind == ExprKind::If ? 2 : branch.count + 1;
    if (copies > kMaxCopies) {
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
    // worth it when a copy's passes then all do the same (collapse runs it once); else the
    // copies only cost bytes
    bool collapses = false;
    for (ExprId copy : made) {
        const Expr& c = At(p, copy);
        collapses = collapses || PlanBody(p, m, c.c, c.ref, -1).same;
    }
    if (!collapses || !Allow(p, "loop unswitched", loop, e.ref)) {
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
