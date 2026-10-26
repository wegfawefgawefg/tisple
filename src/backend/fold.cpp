// Constant parts of arithmetic folded before code generation. Unrolled copies and macros
// leave trees like (+ (+ 5 t) 2), (* 1 3) and (+ (+ p 0) 2); these become (+ t 7), 3 and
// (+ p 2). Every rewrite is exact in the type's wrapping arithmetic (check/arith.hpp, which
// the interpreter uses too): adds and subtracts, multiplies, and bitwise ops are each
// associative and commutative modulo 2^bits, and only constants, which have no effects,
// move. The other terms keep their order.
//
// Nodes are rewritten in place, so whoever names them sees the new tree. A node nothing
// names any more becomes a literal 0 with no children, so passes that scan every
// expression (reads of a var, parents of an expression) see only the live tree.

#include "backend/analysis.hpp"
#include "check/arith.hpp"

namespace tisple {

namespace {

struct Folder {
    Program* program = nullptr;
    std::vector<std::uint32_t> refs; // by expr: how many nodes (or function bodies) name it
    std::vector<bool> done;
};

// one operand of a flattened chain
struct Term {
    ExprId id = -1;
    bool minus = false;
};

Expr& At(Folder& f, ExprId id) {
    return f.program->exprs[static_cast<std::size_t>(id)];
}

template <typename Fn> void EachChild(const Program& program, const Expr& e, Fn fn) {
    for (ExprId child : {e.a, e.b, e.c}) {
        if (child >= 0) {
            fn(child);
        }
    }
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            fn(program.lists[e.first + i]);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            fn(program.clauses[e.first + i].body);
        }
    }
}

void Retain(Folder& f, ExprId id) {
    f.refs[static_cast<std::size_t>(id)]++;
}

// one name fewer; a node nobody names gives up its children and becomes a plain literal
void Release(Folder& f, ExprId id) {
    std::uint32_t& refs = f.refs[static_cast<std::size_t>(id)];
    if (refs == 0 || --refs > 0) {
        return;
    }
    Expr old = At(f, id);
    EachChild(*f.program, old, [&](ExprId child) { Release(f, child); });
    Expr dead;
    dead.kind = ExprKind::Literal;
    dead.type = old.type;
    dead.loc = old.loc;
    At(f, id) = dead;
}

ExprId NewExpr(Folder& f, const Expr& e) {
    ExprId id = static_cast<ExprId>(f.program->exprs.size());
    f.program->exprs.push_back(e);
    f.refs.push_back(0);
    f.done.push_back(true);
    EachChild(*f.program, e, [&](ExprId child) { Retain(f, child); });
    return id;
}

ExprId NewLiteral(Folder& f, TypeId type, std::uint16_t value, const Loc& loc) {
    Expr e;
    e.kind = ExprKind::Literal;
    e.type = type;
    e.value = value;
    e.loc = loc;
    return NewExpr(f, e);
}

ExprId NewBinary(Folder& f, ExprKind kind, Op op, TypeId type, ExprId a, ExprId b,
                 std::uint32_t value, const Loc& loc) {
    Expr e;
    e.kind = kind;
    e.op = op;
    e.type = type;
    e.a = a;
    e.b = b;
    e.value = value;
    e.loc = loc;
    return NewExpr(f, e);
}

// id's content becomes `with` (an expression built for it, or one it had as a part)
void Replace(Folder& f, ExprId id, const Expr& with) {
    Expr old = At(f, id);
    EachChild(*f.program, with, [&](ExprId child) { Retain(f, child); });
    At(f, id) = with;
    At(f, id).loc = old.loc;
    EachChild(*f.program, old, [&](ExprId child) { Release(f, child); });
}

void ReplaceWithLiteral(Folder& f, ExprId id, std::uint16_t value) {
    Expr lit;
    lit.kind = ExprKind::Literal;
    lit.type = At(f, id).type;
    lit.value = value;
    Replace(f, id, lit);
}

bool IsIntegerExpr(const Folder& f, ExprId id) {
    const Expr& e = f.program->exprs[static_cast<std::size_t>(id)];
    return IsIntegerKind(KindOf(*f.program, e.type));
}

std::uint16_t AllOnes(TypeKind kind) {
    return KindBits(kind) == 8 ? 0xFF : 0xFFFF;
}

// ---- integer chains: (op (op x c1) c2) and the like, for one op of one type

bool IsChainOp(Op op) {
    return op == Op::Add || op == Op::Sub || op == Op::Mul || op == Op::And || op == Op::Or ||
           op == Op::Xor;
}

// does node `id` continue a chain of `op` (Add takes Sub too) of this type
bool InChain(const Folder& f, ExprId id, Op op, TypeId type) {
    const Expr& e = f.program->exprs[static_cast<std::size_t>(id)];
    if (e.kind != ExprKind::Binary || e.type != type) {
        return false;
    }
    if (op == Op::Add) {
        return e.op == Op::Add || e.op == Op::Sub;
    }
    return e.op == op;
}

void Flatten(const Folder& f, ExprId id, Op op, TypeId type, bool minus, std::vector<Term>& out) {
    if (!InChain(f, id, op, type)) {
        out.push_back(Term{id, minus});
        return;
    }
    const Expr& e = f.program->exprs[static_cast<std::size_t>(id)];
    Flatten(f, e.a, op, type, minus, out);
    Flatten(f, e.b, op, type, e.op == Op::Sub ? !minus : minus, out);
}

// the chain's constants combined, and how many there were
std::uint16_t CombineConstants(const Folder& f, const std::vector<Term>& terms, Op op,
                               TypeKind kind, std::uint32_t& count, std::vector<Term>& rest) {
    std::uint16_t identity = op == Op::Mul ? 1 : op == Op::And ? AllOnes(kind) : 0;
    std::uint16_t c = identity;
    count = 0;
    for (const Term& t : terms) {
        std::uint16_t value = 0;
        if (!ConstValue(*f.program, t.id, value)) {
            rest.push_back(t);
            continue;
        }
        count++;
        bool unused = false;
        Op apply = op == Op::Add && t.minus ? Op::Sub : op;
        c = EvalBinaryOp(apply, kind, c, value, unused);
    }
    return c;
}

bool AnyEffects(const Folder& f, const std::vector<Term>& terms) {
    for (const Term& t : terms) {
        if (HasEffects(*f.program, t.id)) {
            return true;
        }
    }
    return false;
}

void FoldChain(Folder& f, ExprId id) {
    Expr root = At(f, id);
    Op op = root.op == Op::Sub ? Op::Add : root.op;
    TypeKind kind = KindOf(*f.program, root.type);
    std::vector<Term> terms;
    Flatten(f, id, op, root.type, false, terms);
    std::uint32_t count = 0;
    std::vector<Term> rest;
    std::uint16_t c = CombineConstants(f, terms, op, kind, count, rest);
    std::uint16_t identity = op == Op::Mul ? 1 : op == Op::And ? AllOnes(kind) : 0;
    bool absorbing = (op == Op::Mul && c == 0) || (op == Op::And && c == 0) ||
                     (op == Op::Or && c == AllOnes(kind));
    if (absorbing && count > 0 && !AnyEffects(f, rest)) {
        ReplaceWithLiteral(f, id, c);
        return;
    }
    // one constant that does something stays where the author put it
    if (count == 0 || rest.empty() || (count == 1 && c != identity)) {
        return;
    }
    // the other terms in their order, then the constant
    ExprId acc = -1;
    std::size_t first = 0;
    bool c_used = c == identity;
    if (rest[0].minus) {
        acc = NewLiteral(f, root.type, c, root.loc);
        c_used = true;
    } else {
        acc = rest[0].id;
        first = 1;
    }
    for (std::size_t i = first; i < rest.size(); i++) {
        Op step = op == Op::Add && rest[i].minus ? Op::Sub : op;
        acc = NewBinary(f, ExprKind::Binary, step, root.type, acc, rest[i].id, 0, root.loc);
    }
    if (!c_used) {
        // a net -1 reads better as (- x 1), and the backend knows that shape
        Op step = op;
        std::uint16_t value = c;
        std::uint16_t negated = MaskToKind(kind, 0x10000u - c);
        if (op == Op::Add && negated < c) {
            step = Op::Sub;
            value = negated;
        }
        ExprId lit = NewLiteral(f, root.type, value, root.loc);
        acc = NewBinary(f, ExprKind::Binary, step, root.type, acc, lit, 0, root.loc);
    }
    Retain(f, acc);
    Replace(f, id, At(f, acc));
    Release(f, acc);
}

// ---- pointer chains: (+ (+ p c1) c2) with one element size

bool InPtrChain(const Folder& f, ExprId id, TypeId type, std::uint32_t size) {
    const Expr& e = f.program->exprs[static_cast<std::size_t>(id)];
    bool ptr = e.kind == ExprKind::PtrAdd || e.kind == ExprKind::PtrSub;
    return ptr && e.type == type && e.value == size;
}

void FoldPtrChain(Folder& f, ExprId id) {
    Expr root = At(f, id);
    std::vector<Term> counts;
    ExprId base = id;
    while (InPtrChain(f, base, root.type, root.value)) {
        const Expr& e = At(f, base);
        counts.insert(counts.begin(), Term{e.b, e.kind == ExprKind::PtrSub});
        base = e.a;
    }
    std::uint32_t count = 0;
    std::vector<Term> rest;
    std::uint16_t c = CombineConstants(f, counts, Op::Add, TypeKind::U16, count, rest);
    if (count == 0 || (count == 1 && c != 0)) {
        return;
    }
    ExprId acc = base;
    for (const Term& t : rest) {
        ExprKind step = t.minus ? ExprKind::PtrSub : ExprKind::PtrAdd;
        acc = NewBinary(f, step, Op::Add, root.type, acc, t.id, root.value, root.loc);
    }
    if (c != 0) {
        ExprKind step = c > 0x8000 ? ExprKind::PtrSub : ExprKind::PtrAdd;
        std::uint16_t value = c > 0x8000 ? static_cast<std::uint16_t>(0x10000 - c) : c;
        ExprId lit = NewLiteral(f, kU16Type, value, root.loc);
        acc = NewBinary(f, step, Op::Add, root.type, acc, lit, root.value, root.loc);
    }
    Retain(f, acc);
    Replace(f, id, At(f, acc));
    Release(f, acc);
}

// ---- the walk

void FoldNode(Folder& f, ExprId id) {
    const Expr& e = At(f, id);
    bool arith =
        e.kind == ExprKind::Binary || e.kind == ExprKind::Unary || e.kind == ExprKind::Convert;
    std::uint16_t value = 0;
    if (arith && IsIntegerExpr(f, id) && ConstValue(*f.program, id, value)) {
        ReplaceWithLiteral(f, id, value);
        return;
    }
    if (e.kind == ExprKind::PtrAdd || e.kind == ExprKind::PtrSub) {
        FoldPtrChain(f, id);
        return;
    }
    if (e.kind != ExprKind::Binary || !IsIntegerExpr(f, id)) {
        return;
    }
    if (IsChainOp(e.op)) {
        FoldChain(f, id);
        return;
    }
    // a shift by 0
    bool shift = e.op == Op::Shl || e.op == Op::Shr;
    if (shift && ConstValue(*f.program, e.b, value) && value == 0) {
        ExprId a = e.a;
        Replace(f, id, At(f, a));
    }
}

void Walk(Folder& f, ExprId id) {
    if (id < 0 || f.done[static_cast<std::size_t>(id)]) {
        return;
    }
    f.done[static_cast<std::size_t>(id)] = true;
    Expr e = At(f, id);
    EachChild(*f.program, e, [&](ExprId child) { Walk(f, child); });
    FoldNode(f, id);
}

} // namespace

void FoldConstants(Program& program) {
    Folder f;
    f.program = &program;
    f.refs.assign(program.exprs.size(), 0);
    f.done.assign(program.exprs.size(), false);
    for (const Expr& e : program.exprs) {
        EachChild(program, e, [&](ExprId child) { Retain(f, child); });
    }
    for (const Function& fn : program.functions) {
        if (fn.body >= 0) {
            Retain(f, fn.body);
        }
    }
    for (const Function& fn : program.functions) {
        Walk(f, fn.body);
    }
}

} // namespace tisple
