// Inlined copies that hand a parameter on. A small function such as
//
//   (fn reg ((i u8) (value u8)) void
//       (if (!= (at sent i) value)
//           (send i value)))
//
// inlined at a call keeps i and value in send's own parameter bytes: the call's arguments
// go there, the copy's reads find them there, and the call to send copies nothing. That
// holds while no call has run since the copy began, so the copy's only call is that one, at
// its end (where nothing reads the parameters after it), outside any loop, to a function
// that stays a JSR.

#include "backend/gen.hpp"

namespace tisple {

namespace {

std::uint32_t CountCalls(const Program& program, ExprId id) {
    if (id < 0) {
        return 0;
    }
    const Expr& e = ExprOf(program, id);
    std::uint32_t n = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ? 1 : 0;
    n += CountCalls(program, e.a) + CountCalls(program, e.b) + CountCalls(program, e.c);
    if (e.kind == ExprKind::Call || e.kind == ExprKind::StructLit || e.kind == ExprKind::Do) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            n += CountCalls(program, ListItem(program, e, i));
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            n += CountCalls(program, program.clauses[e.first + i].body);
        }
    }
    return n;
}

// the call that is the last thing id works out, on the path that reaches it: id itself, the
// last form of a do, either arm of an if; -1 when there is none
ExprId TailCall(const Program& program, ExprId id) {
    if (id < 0) {
        return -1;
    }
    const Expr& e = ExprOf(program, id);
    switch (e.kind) {
    case ExprKind::Call:
        return id;
    case ExprKind::Do:
        return e.count == 0 ? -1 : TailCall(program, ListItem(program, e, e.count - 1));
    case ExprKind::If: {
        ExprId then = TailCall(program, e.b);
        return then >= 0 ? then : TailCall(program, e.c);
    }
    default:
        return -1;
    }
}

bool PlainParam(const Gen& g, std::int32_t var) {
    const Var& v = g.program->vars[static_cast<std::size_t>(var)];
    return !v.address_taken && g.be->aliases[static_cast<std::size_t>(var)].value < 0;
}

} // namespace

std::int32_t ForwardedParam(const Gen& g, const Function& fn, std::uint32_t i) {
    const Program& program = *g.program;
    std::int32_t param = fn.params[i];
    if (!PlainParam(g, param) || CountCalls(program, fn.body) != 1) {
        return -1;
    }
    ExprId tail = TailCall(program, fn.body);
    if (tail < 0) {
        return -1;
    }
    const Expr& call = ExprOf(program, tail);
    std::int32_t callee = call.ref;
    if (g.be->inlines[static_cast<std::size_t>(callee)] != InlineKind::None ||
        callee == g.function) {
        return -1;
    }
    const Function& inner = program.functions[static_cast<std::size_t>(callee)];
    for (std::uint32_t j = 0; j < call.count && j < inner.params.size(); j++) {
        const Expr& arg = ExprOf(program, ListItem(program, call, j));
        if (arg.kind != ExprKind::VarRef || arg.ref != param) {
            continue;
        }
        std::int32_t slot = inner.params[j];
        std::uint32_t from =
            TypeOf(program, program.vars[static_cast<std::size_t>(param)].type).size;
        std::uint32_t to = TypeOf(program, program.vars[static_cast<std::size_t>(slot)].type).size;
        if (from != to || !PlainParam(g, slot)) {
            return -1;
        }
        return slot;
    }
    return -1;
}

} // namespace tisple
