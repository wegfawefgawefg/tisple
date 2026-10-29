// Unread parameters: a call's argument for a parameter its function never reads is not worked
// out; a literal 0 goes in its place, when the argument is pure and reads no hardware. The
// function may still set the parameter; nothing reads what the call passed. Loops whose
// passes differ only in such an argument then collapse (collapse.cpp). Variable reads too: an
// inlined call would read one in place (the backend binds it to the parameter) and stores the
// literal, a few cycles, but a loop around a call that reads what the loop writes does not
// collapse.
//
// Unused return values (slice 5): a function no call's value is used of (every call a
// statement) returns what it likes; its pure return values become a literal 0, so the work
// that made them goes (585's (* g6 59719) on every call). Only with a main (with none, any
// function may be called from outside, its value read), and only for functions something
// calls.

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

// marks the functions whose call values code uses; used: id's value is
void FindUsed(const Pass& p, ExprId id, bool used, std::vector<bool>& value_used) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Call:
        if (used) {
            value_used[static_cast<std::size_t>(e.ref)] = true;
        }
        for (std::uint32_t i = 0; i < e.count; i++) {
            FindUsed(p, Item(p, e, i), true, value_used);
        }
        return;
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            FindUsed(p, Item(p, e, i), used && i + 1 == e.count, value_used);
        }
        return;
    case ExprKind::If:
        FindUsed(p, e.a, true, value_used);
        FindUsed(p, e.b, used, value_used);
        FindUsed(p, e.c, used, value_used);
        return;
    case ExprKind::Match:
        FindUsed(p, e.a, true, value_used);
        for (std::uint32_t i = 0; i < e.count; i++) {
            FindUsed(p, p.program->clauses[e.first + i].body, used, value_used);
        }
        return;
    case ExprKind::While:
        FindUsed(p, e.a, true, value_used);
        FindUsed(p, e.b, false, value_used);
        return;
    case ExprKind::For:
        FindUsed(p, e.a, true, value_used);
        FindUsed(p, e.b, true, value_used);
        FindUsed(p, e.c, false, value_used);
        return;
    default:
        EachChild(p, e, [&](ExprId child) { FindUsed(p, child, true, value_used); });
        return;
    }
}

// a pure value worth nothing to its callers becomes 0
std::uint64_t Unused(Pass& p, ExprId value) {
    const Expr& e = At(p, value);
    if (e.kind == ExprKind::Literal || Shared(p, value) || !IsPure(p, value) ||
        !ConstKind(KindOf(*p.program, e.type)) || !Allow(p, "unused return value", value, -1)) {
        return 0;
    }
    ReplaceWithLiteral(p, value, 0);
    return 1;
}

// the returns under id, and the value of a do as its last item
std::uint64_t DropReturns(Pass& p, ExprId id) {
    if (id < 0) {
        return 0;
    }
    std::uint64_t changes = 0;
    Expr e = At(p, id);
    if (e.kind == ExprKind::Return && e.a >= 0) {
        changes += Unused(p, e.a);
    }
    EachChild(p, e, [&](ExprId child) { changes += DropReturns(p, child); });
    return changes;
}

} // namespace

std::uint64_t DropUnusedReturns(Pass& p) {
    const Program& program = *p.program;
    if (program.main < 0) {
        return 0;
    }
    std::vector<bool> value_used(program.functions.size(), false);
    std::vector<bool> called(program.functions.size(), false);
    for (const Function& fn : program.functions) {
        FindUsed(p, fn.body, fn.ret != kVoidType, value_used);
        for (std::int32_t callee : fn.callees) {
            called[static_cast<std::size_t>(callee)] = true;
        }
    }
    std::uint64_t changes = 0;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        const Function& fn = program.functions[f];
        // a function nothing calls is called from outside (main, or by address), which may
        // read its value
        if (value_used[f] || !called[f] || fn.ret == kVoidType || fn.body < 0) {
            continue;
        }
        changes += DropReturns(p, fn.body);
        const Expr& body = At(p, fn.body);
        if (body.kind == ExprKind::Do && body.count > 0) {
            changes += Unused(p, Item(p, body, body.count - 1));
        }
    }
    return changes;
}

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
