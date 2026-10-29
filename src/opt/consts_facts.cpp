// Facts from conditions, for constant propagation (consts_walk.cpp): inside
// (if (= x 5) ...) x is 5, and in the else arm of (if (!= x 5) ...); a bool variable tested
// is false in the else arm (and after (if flag (return))); (not c) turns them round,
// (and a b) taken gives a's and b's, (or a b) not taken gives both theirs. The walk applies
// them only to conditions that are pure (nothing in them changes x after it is read), and
// only to variables it follows.

#include "opt/consts.hpp"

namespace tisple::opt {

void FindEqualities(const Pass& p, ExprId cond, bool truth, std::vector<Equality>& out) {
    if (cond < 0) {
        return;
    }
    const Expr& e = At(p, cond);
    switch (e.kind) {
    case ExprKind::Unary:
        if (e.op == Op::Not) {
            FindEqualities(p, e.a, !truth, out);
        }
        return;
    case ExprKind::And:
        if (truth) {
            FindEqualities(p, e.a, true, out);
            FindEqualities(p, e.b, true, out);
        }
        return;
    case ExprKind::Or:
        if (!truth) {
            FindEqualities(p, e.a, false, out);
            FindEqualities(p, e.b, false, out);
        }
        return;
    case ExprKind::VarRef:
        // (if flag ...): flag is false in the else arm (in the then arm it is not 0, which a
        // bool loaded from memory need not hold as 1)
        if (KindOf(*p.program, e.type) == TypeKind::Bool && !truth) {
            out.push_back(Equality{e.ref, -1, false});
        }
        return;
    case ExprKind::Binary: {
        bool equal = (e.op == Op::Eq && truth) || (e.op == Op::Ne && !truth);
        if (!equal || At(p, e.a).type != At(p, e.b).type) {
            return;
        }
        if (At(p, e.a).kind == ExprKind::VarRef) {
            out.push_back(Equality{At(p, e.a).ref, e.b});
        }
        if (At(p, e.b).kind == ExprKind::VarRef) {
            out.push_back(Equality{At(p, e.b).ref, e.a});
        }
        return;
    }
    default:
        return;
    }
}

} // namespace tisple::opt
