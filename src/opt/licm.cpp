// Loop-invariant code motion (docs/midlevel-optimizer.md, pass 4). A pure expression in a
// loop whose inputs the loop never changes (licm_scan.cpp) is worked out once, into a new
// local let just before the loop, and the loop reads the local. Outer loops go first, so an
// expression moves out of as many loops as it can in one step; what only an inner loop
// leaves alone moves on the way down.
//
// - From a while's condition: always, since the condition runs whenever the loop is reached.
// - Not out of a loop that makes one pass.
// - From the body: when the body surely runs (BodyRuns, KnownTrips), else behind a guard that
//   tests the loop's condition once more, so a loop that runs zero times does not pay:
//     (if (< start end) (do (let t work) (for i T start end ...t...)))
//     (if cond (do (let t work) (while cond ...t...)))
//   A bound too big to work out twice moves into a let both read. A range or condition that
//   is not pure (or too big to test twice) keeps the body's work where it is. Constant
//   propagation cuts guards it can decide.
// - The same expression twice in a loop is worked out once.
// - The loop's own range (a for's start and end) is worked out once already: left alone.

#include "opt/licm.hpp"

#include "backend/analysis.hpp"

namespace tisple::opt {

namespace {

// a condition a guard tests again has at most this many nodes
constexpr std::uint32_t kMaxGuardNodes = 12;

struct Hoister {
    Pass* p = nullptr;
    std::int32_t function = -1;
    std::uint64_t changes = 0;
};

// (let var value) for an expression moved out of its place, which now reads var
ExprId MoveToLet(Hoister& h, ExprId id, Loc loc, std::int32_t& var) {
    return MoveToLet(*h.p, h.function, id, loc, var);
}

struct Moved {
    ExprId value = -1; // the expression, now the let's value
    std::int32_t var = -1;
};

// the expressions moved into lets, in order (named by nobody yet)
void MoveAll(Hoister& h, ExprId loop, const std::vector<ExprId>& found, std::vector<Moved>& moved,
             std::vector<ExprId>& lets) {
    Pass& p = *h.p;
    for (ExprId c : found) {
        std::int32_t again = -1;
        for (const Moved& m : moved) {
            if (again < 0 && SameExpr(*p.program, m.value, c)) {
                again = m.var;
            }
        }
        if (again >= 0) {
            if (Allow(p, "invariant read again", c, again)) {
                BecomeRead(p, c, again);
                h.changes++;
            }
            continue;
        }
        if (!Allow(p, "invariant hoisted", c, -1)) {
            continue;
        }
        std::int32_t var = -1;
        ExprId let = MoveToLet(h, c, At(p, loop).loc, var);
        lets.push_back(let);
        moved.push_back(Moved{At(p, let).a, var});
        h.changes++;
    }
}

// a literal, or a read of a variable no hardware is behind: safe and cheap to read twice
bool Plain(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Literal) {
        return true;
    }
    if (e.kind != ExprKind::VarRef) {
        return false;
    }
    const Var& v = VarOf(p, e.ref);
    return !v.placement.has_address && !v.placement.in_aux && !v.irq_shared;
}

// small pure trees of plain reads and arithmetic: what a guard may test a second time
bool Copyable(const Pass& p, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
        return Plain(p, id);
    case ExprKind::Index:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::And:
    case ExprKind::Or:
        return Copyable(p, e.a) && Copyable(p, e.b);
    case ExprKind::Field:
        return Copyable(p, e.a);
    default:
        return false;
    }
}

ExprId Copy(Pass& p, ExprId id) {
    if (id < 0) {
        return -1;
    }
    Expr e = At(p, id);
    e.a = Copy(p, e.a);
    e.b = Copy(p, e.b);
    return NewExpr(p, e);
}

// may the loop's condition be tested once more before it
bool CanGuard(const Pass& p, ExprId loop) {
    const Expr& e = At(p, loop);
    if (e.kind == ExprKind::For) {
        return IsPure(p, e.a) && IsPure(p, e.b);
    }
    return IsPure(p, e.a) && Copyable(p, e.a) && CountExprs(*p.program, e.a) <= kMaxGuardNodes;
}

// a for's start or end for the guard to test: worked out again when it is small and plain
// (the for keeps its own, whose shape the backend may know, a byte widened say), else moved
// into a let both read
ExprId GuardBound(Hoister& h, ExprId loop, ExprId bound, std::vector<ExprId>& before) {
    Pass& p = *h.p;
    bool again = Copyable(p, bound) && CountExprs(*p.program, bound) <= kMaxGuardNodes;
    if (!again) {
        std::int32_t var = -1;
        before.push_back(MoveToLet(h, bound, At(p, loop).loc, var));
    }
    return Copy(p, bound);
}

// the test that the loop's body runs at least once: (< start end), or the condition again;
// statements it needs first go to before
ExprId GuardTest(Hoister& h, ExprId loop, std::vector<ExprId>& before) {
    Pass& p = *h.p;
    Expr e = At(p, loop);
    if (e.kind == ExprKind::While) {
        return Copy(p, e.a);
    }
    Expr test;
    test.kind = ExprKind::Binary;
    test.op = Op::Lt;
    test.type = kBoolType;
    test.loc = e.loc;
    test.a = GuardBound(h, loop, e.a, before);
    test.b = GuardBound(h, loop, e.b, before);
    return NewExpr(p, test);
}

// what stands where the loop stood: lets, then the loop (maybe in a guard); empty when
// nothing moved. before: the statement just before the loop, or -1
std::vector<ExprId> Hoist(Hoister& h, ExprId loop, ExprId before) {
    Pass& p = *h.p;
    std::vector<ExprId> out;
    // a loop that makes one pass (or none) has nothing to win
    std::int64_t trips = KnownTrips(p, loop, before);
    LoopWrites w = FindLoopWrites(p, loop);
    if (w.all || trips == 0 || trips == 1) {
        return out;
    }
    Invariants found;
    FindInvariants(p, loop, w, trips, found);
    std::vector<Moved> moved;
    MoveAll(h, loop, found.head, moved, out);
    bool guard = !found.body.empty() && trips < 0 && !BodyRuns(p, loop);
    if (guard && (!CanGuard(p, loop) || !Allow(p, "loop guarded", loop, -1))) {
        found.body.clear();
        guard = false;
    }
    std::vector<ExprId> inner;
    MoveAll(h, loop, found.body, moved, inner);
    if (out.empty() && inner.empty() && !guard) {
        return out;
    }
    inner.push_back(loop);
    if (!guard) {
        out.insert(out.end(), inner.begin(), inner.end());
        return out;
    }
    h.changes++;
    Expr branch;
    branch.kind = ExprKind::If;
    branch.type = kVoidType;
    branch.loc = At(p, loop).loc;
    branch.a = GuardTest(h, loop, out);
    branch.b = NewList(p, ExprKind::Do, kVoidType, inner, branch.loc);
    out.push_back(NewExpr(p, branch));
    return out;
}

bool IsLoop(const Pass& p, ExprId id) {
    ExprKind kind = At(p, id).kind;
    return kind == ExprKind::While || kind == ExprKind::For;
}

void Visit(Hoister& h, ExprId id, bool shared);

// the loops inside a loop just looked at, which keeps its id wherever it now stands
void VisitInside(Hoister& h, ExprId loop) {
    Expr e = At(*h.p, loop);
    EachChild(*h.p, e, [&](ExprId child) { Visit(h, child, false); });
}

// a loop that is an if's arm or a clause's body: what stands for it in a new do in that slot
ExprId HoistInSlot(Hoister& h, ExprId slot) {
    Pass& p = *h.p;
    std::vector<ExprId> stmts = Hoist(h, slot, -1);
    VisitInside(h, slot);
    if (stmts.empty()) {
        return slot;
    }
    ExprId wrap = NewList(p, ExprKind::Do, kVoidType, stmts, At(p, slot).loc);
    Retain(p, wrap);
    Release(p, slot); // what stands for it names it now
    return wrap;
}

void VisitDo(Hoister& h, ExprId id, bool shared) {
    Pass& p = *h.p;
    for (std::uint32_t i = 0; i < At(p, id).count; i++) {
        ExprId item = Item(p, At(p, id), i);
        if (shared || Shared(p, item) || !IsLoop(p, item)) {
            Visit(h, item, shared);
            continue;
        }
        ExprId before = i > 0 ? Item(p, At(p, id), i - 1) : -1;
        std::vector<ExprId> stmts = Hoist(h, item, before);
        VisitInside(h, item);
        if (stmts.empty()) {
            continue;
        }
        Expr d = At(p, id);
        std::vector<ExprId> items(p.program->lists.begin() + d.first,
                                  p.program->lists.begin() + d.first + d.count);
        items.erase(items.begin() + i);
        items.insert(items.begin() + i, stmts.begin(), stmts.end());
        for (ExprId stmt : stmts) {
            Retain(p, stmt);
        }
        Release(p, item); // the do named it here; what stands for it names it now
        Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
        out.first = static_cast<std::uint32_t>(p.program->lists.size());
        out.count = static_cast<std::uint32_t>(items.size());
        p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
        i += static_cast<std::uint32_t>(stmts.size()) - 1;
    }
}

void Visit(Hoister& h, ExprId id, bool shared) {
    if (id < 0) {
        return;
    }
    Pass& p = *h.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::Do:
        VisitDo(h, id, shared);
        return;
    case ExprKind::If:
        Visit(h, e.a, shared);
        for (int arm = 0; arm < 2; arm++) {
            ExprId slot = arm == 0 ? e.b : e.c;
            if (slot < 0) {
                continue;
            }
            if (shared || Shared(p, slot) || !IsLoop(p, slot)) {
                Visit(h, slot, shared);
                continue;
            }
            slot = HoistInSlot(h, slot);
            Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
            (arm == 0 ? out.b : out.c) = slot;
        }
        return;
    case ExprKind::Match:
        Visit(h, e.a, shared);
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId slot = p.program->clauses[e.first + i].body;
            if (shared || Shared(p, slot) || !IsLoop(p, slot)) {
                Visit(h, slot, shared);
                continue;
            }
            p.program->clauses[e.first + i].body = HoistInSlot(h, slot);
        }
        return;
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::Store:
    case ExprKind::LoadField:
    case ExprKind::StoreField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Call:
    case ExprKind::CallPtr:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::Return:
    case ExprKind::Let:
    case ExprKind::Set:
    case ExprKind::While:
    case ExprKind::For:
        EachChild(p, e, [&](ExprId child) { Visit(h, child, shared); });
        return;
    default:
        return; // not known here
    }
}

} // namespace

std::uint64_t HoistInvariants(Pass& p, std::int32_t function) {
    ExprId body = p.program->functions[static_cast<std::size_t>(function)].body;
    if (body < 0) {
        return 0;
    }
    Hoister h;
    h.p = &p;
    h.function = function;
    Visit(h, body, false);
    return h.changes;
}

} // namespace tisple::opt
