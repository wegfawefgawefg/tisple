// Divisions the ranges pass (ranges.cpp) works out without dividing (slice 6): what the
// language defines a division by 0 and 1 to give, and what the ranges of the operands say.
// - (/ x 1) is x, (% x 1) is 0; (% x 0) is x, (/ x 0) every bit set for unsigned x (and for
//   signed x known not negative; 1 for one known negative), docs/language.md.
// - Unsigned, or both operands never negative: x below y makes (/ x y) 0 and (% x y) x.
// - Unsigned: (/ x (| x 1)) is (& x 1) (an odd x divides by itself, an even one by x + 1),
//   (% x (| x 1)) is x for an even x and 0 for an odd one, (& x (- (& x 1) 1)); (/ (% x y) y)
//   is 0 when y is never 0; (/ x x) is 1 when x is never 0, else (if (= x 0) all-ones 1).
//   Not for an x that reads a loop's counter: unrolling may make it a literal.
// A divisor or dividend that goes must be pure and quiet (no hardware or memory behind it);
// one that appears twice is small and steady, so both readings give the same value.
// Seeds 1241, 1407 and 1551 of the race; 150 of 2000 programs divide x by (| x 1).

#include "opt/collapse.hpp"
#include "opt/consts.hpp"
#include "opt/ranges.hpp"

#include "backend/analysis.hpp"
#include "check/arith.hpp"

namespace tisple::opt {

namespace {

// a copy of a quiet operand read twice is at most this many nodes
constexpr int kMaxTwice = 4;

ExprId NewNode(Pass& p, ExprKind kind, Op op, TypeId type, ExprId a, ExprId b, const Loc& loc) {
    Expr e;
    e.kind = kind;
    e.op = op;
    e.type = type;
    e.a = a;
    e.b = b;
    e.loc = loc;
    return NewExpr(p, e);
}

ExprId NewLit(Pass& p, TypeId type, std::uint32_t bits, const Loc& loc) {
    Expr e;
    e.kind = ExprKind::Literal;
    e.type = type;
    e.value = MaskToKind(KindOf(*p.program, type), bits);
    e.loc = loc;
    return NewExpr(p, e);
}

bool Goes(const Pass& p, ExprId id) {
    return IsPure(p, id) && Quiet(p, id);
}

int Size(const Pass& p, ExprId id, int limit) {
    int n = 1;
    EachChild(p, At(p, id), [&](ExprId child) {
        if (n <= limit) {
            n += Size(p, child, limit - n);
        }
    });
    return n;
}

// read twice, it gives the same value both times, and a copy costs little
bool Twice(const Pass& p, ExprId id) {
    return Goes(p, id) && Steady(p, id) && Size(p, id, kMaxTwice + 1) <= kMaxTwice;
}

// reads a loop's counter, which unrolling may make a literal that folds the division away (a
// rewrite would only make the body longer)
bool Counted(const Ranger& g, ExprId id) {
    const Expr& e = At(*g.p, id);
    if (e.kind == ExprKind::VarRef) {
        for (const Counter& c : g.counters) {
            if (!c.fact && c.var == e.ref) {
                return true;
            }
        }
        return false;
    }
    bool counted = false;
    EachChild(*g.p, e, [&](ExprId child) { counted = counted || Counted(g, child); });
    return counted;
}

// (| x 1), x the same as dividend
bool OrOne(const Pass& p, ExprId divisor, ExprId dividend) {
    const Expr& d = At(p, divisor);
    std::uint16_t one = 0;
    return d.kind == ExprKind::Binary && d.op == Op::Or && ConstValue(*p.program, d.b, one) &&
           one == 1 && SameExpr(*p.program, d.a, dividend);
}

// what id becomes when it is its dividend
constexpr ExprId kDividend = -2;

// what the division at id becomes, named by nobody (or kDividend); -1 when it stays
ExprId Rewrite(Ranger& g, ExprId id, const char*& what) {
    Pass& p = *g.p;
    Expr e = At(p, id);
    TypeKind kind = KindOf(*p.program, e.type);
    bool div = e.op == Op::Div;
    bool sign = IsSignedKind(kind);
    std::uint32_t ones = MaskToKind(kind, 0xFFFF);
    std::uint16_t k = 0;
    bool literal = ConstValue(*p.program, e.b, k);
    Range x = ValueRange(p, g.counters, e.a);
    Range y = ValueRange(p, g.counters, e.b);
    if (literal && k == 1) {
        what = div ? "division by 1" : "remainder by 1";
        if (div) {
            return kDividend;
        }
        return Goes(p, e.a) ? NewLit(p, e.type, 0, e.loc) : -1;
    }
    if (literal && k == 0) {
        what = div ? "division by 0" : "remainder by 0";
        if (!div) {
            return kDividend;
        }
        if (!Goes(p, e.a) || (sign && x.lo < 0 && x.hi >= 0)) {
            return -1;
        }
        return NewLit(p, e.type, sign && x.hi < 0 ? 1 : ones, e.loc);
    }
    bool natural = !sign || (x.lo >= 0 && y.lo >= 0);
    if (natural && x.hi < y.lo && Goes(p, e.b) && (!div || Goes(p, e.a))) {
        what = div ? "quotient below 1" : "remainder of a smaller dividend";
        return div ? NewLit(p, e.type, 0, e.loc) : kDividend;
    }
    if (sign) {
        return -1;
    }
    if (OrOne(p, e.b, e.a) && Twice(p, e.a) && !Counted(g, e.a)) {
        what = div ? "x by (| x 1)" : "x remainder (| x 1)";
        ExprId odd = NewNode(p, ExprKind::Binary, Op::And, e.type, CloneExpr(p, e.a),
                             NewLit(p, e.type, 1, e.loc), e.loc);
        if (div) {
            return odd;
        }
        ExprId mask = NewNode(p, ExprKind::Binary, Op::Sub, e.type, odd,
                              NewLit(p, e.type, 1, e.loc), e.loc);
        return NewNode(p, ExprKind::Binary, Op::And, e.type, CloneExpr(p, e.a), mask, e.loc);
    }
    const Expr& inner = At(p, e.a);
    if (div && inner.kind == ExprKind::Binary && inner.op == Op::Mod && y.lo >= 1 &&
        SameExpr(*p.program, inner.b, e.b) && Goes(p, e.a) && Twice(p, e.b)) {
        what = "quotient of a remainder";
        return NewLit(p, e.type, 0, e.loc);
    }
    if (div && SameExpr(*p.program, e.a, e.b) && Twice(p, e.a)) {
        what = "x by x";
        if (x.lo >= 1) {
            return NewLit(p, e.type, 1, e.loc);
        }
        if (Counted(g, e.a)) {
            return -1;
        }
        ExprId zero = NewNode(p, ExprKind::Binary, Op::Eq, kBoolType, CloneExpr(p, e.a),
                              NewLit(p, e.type, 0, e.loc), e.loc);
        Expr branch;
        branch.kind = ExprKind::If;
        branch.type = e.type;
        branch.a = zero;
        branch.b = NewLit(p, e.type, ones, e.loc);
        branch.c = NewLit(p, e.type, 1, e.loc);
        branch.loc = e.loc;
        return NewExpr(p, branch);
    }
    return -1;
}

} // namespace

void FoldDivision(Ranger& g, ExprId id) {
    Pass& p = *g.p;
    const Expr& e = At(p, id);
    TypeKind kind = KindOf(*p.program, e.type);
    if (e.kind != ExprKind::Binary || (e.op != Op::Div && e.op != Op::Mod) ||
        !IsIntegerKind(kind)) {
        return;
    }
    const char* what = "";
    ExprId made = Rewrite(g, id, what);
    if (made == -1) {
        return;
    }
    bool allowed = Allow(p, what, id, -1);
    if (made == kDividend) {
        if (allowed) {
            ReplaceWith(p, id, At(p, id).a);
            g.changes++;
        }
        return;
    }
    if (!allowed) {
        Retain(p, made);
        Release(p, made); // named by nobody: gives up its parts
        return;
    }
    Retain(p, made);
    ReplaceWith(p, id, made);
    Release(p, made);
    g.changes++;
}

} // namespace tisple::opt
