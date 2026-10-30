// Scans that run to their bound (slice 6): a while whose body only steps its counter by 1, and
// whose test is (< c k), or (and (< c k) x) with x the same on every pass (pure, quiet, not
// reading c), ends with c at k when it makes a pass, else where it was:
//   (let c u16 v) (while (and (< c 999) (!= 0 g)) (set c (+ c 1)))
//   -> (let c u16 v) (if (and (< c 999) (!= 0 g)) (set c 999))
// Seed 841's scan of wb, once the elements it reads are known (ranges_values.cpp's counter
// range, ranges_elements.cpp). The test is worked out once, as the loop's first pass does.

#include "opt/collapse.hpp"
#include "opt/ranges.hpp"

namespace tisple::opt {

namespace {

bool ReadsVar(const Pass& p, ExprId id, std::int32_t var) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef) {
        return e.ref == var;
    }
    bool reads = false;
    EachChild(p, e, [&](ExprId child) { reads = reads || ReadsVar(p, child, var); });
    return reads;
}

// the body's one statement (set c (+ c 1)), or -1
ExprId OnlyStep(const Pass& p, ExprId body, std::int32_t c) {
    ExprId stmt = body;
    const Expr& d = At(p, body);
    if (d.kind == ExprKind::Do) {
        if (d.count != 1) {
            return -1;
        }
        stmt = Item(p, d, 0);
    }
    const Expr& set = At(p, stmt);
    if (set.kind != ExprKind::Set || At(p, set.a).kind != ExprKind::VarRef ||
        At(p, set.a).ref != c) {
        return -1;
    }
    const Expr& add = At(p, set.b);
    std::uint16_t s = 0;
    bool step = add.kind == ExprKind::Binary && add.op == Op::Add &&
                At(p, add.a).kind == ExprKind::VarRef && At(p, add.a).ref == c &&
                ConstValue(*p.program, add.b, s) && s == 1;
    return step ? stmt : -1;
}

} // namespace

bool ScanToEnd(Pass& p, ExprId loop, ExprId before, std::vector<ExprId>& out) {
    const Expr& w = At(p, loop);
    if (before < 0 || At(p, before).kind != ExprKind::Let || Shared(p, w.a)) {
        return false;
    }
    std::int32_t c = At(p, before).ref;
    const Expr* test = &At(p, w.a);
    ExprId extra = -1;
    if (test->kind == ExprKind::And) {
        extra = test->b;
        test = &At(p, test->a);
    }
    std::uint16_t k = 0;
    if (test->kind != ExprKind::Binary || test->op != Op::Lt ||
        At(p, test->a).kind != ExprKind::VarRef || At(p, test->a).ref != c ||
        !ConstValue(*p.program, test->b, k) || VarOf(p, c).address_taken ||
        !IsIntegerKind(KindOf(*p.program, VarOf(p, c).type))) {
        return false;
    }
    if (extra >= 0 && (!IsPure(p, extra) || !Quiet(p, extra) || ReadsVar(p, extra, c))) {
        return false;
    }
    if (OnlyStep(p, w.b, c) < 0 || !Allow(p, "scan run to its bound", loop, c)) {
        return false;
    }
    Loc loc = w.loc;
    Expr read;
    read.kind = ExprKind::VarRef;
    read.type = VarOf(p, c).type;
    read.ref = c;
    read.loc = loc;
    Expr end;
    end.kind = ExprKind::Literal;
    end.type = read.type;
    end.value = k;
    end.loc = loc;
    Expr set;
    set.kind = ExprKind::Set;
    set.type = kVoidType;
    set.a = NewExpr(p, read);
    set.b = NewExpr(p, end);
    set.loc = loc;
    Expr branch;
    branch.kind = ExprKind::If;
    branch.type = kVoidType;
    branch.a = At(p, loop).a; // the loop gives it up once replaced
    branch.b = NewExpr(p, set);
    branch.loc = loc;
    out.push_back(NewExpr(p, branch));
    return true;
}

} // namespace tisple::opt
