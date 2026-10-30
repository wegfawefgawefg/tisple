// Constant propagation and folding across statements (docs/midlevel-optimizer.md, pass 5).
// consts_walk.cpp finds what each expression holds on every run; here the tree is rewritten:
//
// - a pure expression that always holds one value becomes that literal (its variables, its
//   arithmetic, a const array read at a known index; a division by zero is folded to the
//   defined result, as the interpreter gives it)
// - an if or match whose condition is known and pure becomes the arm it picks
// - a while whose condition is false whenever it is tested becomes nothing
// - (and false x) and (or true x) become their value, (and true x) and (or false x) become x
//
// Only nodes one path of single parents leads to are rewritten (constant folding may leave
// shared nodes). The dead code pass then removes the lets and stores nobody reads any more.

#include "opt/consts.hpp"

#include "backend/analysis.hpp"
#include "check/arith.hpp"

namespace tisple::opt {

namespace {

struct Rewriter {
    Pass* p = nullptr;
    const ConstFacts* facts = nullptr;
    std::uint64_t changes = 0;
};

bool Known(const Rewriter& r, ExprId id, Val& v) {
    v = ValueOf(*r.facts, id);
    return v.kind == Val::Const && IsPure(*r.p, id);
}

void MakeEmptyDo(Pass& p, ExprId id) {
    Expr old = At(p, id);
    Expr empty;
    empty.kind = ExprKind::Do;
    empty.type = kVoidType;
    empty.loc = old.loc;
    p.program->exprs[static_cast<std::size_t>(id)] = empty;
    EachChild(p, old, [&](ExprId child) { Release(p, child); });
}

// may arm stand where branch was: the same type; or, for a statement, an exit or a scalar
// value as code may write one (a do with a value never is one), as dead.cpp leaves them
bool FitsFor(const Pass& p, ExprId branch, ExprId arm) {
    const Expr& b = At(p, branch);
    const Expr& a = At(p, arm);
    if (a.type == b.type) {
        return true;
    }
    if (b.type != kVoidType) {
        return false;
    }
    bool scalar = IsScalarKind(KindOf(*p.program, a.type)) && a.kind != ExprKind::Do;
    return a.type == kNeverType || scalar;
}

// the arm (or nothing) a branch on a known condition takes in place of the branch
void TakeArm(Rewriter& r, ExprId id, ExprId arm, const char* what) {
    Pass& p = *r.p;
    if (arm >= 0 && !FitsFor(p, id, arm)) {
        return;
    }
    if (arm < 0 && At(p, id).type != kVoidType) {
        return;
    }
    if (!Allow(p, what, id, -1)) {
        return;
    }
    if (arm >= 0) {
        ReplaceWith(p, id, arm);
    } else {
        MakeEmptyDo(p, id);
    }
    r.changes++;
}

// an if, match, while, and or or on a known condition: true when id was rewritten
bool Cut(Rewriter& r, ExprId id) {
    Pass& p = *r.p;
    Expr e = At(p, id);
    Val v;
    std::uint64_t before = r.changes;
    switch (e.kind) {
    case ExprKind::If:
        if (Known(r, e.a, v)) {
            TakeArm(r, id, v.bits != 0 ? e.b : e.c, "constant if");
        }
        break;
    case ExprKind::Match: {
        if (!Known(r, e.a, v)) {
            break;
        }
        TypeKind kind = KindOf(*p.program, At(p, e.a).type);
        ExprId arm = -1;
        for (std::uint32_t i = 0; i < e.count && arm < 0; i++) {
            const Clause& clause = p.program->clauses[e.first + i];
            if (clause.wildcard || MaskToKind(kind, clause.value) == v.bits) {
                arm = clause.body;
            }
        }
        TakeArm(r, id, arm, "constant match");
        break;
    }
    case ExprKind::While:
        if (Known(r, e.a, v) && v.bits == 0 && Allow(p, "while that never runs", id, -1)) {
            MakeEmptyDo(p, id);
            r.changes++;
        }
        break;
    case ExprKind::And:
    case ExprKind::Or: {
        if (!Known(r, e.a, v)) {
            break;
        }
        bool is_and = e.kind == ExprKind::And;
        bool decided = is_and ? v.bits == 0 : v.bits != 0;
        if (!Allow(p, is_and ? "constant and" : "constant or", id, -1)) {
            break;
        }
        if (decided) {
            ReplaceWithLiteral(p, id, is_and ? 0 : 1);
        } else {
            ReplaceWith(p, id, e.b);
        }
        r.changes++;
        break;
    }
    case ExprKind::Binary: {
        // (& x x) and (| x x) are x
        bool same =
            (e.op == Op::And || e.op == Op::Or) && SameExpr(*p.program, e.a, e.b) && Steady(p, e.a);
        if (same && Allow(p, "same operands", id, -1)) {
            ReplaceWith(p, id, e.a);
            r.changes++;
        }
        break;
    }
    default:
        break;
    }
    return r.changes != before;
}

bool Foldable(ExprKind kind) {
    switch (kind) {
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::Convert:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::If:
    case ExprKind::Match:
    case ExprKind::Do:
        return true;
    default:
        return false;
    }
}

// a pure value with one known value becomes that literal
bool Fold(Rewriter& r, ExprId id) {
    Pass& p = *r.p;
    const Expr& e = At(p, id);
    Val v;
    if (!Foldable(e.kind) || !ConstType(*p.program, e.type) || !Known(r, id, v)) {
        return false;
    }
    std::int32_t var = e.kind == ExprKind::VarRef ? e.ref : -1;
    if (!Allow(p, var >= 0 ? "constant variable" : "constant expression", id, var)) {
        return false;
    }
    ReplaceWithLiteral(p, id, v.bits);
    r.changes++;
    return true;
}

// (+ 65535 x), with the literal just put in for a variable, as the backend knows the shape:
// (- x 1). An add or subtract with one literal has it on the right, and the smaller of it and
// its negation (the arithmetic wraps, so both mean the same)
void AddendLast(Pass& p, ExprId id) {
    Expr e = At(p, id);
    if (e.kind != ExprKind::Binary || (e.op != Op::Add && e.op != Op::Sub)) {
        return;
    }
    TypeKind kind = KindOf(*p.program, e.type);
    bool left = At(p, e.a).kind == ExprKind::Literal;
    bool right = At(p, e.b).kind == ExprKind::Literal;
    if (!IsIntegerKind(kind) || left == right || (left && e.op == Op::Sub)) {
        return;
    }
    ExprId literal = left ? e.a : e.b;
    if (Shared(p, literal)) {
        return;
    }
    std::uint16_t c = static_cast<std::uint16_t>(At(p, literal).value);
    std::uint16_t negated = MaskToKind(kind, 0x10000u - c);
    Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
    if (left) {
        out.a = e.b;
        out.b = literal;
    }
    if (negated < c) {
        out.op = e.op == Op::Add ? Op::Sub : Op::Add;
        p.program->exprs[static_cast<std::size_t>(literal)].value = negated;
    }
}

void Rewrite(Rewriter& r, ExprId id, bool shared);

// the index expressions of a place; its root stays a variable
void RewritePlace(Rewriter& r, ExprId id, bool shared) {
    shared = shared || Shared(*r.p, id);
    Expr e = At(*r.p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return;
    case ExprKind::Index:
        RewritePlace(r, e.a, shared);
        Rewrite(r, e.b, shared);
        return;
    case ExprKind::Field:
        RewritePlace(r, e.a, shared);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        Rewrite(r, e.a, shared);
        return;
    default:
        Rewrite(r, id, shared);
        return;
    }
}

void Rewrite(Rewriter& r, ExprId id, bool shared) {
    if (id < 0) {
        return;
    }
    Pass& p = *r.p;
    shared = shared || Shared(p, id);
    // a cut puts an arm where the branch was: look at what is there now
    while (!shared && Cut(r, id)) {
    }
    if (!shared && Fold(r, id)) {
        return;
    }
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::Index:
        RewritePlace(r, e.a, shared);
        Rewrite(r, e.b, shared);
        return;
    case ExprKind::Field:
    case ExprKind::AddrOf:
        RewritePlace(r, e.a, shared);
        return;
    case ExprKind::Set:
        RewritePlace(r, e.a, shared);
        Rewrite(r, e.b, shared);
        return;
    case ExprKind::Binary: {
        std::uint64_t before = r.changes;
        Rewrite(r, e.a, shared);
        Rewrite(r, e.b, shared);
        if (!shared && r.changes != before) {
            AddendLast(p, id);
        }
        return;
    }
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Store:
    case ExprKind::StoreField:
    case ExprKind::Unary:
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
    case ExprKind::If:
    case ExprKind::Do:
    case ExprKind::While:
    case ExprKind::For:
    case ExprKind::Match:
    case ExprKind::Return:
    case ExprKind::Let:
        EachChild(p, e, [&](ExprId child) { Rewrite(r, child, shared); });
        return;
    default:
        return; // not known here: left as it is
    }
}

} // namespace

std::uint64_t PropagateConstants(Pass& p) {
    ConstFacts facts;
    facts.values.assign(p.program->exprs.size(), Val());
    Rewriter r;
    r.p = &p;
    r.facts = &facts;
    for (std::size_t f = 0; f < p.program->functions.size(); f++) {
        ExprId body = p.program->functions[f].body;
        if (body < 0) {
            continue;
        }
        FindConstants(p, static_cast<std::int32_t>(f), facts);
        Rewrite(r, body, false);
    }
    return r.changes;
}

} // namespace tisple::opt
