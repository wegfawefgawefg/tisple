// Unread parameters: a call's argument for a parameter its function never reads is not worked
// out; a literal 0 goes in its place, when the argument is pure and reads no hardware. The
// function may still set the parameter; nothing reads what the call passed. Variable reads
// too: an inlined call would read one in place (the backend binds it to the parameter) and
// stores the literal, a few cycles, but a loop around a call that reads what the loop writes
// does not collapse. Loops whose
// passes differ only in such an argument then collapse (collapse.cpp).

#include "opt/ranges.hpp"

namespace tisple::opt {

namespace {

// does id read var's value (a set's place names it without reading it)
bool ReadsVar(const Pass& p, ExprId id, std::int32_t var) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef) {
        return e.ref == var;
    }
    if (e.kind == ExprKind::Set && At(p, e.a).kind == ExprKind::VarRef) {
        return ReadsVar(p, e.b, var);
    }
    bool reads = false;
    EachChild(p, e, [&](ExprId child) { reads = reads || ReadsVar(p, child, var); });
    return reads;
}

// integers, bools and enums, whose 0 is a literal like any other
bool ConstKind(TypeKind kind) {
    return IsIntegerKind(kind) || kind == TypeKind::Bool || kind == TypeKind::Enum;
}

} // namespace

std::uint64_t DropUnreadArgs(Pass& p) {
    const Program& program = *p.program;
    // by function: which parameters its body reads
    std::vector<std::vector<bool>> read(program.functions.size());
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        const Function& fn = program.functions[f];
        for (std::int32_t param : fn.params) {
            const Var& v = VarOf(p, param);
            read[f].push_back(v.address_taken || ReadsVar(p, fn.body, param));
        }
    }
    std::uint64_t changes = 0;
    for (std::size_t id = 0; id < program.exprs.size(); id++) {
        const Expr& e = At(p, static_cast<ExprId>(id));
        if (e.kind != ExprKind::Call || p.refs[id] == 0) {
            continue;
        }
        const std::vector<bool>& params = read[static_cast<std::size_t>(e.ref)];
        for (std::uint32_t i = 0; i < e.count && i < params.size(); i++) {
            ExprId arg = Item(p, At(p, static_cast<ExprId>(id)), i);
            const Expr& a = At(p, arg);
            if (params[i] || a.kind == ExprKind::Literal || Shared(p, arg) || !Quiet(p, arg) ||
                !ConstKind(KindOf(*p.program, a.type)) || !Allow(p, "unread argument", arg, -1)) {
                continue;
            }
            ReplaceWithLiteral(p, arg, 0);
            changes++;
        }
    }
    return changes;
}

} // namespace tisple::opt
