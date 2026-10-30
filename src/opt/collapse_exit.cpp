// Scans that run to their bound (slice 6): a while whose body only steps its counter by 1, and
// whose test is (< c k), or (and (< c k) x) with x the same on every pass (pure, quiet, not
// reading c), ends with c at k when it makes a pass, else where it was:
//   (let c u16 v) (while (and (< c 999) (!= 0 g)) (set c (+ c 1)))
//   -> (let c u16 v) (if (and (< c 999) (!= 0 g)) (set c 999))
// Seed 841's scan of wb, once the elements it reads are known (ranges_values.cpp's counter
// range, ranges_elements.cpp). The test is worked out once, as the loop's first pass does.
// And a while whose body ends in a break it reaches every time (nothing before it leaves)
// makes one pass: (if test (do items...)), as unswitching on (if x (break)) leaves one.

#include "opt/collapse.hpp"
#include "opt/licm.hpp"
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

bool LetsBefore(const Pass& p, ExprId list, std::uint32_t index) {
    const Expr& d = At(p, list);
    for (std::uint32_t i = 0; i < index; i++) {
        if (At(p, Item(p, d, i)).kind != ExprKind::Let) {
            return false;
        }
    }
    return true;
}

// a while's counter: c of (< c k) or of (and (< c k) x), or -1
std::int32_t WhileCounterVar(const Pass& p, ExprId loop) {
    const Expr& w = At(p, loop);
    if (w.kind != ExprKind::While) {
        return -1;
    }
    const Expr* test = &At(p, w.a);
    if (test->kind == ExprKind::And) {
        test = &At(p, test->a);
    }
    bool counts = test->kind == ExprKind::Binary && At(p, test->a).kind == ExprKind::VarRef;
    return counts ? At(p, test->a).ref : -1;
}

bool LetOfCounter(const Pass& p, ExprId before, ExprId loop) {
    std::int32_t c = WhileCounterVar(p, loop);
    return before >= 0 && c >= 0 && At(p, before).kind == ExprKind::Let && At(p, before).ref == c;
}

ExprId OuterCounterLet(const Pass& p, ExprId list, std::uint32_t index, ExprId loop) {
    std::int32_t c = WhileCounterVar(p, loop);
    const Expr& d = At(p, list);
    for (std::uint32_t i = index; c >= 0 && i > 0; i--) {
        ExprId item = Item(p, d, i - 1);
        if (At(p, item).kind != ExprKind::Let) {
            return -1;
        }
        if (At(p, item).ref == c) {
            return item;
        }
    }
    return -1;
}

bool RunsOnce(Pass& p, ExprId loop, std::vector<ExprId>& out) {
    const Expr& w = At(p, loop);
    const Expr& d = At(p, w.b);
    if (w.kind != ExprKind::While || d.kind != ExprKind::Do || d.count == 0 ||
        At(p, Item(p, d, d.count - 1)).kind != ExprKind::Break || !IsPure(p, w.a) ||
        Shared(p, w.a)) {
        return false;
    }
    for (std::uint32_t i = 0; i + 1 < d.count; i++) {
        if (MayLeave(p, Item(p, d, i), 0)) {
            return false; // a break or continue of its own, or a return
        }
    }
    if (!Allow(p, "while of one pass", loop, -1)) {
        return false;
    }
    std::vector<ExprId> items(p.program->lists.begin() + d.first,
                              p.program->lists.begin() + d.first + d.count - 1);
    std::uint16_t always = 0;
    if (ConstValue(*p.program, w.a, always) && always != 0) {
        out = items;
        return true;
    }
    Expr branch;
    branch.kind = ExprKind::If;
    branch.type = kVoidType;
    branch.a = w.a;
    branch.b = NewList(p, ExprKind::Do, kVoidType, items, w.loc);
    branch.loc = w.loc;
    out.push_back(NewExpr(p, branch));
    return true;
}

bool ScanToEnd(Pass& p, ExprId loop, ExprId before, std::vector<ExprId>& out) {
    const Expr& w = At(p, loop);
    if (before < 0 || At(p, before).kind != ExprKind::Let || Shared(p, w.a) ||
        PassOff(p, "scans")) {
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
