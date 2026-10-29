// A loop body's (if c (continue)) items, for loop collapse (collapse.cpp): the continue skips
// the rest of the pass, so the rest becomes the else, (if (not c) (do rest...)), and the last
// item, which skips nothing, goes. The body then ends each pass at its end, which collapse
// and the counting loops' rules need:
//   (for i T 0 4 (set g K) (if (!= a b) (continue)) (set h L))
//   -> (for i T 0 4 (set g K) (if (not (!= a b)) (do (set h L))))
// Only when every continue of the loop (not of a loop inside it) is such an item, its c pure.

#include "opt/collapse.hpp"

namespace tisple::opt {

namespace {

// (if c (continue)) or (if c (do (continue))), c pure, not shared
bool SkipsRest(const Pass& p, ExprId id) {
    const Expr& e = At(p, id);
    if (e.kind != ExprKind::If || e.c >= 0 || Shared(p, id) || !IsPure(p, e.a)) {
        return false;
    }
    const Expr& arm = At(p, e.b);
    if (arm.kind == ExprKind::Do && arm.count == 1) {
        return At(p, Item(p, arm, 0)).kind == ExprKind::Continue;
    }
    return arm.kind == ExprKind::Continue;
}

ExprId NewNot(Pass& p, ExprId cond) {
    Expr e;
    e.kind = ExprKind::Unary;
    e.op = Op::Not;
    e.type = kBoolType;
    e.a = cond;
    e.loc = At(p, cond).loc;
    return NewExpr(p, e);
}

// the first skip among do's items becomes the else of the rest, and so on in the rest
std::uint64_t Rewrite(Pass& p, ExprId id, ExprId loop) {
    Expr d = At(p, id);
    std::vector<ExprId> items(p.program->lists.begin() + d.first,
                              p.program->lists.begin() + d.first + d.count);
    std::size_t at = 0;
    while (at < items.size() && !SkipsRest(p, items[at])) {
        at++;
    }
    if (at == items.size() || !Allow(p, "continue made an else", loop, -1)) {
        return 0;
    }
    ExprId skip = items[at];
    std::vector<ExprId> rest(items.begin() + static_cast<std::ptrdiff_t>(at) + 1, items.end());
    std::vector<ExprId> kept(items.begin(), items.begin() + static_cast<std::ptrdiff_t>(at));
    std::uint64_t changes = 1;
    if (!rest.empty()) {
        ExprId tail = NewList(p, ExprKind::Do, kVoidType, rest, At(p, skip).loc);
        changes += Rewrite(p, tail, loop);
        Expr branch;
        branch.kind = ExprKind::If;
        branch.type = kVoidType;
        branch.loc = At(p, skip).loc;
        branch.a = NewNot(p, At(p, skip).a);
        branch.b = tail;
        ExprId made = NewExpr(p, branch);
        Retain(p, made);
        kept.push_back(made);
    }
    Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
    out.first = static_cast<std::uint32_t>(p.program->lists.size());
    out.count = static_cast<std::uint32_t>(kept.size());
    p.program->lists.insert(p.program->lists.end(), kept.begin(), kept.end());
    // the do names none of these now (the new else names the rest)
    for (std::size_t i = at; i < items.size(); i++) {
        Release(p, items[i]);
    }
    return changes;
}

} // namespace

std::uint64_t ContinuesToElse(Pass& p, ExprId loop) {
    const Expr& e = At(p, loop);
    ExprId body = e.kind == ExprKind::For ? e.c : e.b;
    const Expr& d = At(p, body);
    if (d.kind != ExprKind::Do || d.type != kVoidType || Shared(p, body)) {
        return 0;
    }
    bool skips = false;
    for (std::uint32_t i = 0; i < d.count; i++) {
        ExprId item = Item(p, d, i);
        if (SkipsRest(p, item)) {
            skips = true;
        } else if (ContinuesHere(p, item, 0)) {
            return 0;
        }
    }
    return skips ? Rewrite(p, body, loop) : 0;
}

} // namespace tisple::opt
