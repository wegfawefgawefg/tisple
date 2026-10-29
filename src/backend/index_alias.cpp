#include "backend/index_alias.hpp"

#include "backend/analysis.hpp"

namespace tisple {

namespace {

constexpr int kMaxFormDepth = 8;

bool IsByteInt(const Program& program, ExprId id) {
    TypeKind kind = ExprKindOf(program, id);
    return kind == TypeKind::U8 || kind == TypeKind::I8;
}

// a byte made of at most one variable and constants: x, k, (+ x k), (- x k), conversions
// between u8 and i8; `vars` gets the variables it reads
bool ByteForm(const Program& program, ExprId id, int depth, std::vector<std::int32_t>& vars) {
    if (depth > kMaxFormDepth || !IsByteInt(program, id)) {
        return false;
    }
    std::uint16_t c = 0;
    if (ConstValue(program, id, c)) {
        return true;
    }
    const Expr& e = ExprOf(program, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        if (program.vars[static_cast<std::size_t>(e.ref)].kind == VarKind::ConstArray) {
            return false;
        }
        vars.push_back(e.ref);
        return true;
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        return ByteForm(program, e.a, depth + 1, vars);
    case ExprKind::Binary:
        if (e.op == Op::Add && ConstValue(program, e.a, c)) {
            return ByteForm(program, e.b, depth + 1, vars);
        }
        if ((e.op == Op::Add || e.op == Op::Sub) && ConstValue(program, e.b, c)) {
            return ByteForm(program, e.a, depth + 1, vars);
        }
        return false;
    default:
        return false;
    }
}

// the let's value: a byte form, or one widened to u16
bool AliasForm(const Program& program, ExprId id, std::vector<std::int32_t>& vars) {
    const Expr& e = ExprOf(program, id);
    if (ExprKindOf(program, id) == TypeKind::U16) {
        return e.kind == ExprKind::Convert && ExprKindOf(program, e.a) == TypeKind::U8 &&
               ByteForm(program, e.a, 1, vars);
    }
    return ByteForm(program, id, 0, vars);
}

// a let in a do whose later forms keep it and what it reads unchanged
void CheckIndexLet(const Program& program, const Expr& block, std::uint32_t at,
                   std::vector<IndexAlias>& out) {
    const Expr& let = ExprOf(program, ListItem(program, block, at));
    std::vector<std::int32_t> vars;
    if (!AliasForm(program, let.a, vars)) {
        return;
    }
    vars.push_back(let.ref);
    for (std::uint32_t i = at + 1; i < block.count; i++) {
        ExprId later = ListItem(program, block, i);
        for (std::int32_t v : vars) {
            if (MayWriteVar(program, later, v)) {
                return;
            }
        }
    }
    out[static_cast<std::size_t>(let.ref)].value = let.a;
}

void CollectVars(const Program& program, ExprId id, std::vector<std::int32_t>& vars) {
    if (id < 0) {
        return;
    }
    const Expr& e = ExprOf(program, id);
    if (e.kind == ExprKind::VarRef) {
        vars.push_back(e.ref);
    }
    CollectVars(program, e.a, vars);
    CollectVars(program, e.b, vars);
    CollectVars(program, e.c, vars);
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            CollectVars(program, ListItem(program, e, i), vars);
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            CollectVars(program, program.clauses[e.first + i].body, vars);
        }
    }
}

// the parts of a form that run before it has any effect of its own, if they have none
bool EffectFreeParts(const Program& program, const Expr& form, ExprId parts[2]) {
    parts[0] = -1;
    parts[1] = -1;
    switch (form.kind) {
    case ExprKind::Let:
    case ExprKind::Return:
        parts[0] = form.a;
        break;
    case ExprKind::Set:
    case ExprKind::Store:
    case ExprKind::StoreField:
        parts[0] = form.a;
        parts[1] = form.b;
        break;
    case ExprKind::If:
        parts[0] = form.a; // the condition, before either arm
        break;
    default:
        return false;
    }
    for (int i = 0; i < 2; i++) {
        ExprId part = parts[i];
        if (part >= 0 && HasEffects(program, part)) {
            return false;
        }
    }
    return true;
}

// the let at `at` and the one before or after it work out (/ a b) and (% a b) of the same
// operands, which one division gives both of (src/opt/cse.cpp puts them side by side)
bool DivisionPair(const Program& program, const Expr& block, std::uint32_t at) {
    const Expr& let = ExprOf(program, ListItem(program, block, at));
    const Expr& x = ExprOf(program, let.a);
    if (x.kind != ExprKind::Binary || (x.op != Op::Div && x.op != Op::Mod)) {
        return false;
    }
    for (std::int64_t step : {-1, 1}) {
        std::int64_t other = static_cast<std::int64_t>(at) + step;
        if (other < 0 || other >= static_cast<std::int64_t>(block.count)) {
            continue;
        }
        const Expr& next =
            ExprOf(program, ListItem(program, block, static_cast<std::uint32_t>(other)));
        if (next.kind != ExprKind::Let) {
            continue;
        }
        const Expr& y = ExprOf(program, next.a);
        bool partner = y.kind == ExprKind::Binary && y.op != x.op &&
                       (y.op == Op::Div || y.op == Op::Mod) && y.type == x.type;
        if (partner && SameExpr(program, x.a, y.a) && SameExpr(program, x.b, y.b)) {
            return true;
        }
    }
    return false;
}

// a let read once, by a later form of its do before that form does anything, with nothing
// in between changing what its value reads: the read can be the value itself. Not half of a
// division pair, which would then divide twice
void CheckForwardLet(const Program& program, const Expr& block, std::uint32_t at,
                     std::vector<IndexAlias>& out, std::vector<std::int32_t>& reader_let) {
    const Expr& let = ExprOf(program, ListItem(program, block, at));
    TypeKind kind = KindOf(program, program.vars[static_cast<std::size_t>(let.ref)].type);
    if (!IsScalarKind(kind) || kind == TypeKind::Ptr || HasEffects(program, let.a)) {
        return;
    }
    if (DivisionPair(program, block, at)) {
        return;
    }
    std::vector<std::int32_t> vars;
    CollectVars(program, let.a, vars);
    bool loads = HasLoads(program, let.a);
    for (std::uint32_t i = at + 1; i < block.count; i++) {
        const Expr& form = ExprOf(program, ListItem(program, block, i));
        ExprId parts[2];
        bool here = ReadsVar(program, ListItem(program, block, i), let.ref);
        if (here) {
            bool in_parts = EffectFreeParts(program, form, parts) &&
                            (ReadsVar(program, parts[0], let.ref) ||
                             (parts[1] >= 0 && ReadsVar(program, parts[1], let.ref)));
            if (in_parts) {
                out[static_cast<std::size_t>(let.ref)].value = let.a;
                out[static_cast<std::size_t>(let.ref)].forward = true;
                reader_let[static_cast<std::size_t>(let.ref)] =
                    form.kind == ExprKind::Let ? form.ref : -1;
            }
            return;
        }
        ExprId between = ListItem(program, block, i);
        if (loads && HasWrites(program, between)) {
            return;
        }
        for (std::int32_t v : vars) {
            if (MayWriteVar(program, between, v)) {
                return;
            }
        }
    }
}

} // namespace

std::vector<IndexAlias> FindIndexAliases(const Program& program) {
    std::vector<IndexAlias> out(program.vars.size());
    // reads of each var, and of those the ones that index a pointer or an array; a var read
    // through an expression with two parents is left alone
    std::vector<std::uint32_t> reads(program.vars.size(), 0);
    std::vector<std::uint32_t> index_reads(program.vars.size(), 0);
    std::vector<std::uint32_t> parents(program.exprs.size(), 0);
    for (const Expr& e : program.exprs) {
        for (ExprId child : {e.a, e.b, e.c}) {
            if (child >= 0) {
                parents[static_cast<std::size_t>(child)]++;
            }
        }
        if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
            for (std::uint32_t i = 0; i < e.count; i++) {
                parents[static_cast<std::size_t>(ListItem(program, e, i))]++;
            }
        }
        if (e.kind == ExprKind::Match) {
            for (std::uint32_t i = 0; i < e.count; i++) {
                parents[static_cast<std::size_t>(program.clauses[e.first + i].body)]++;
            }
        }
        if (e.kind == ExprKind::VarRef) {
            reads[static_cast<std::size_t>(e.ref)]++;
        }
        ExprId index = -1;
        if (e.kind == ExprKind::Index) {
            index = e.b;
        } else if (e.kind == ExprKind::Load || e.kind == ExprKind::Store) {
            const Expr& pointer = ExprOf(program, e.a);
            index = pointer.kind == ExprKind::PtrAdd ? pointer.b : -1;
        }
        if (index >= 0 && ExprOf(program, index).kind == ExprKind::VarRef) {
            index_reads[static_cast<std::size_t>(ExprOf(program, index).ref)]++;
        }
    }
    std::vector<bool> shared(program.vars.size(), false);
    for (std::size_t i = 0; i < program.exprs.size(); i++) {
        const Expr& e = program.exprs[i];
        if (e.kind == ExprKind::VarRef && parents[i] > 1) {
            shared[static_cast<std::size_t>(e.ref)] = true;
        }
    }
    // the let whose value reads each forwarded let
    std::vector<std::int32_t> reader_let(program.vars.size(), -1);
    for (const Expr& e : program.exprs) {
        if (e.kind != ExprKind::Do) {
            continue;
        }
        for (std::uint32_t i = 0; i < e.count; i++) {
            const Expr& let = ExprOf(program, ListItem(program, e, i));
            if (let.kind != ExprKind::Let || let.a < 0) {
                continue;
            }
            std::size_t v = static_cast<std::size_t>(let.ref);
            const Var& var = program.vars[v];
            if (var.kind != VarKind::Local || var.address_taken || shared[v]) {
                continue;
            }
            CheckIndexLet(program, e, i, out);
            if (out[v].value < 0 && reads[v] == 1) {
                CheckForwardLet(program, e, i, out, reader_let);
            }
        }
    }
    // a forwarded let is read where its reader is. When that reader is itself a let read
    // later as its value (an index alias or another forwarded let), the read moves later
    // still, past forms the check never looked at: such a let runs after all.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t v = 0; v < out.size(); v++) {
            std::int32_t reader = reader_let[v];
            if (!out[v].forward || reader < 0 || out[static_cast<std::size_t>(reader)].value < 0) {
                continue;
            }
            out[v] = IndexAlias();
            changed = true;
        }
    }

    for (std::size_t v = 0; v < out.size(); v++) {
        out[v].index_only = out[v].value >= 0 && reads[v] == index_reads[v];
    }
    return out;
}

} // namespace tisple
