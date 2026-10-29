// Loop deletion: a while or for whose only effects are writes of tracked variables nobody
// reads after it, and that provably ends (terminate.cpp), is removed. Its condition, range
// and body may read anything, call pure functions, break and continue (to itself or loops
// inside it); a store through a pointer, I/O, jsr, a return, an exit to a loop outside it,
// or a node not known here keeps it. Timing goes through wait_cycles, never through empty
// loops (docs/language.md), so an empty counting loop is dead work.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

struct Scan {
    bool ok = true;
    std::vector<std::int32_t> writes; // vars set, let or looped over inside
};

// depth: loops entered inside the scanned one; -1 is the scanned loop's own head (its
// condition, start and end), where break and continue go to a loop outside
void ScanCode(const Pass& p, Scan& s, ExprId id, int depth) {
    if (id < 0 || !s.ok) {
        return;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Break:
    case ExprKind::Continue:
        s.ok = depth >= 0;
        return;
    case ExprKind::Set: {
        std::int32_t root = PlaceRoot(p, e.a);
        if (root < 0) {
            s.ok = false;
            return;
        }
        s.writes.push_back(root);
        break;
    }
    case ExprKind::Let:
        s.writes.push_back(e.ref);
        break;
    case ExprKind::While:
        ScanCode(p, s, e.a, depth);
        ScanCode(p, s, e.b, depth + 1);
        return;
    case ExprKind::For:
        s.writes.push_back(e.ref);
        ScanCode(p, s, e.a, depth);
        ScanCode(p, s, e.b, depth);
        ScanCode(p, s, e.c, depth + 1);
        return;
    case ExprKind::Call:
        s.ok = PureFunction(p, e.ref);
        break;
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::StructLit:
    case ExprKind::If:
    case ExprKind::Do:
    case ExprKind::Match:
        break;
    default:
        // stores, I/O, jsr, return, and anything not known here
        s.ok = false;
        return;
    }
    ScanCode(p, s, e.a, depth);
    ScanCode(p, s, e.b, depth);
    ScanCode(p, s, e.c, depth);
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            ScanCode(p, s, Item(p, e, i), depth);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            ScanCode(p, s, p.program->clauses[e.first + i].body, depth);
        }
    }
}

} // namespace

Action DecideLoop(const Pass& p, const Liveness& live, ExprId stmt) {
    const Expr& e = At(p, stmt);
    if (e.kind != ExprKind::While && e.kind != ExprKind::For) {
        return Action();
    }
    Scan s;
    if (e.kind == ExprKind::While) {
        ScanCode(p, s, e.a, -1);
        ScanCode(p, s, e.b, 0);
    } else {
        s.writes.push_back(e.ref);
        ScanCode(p, s, e.a, -1);
        ScanCode(p, s, e.b, -1);
        ScanCode(p, s, e.c, 0);
    }
    if (!s.ok) {
        return Action();
    }
    // every write lands in a variable this function tracks and nobody reads afterwards
    for (std::int32_t var : s.writes) {
        if (LiveAfter(live, stmt, var)) {
            return Action();
        }
    }
    if (!CodeTerminates(p, stmt)) {
        return Action();
    }
    Action a;
    a.kind = Action::Drop;
    a.what = e.kind == ExprKind::While ? "dead while" : "dead for";
    a.var = e.kind == ExprKind::For ? e.ref : -1;
    return a;
}

} // namespace tisple::opt
