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
void CheckLet(const Program& program, const Expr& block, std::uint32_t at,
              std::vector<IndexAlias>& out) {
    const Expr& let = ExprOf(program, ListItem(program, block, at));
    const Var& var = program.vars[static_cast<std::size_t>(let.ref)];
    if (var.kind != VarKind::Local || var.address_taken || let.a < 0) {
        return;
    }
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

} // namespace

std::vector<IndexAlias> FindIndexAliases(const Program& program) {
    std::vector<IndexAlias> out(program.vars.size());
    for (const Expr& e : program.exprs) {
        if (e.kind != ExprKind::Do) {
            continue;
        }
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (ExprOf(program, ListItem(program, e, i)).kind == ExprKind::Let) {
                CheckLet(program, e, i, out);
            }
        }
    }
    // reads of each var, and of those the ones that index a pointer or an array
    std::vector<std::uint32_t> reads(program.vars.size(), 0);
    std::vector<std::uint32_t> index_reads(program.vars.size(), 0);
    for (const Expr& e : program.exprs) {
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
    for (std::size_t v = 0; v < out.size(); v++) {
        out[v].index_only = out[v].value >= 0 && reads[v] == index_reads[v];
    }
    return out;
}

} // namespace tisple
