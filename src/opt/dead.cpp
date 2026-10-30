// Dead code: a let or set whose variable nobody reads before it is written again (or ever,
// for a local; before the function returns, for a global), (set x x), and statements with
// no effect. Only tracked variables (live.cpp) have dead stores: stores to address-taken
// variables, :address lets (screens, soft switches, the race's checksum), through pointers,
// and io_write and jsr all stay. A dead store whose value has an effect keeps the value as a
// statement of its own.

#include "backend/analysis.hpp"
#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

bool IsScalar(const Pass& p, ExprId id) {
    return IsScalarKind(KindOf(*p.program, At(p, id).type));
}

Action Make(Action::Kind kind, const char* what, std::int32_t var, ExprId with = -1) {
    Action a;
    a.kind = kind;
    a.what = what;
    a.var = var;
    a.with = with;
    return a;
}

// a store nobody reads: gone, or only its value's effects left
Action DeadValue(const Pass& p, ExprId value, const char* what, std::int32_t var) {
    if (IsPure(p, value)) {
        return Make(Action::Drop, what, var);
    }
    // a scalar value as a statement, as code may write it; a do with a value never is one
    if (IsScalar(p, value) && At(p, value).kind != ExprKind::Do) {
        return Make(Action::Replace, what, var, value);
    }
    return Action();
}

// (set x x), (set s.a s.a), (set (at a i) (at a i)): the same bytes written back
bool SelfAssignment(const Pass& p, const Expr& set, std::int32_t root) {
    const Var& var = VarOf(p, root);
    if (var.placement.has_address || var.placement.in_aux || var.irq_shared) {
        return false; // hardware, or the interrupt handler, may see the write
    }
    return SameExpr(*p.program, set.a, set.b) && PurePlace(p, set.a) && IsPure(p, set.b);
}

} // namespace

Action DecideDead(const Pass& p, const Liveness& live, ExprId stmt) {
    const Expr& e = At(p, stmt);
    if (e.kind == ExprKind::Let) {
        if (LiveAfter(live, stmt, e.ref)) {
            return Action();
        }
        return DeadValue(p, e.a, "dead let", e.ref);
    }
    if (e.kind == ExprKind::Set) {
        std::int32_t root = PlaceRoot(p, e.a);
        if (root < 0) {
            return Action();
        }
        if (!LiveAfter(live, stmt, root) && PurePlace(p, e.a)) {
            return DeadValue(p, e.b, "dead store", root);
        }
        if (SelfAssignment(p, e, root)) {
            return Make(Action::Drop, "self-assignment", root);
        }
        return Action();
    }
    if (IsPure(p, stmt)) {
        return Make(Action::Drop, "statement with no effect", -1);
    }
    return Action();
}

} // namespace tisple::opt
