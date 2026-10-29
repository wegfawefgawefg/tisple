// Unread parameters: a call's argument for a parameter its function never reads is not worked
// out; a literal 0 goes in its place, when the argument is pure and reads no hardware. The
// function may still set the parameter; nothing reads what the call passed. Loops whose
// passes differ only in such an argument then collapse (collapse.cpp). Variable reads too: an
// inlined call would read one in place (the backend binds it to the parameter) and stores the
// literal, a few cycles, but a loop around a call that reads what the loop writes does not
// collapse.

#include "opt/ranges.hpp"

namespace tisple::opt {

namespace {

// the variables whose values code reads (a set's place names its variable without reading
// it), and the calls under id
void Walk(const Pass& p, ExprId id, std::vector<bool>& read, std::vector<ExprId>& calls) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef) {
        read[static_cast<std::size_t>(e.ref)] = true;
        return;
    }
    if (e.kind == ExprKind::Call) {
        calls.push_back(id);
    }
    if (e.kind == ExprKind::Set && At(p, e.a).kind == ExprKind::VarRef) {
        Walk(p, e.b, read, calls);
        return;
    }
    EachChild(p, e, [&](ExprId child) { Walk(p, child, read, calls); });
}

// integers, bools and enums, whose 0 is a literal like any other
bool ConstKind(TypeKind kind) {
    return IsIntegerKind(kind) || kind == TypeKind::Bool || kind == TypeKind::Enum;
}

} // namespace

std::uint64_t DropUnreadArgs(Pass& p) {
    const Program& program = *p.program;
    // one walk of every function: a parameter is read only in its own function
    std::vector<bool> read(program.vars.size(), false);
    std::vector<ExprId> calls;
    for (const Function& fn : program.functions) {
        Walk(p, fn.body, read, calls);
    }
    std::uint64_t changes = 0;
    for (ExprId call : calls) {
        const Function& fn = program.functions[static_cast<std::size_t>(At(p, call).ref)];
        for (std::uint32_t i = 0; i < At(p, call).count && i < fn.params.size(); i++) {
            std::int32_t param = fn.params[i];
            ExprId arg = Item(p, At(p, call), i);
            const Expr& a = At(p, arg);
            bool needed = read[static_cast<std::size_t>(param)] || VarOf(p, param).address_taken;
            if (needed || a.kind == ExprKind::Literal || Shared(p, arg) || !Quiet(p, arg) ||
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
