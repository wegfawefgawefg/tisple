#include "backend/gen.hpp"

// Facts: what a condition tells about a u8 variable where it is known to hold. In
// (and (!= j 0) (> (at arr (- j 1)) v)) the second half only runs with j at least 1, so
// (at arr (- j 1)) is arr - 1 indexed by j; the same holds in the then arm of an if and in
// the body of a while loop. A fact lives until a statement that may change its variable
// starts (KillFacts), or until its scope ends (PopFacts).

namespace tisple {

namespace {

constexpr int kMaxDepth = 8;

const Expr& At(const Gen& g, ExprId id) {
    return ExprOf(*g.program, id);
}

bool IsComparison(Op op) {
    return op == Op::Eq || op == Op::Ne || op == Op::Lt || op == Op::Le || op == Op::Gt ||
           op == Op::Ge;
}

Op Negate(Op op) {
    switch (op) {
    case Op::Eq:
        return Op::Ne;
    case Op::Ne:
        return Op::Eq;
    case Op::Lt:
        return Op::Ge;
    case Op::Ge:
        return Op::Lt;
    case Op::Gt:
        return Op::Le;
    default:
        return Op::Gt;
    }
}

// c op v as v op' c
Op Mirror(Op op) {
    switch (op) {
    case Op::Lt:
        return Op::Gt;
    case Op::Gt:
        return Op::Lt;
    case Op::Le:
        return Op::Ge;
    case Op::Ge:
        return Op::Le;
    default:
        return op;
    }
}

// the u8 variable id reads, or -1
std::int32_t ByteVar(const Gen& g, ExprId id) {
    id = BoundTarget(g, id);
    const Expr& e = At(g, id);
    if (e.kind != ExprKind::VarRef || ExprKindOf(*g.program, id) != TypeKind::U8) {
        return -1;
    }
    return e.ref;
}

// v op c as a range of v; false if it says nothing a range can hold
bool RangeOf(Op op, int c, int& lo, int& hi) {
    lo = 0;
    hi = 255;
    switch (op) {
    case Op::Eq:
        lo = c;
        hi = c;
        return true;
    case Op::Ne:
        if (c == 0) {
            lo = 1;
            return true;
        }
        if (c == 255) {
            hi = 254;
            return true;
        }
        return false;
    case Op::Lt:
        if (c == 0) {
            return false;
        }
        hi = c - 1;
        return true;
    case Op::Le:
        hi = c;
        return true;
    case Op::Gt:
        if (c == 255) {
            return false;
        }
        lo = c + 1;
        return true;
    case Op::Ge:
        lo = c;
        return true;
    default:
        return false;
    }
}

void AddFact(Gen& g, std::int32_t var, int lo, int hi, ExprId scope) {
    if (scope >= 0 && MayWriteVarIn(g, scope, var)) {
        return;
    }
    Fact f;
    f.var = var;
    f.lo = lo;
    f.hi = hi;
    g.facts.push_back(f);
}

void Collect(Gen& g, ExprId id, bool when, ExprId scope, int depth) {
    if (id < 0 || depth > kMaxDepth) {
        return;
    }
    const Expr& e = At(g, id);
    switch (e.kind) {
    case ExprKind::Unary:
        if (e.op == Op::Not) {
            Collect(g, e.a, !when, scope, depth + 1);
        }
        return;
    case ExprKind::And:
        // both halves hold only when it is true
        if (when) {
            Collect(g, e.a, true, scope, depth + 1);
            Collect(g, e.b, true, scope, depth + 1);
        }
        return;
    case ExprKind::Or:
        if (!when) {
            Collect(g, e.a, false, scope, depth + 1);
            Collect(g, e.b, false, scope, depth + 1);
        }
        return;
    case ExprKind::Binary:
        break;
    default:
        return;
    }
    if (!IsComparison(e.op)) {
        return;
    }
    Op op = when ? e.op : Negate(e.op);
    std::uint16_t c = 0;
    std::int32_t var = ByteVar(g, e.a);
    if (var < 0 || !ConstValue(*g.program, e.b, c)) {
        var = ByteVar(g, e.b);
        if (var < 0 || !ConstValue(*g.program, e.a, c)) {
            return;
        }
        op = Mirror(op);
    }
    int lo = 0;
    int hi = 0;
    if (RangeOf(op, c & 0xFF, lo, hi)) {
        AddFact(g, var, lo, hi, scope);
    }
}

} // namespace

std::size_t PushFacts(Gen& g, ExprId cond, bool when, ExprId scope) {
    std::size_t mark = g.facts.size();
    Collect(g, cond, when, scope, 0);
    return mark;
}

void PopFacts(Gen& g, std::size_t mark) {
    g.facts.resize(mark);
}

void KillFacts(Gen& g, ExprId stmt) {
    for (Fact& f : g.facts) {
        if (f.live && MayWriteVarIn(g, stmt, f.var)) {
            f.live = false;
        }
    }
}

bool VarRange(const Gen& g, std::int32_t var, int& lo, int& hi) {
    lo = 0;
    hi = 255;
    bool known = false;
    for (const CounterRange& r : g.ranges) {
        if (r.var == var) {
            lo = r.lo > lo ? r.lo : lo;
            hi = r.hi < hi ? r.hi : hi;
            known = true;
        }
    }
    for (const Fact& f : g.facts) {
        if (f.live && f.var == var) {
            lo = f.lo > lo ? f.lo : lo;
            hi = f.hi < hi ? f.hi : hi;
            known = true;
        }
    }
    return known;
}

} // namespace tisple
