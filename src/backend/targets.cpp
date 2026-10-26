#include "backend/analysis.hpp"
#include "backend/ranges.hpp"

// What each pointer may point into, under the provenance rule (ranges.hpp): pointer values
// followed through lets, sets, arguments, returns, and memory (arrays, struct fields, stores
// through pointers) lumped together as the heap.

namespace tisple {

namespace {

// rounds over the program until the targets settle
constexpr int kMaxRounds = 48;

bool IsPointerType(const Program& program, TypeId type) {
    return KindOf(program, type) == TypeKind::Ptr;
}

Targets NoTargets(const Program& program) {
    Targets t;
    t.vars.assign(program.vars.size(), false);
    return t;
}

// to |= from; true if to grew
bool Join(Targets& to, const Targets& from) {
    bool grew = false;
    if (from.any && !to.any) {
        to.any = true;
        grew = true;
    }
    if (from.undeclared && !to.undeclared) {
        to.undeclared = true;
        grew = true;
    }
    for (std::size_t v = 0; v < from.vars.size(); v++) {
        if (from.vars[v] && !to.vars[v]) {
            to.vars[v] = true;
            grew = true;
        }
    }
    return grew;
}

// visits id and every sub-expression
template <typename Fn> void Walk(const Program& program, ExprId id, Fn& fn) {
    if (id < 0) {
        return;
    }
    const Expr& e = ExprOf(program, id);
    fn(id, e);
    Walk(program, e.a, fn);
    Walk(program, e.b, fn);
    Walk(program, e.c, fn);
    switch (e.kind) {
    case ExprKind::Call:
    case ExprKind::StructLit:
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            Walk(program, ListItem(program, e, i), fn);
        }
        break;
    case ExprKind::Match:
        for (std::uint32_t i = 0; i < e.count; i++) {
            Walk(program, program.clauses[e.first + i].body, fn);
        }
        break;
    default:
        break;
    }
}

} // namespace

Targets AnyTargets(const Program& program) {
    Targets t = NoTargets(program);
    t.any = true;
    t.undeclared = true;
    return t;
}

// what a pointer-valued expression may point into
Targets PointsTo(const Program& program, const ValueRanges& r, ExprId id) {
    const Expr& e = ExprOf(program, id);
    switch (e.kind) {
    case ExprKind::AddrOf: {
        ExprId place = e.a;
        while (place >= 0) {
            const Expr& p = ExprOf(program, place);
            if (p.kind == ExprKind::VarRef) {
                Targets t = NoTargets(program);
                t.vars[static_cast<std::size_t>(p.ref)] = true;
                return t;
            }
            if (p.kind == ExprKind::Load || p.kind == ExprKind::LoadField) {
                return PointsTo(program, r, p.a);
            }
            if (p.kind != ExprKind::Index && p.kind != ExprKind::Field) {
                break;
            }
            place = p.a;
        }
        return AnyTargets(program);
    }
    case ExprKind::VarRef: {
        const Var& v = program.vars[static_cast<std::size_t>(e.ref)];
        if (!IsPointerType(program, v.type)) {
            return AnyTargets(program);
        }
        Targets t = r.var_targets[static_cast<std::size_t>(e.ref)];
        if (v.address_taken) {
            Join(t, r.heap);
        }
        return t;
    }
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
        return r.heap;
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return PointsTo(program, r, e.a);
    case ExprKind::Reinterpret:
    case ExprKind::Literal: {
        // made from a number: memory no variable uses (provenance)
        Targets t = NoTargets(program);
        t.undeclared = true;
        return t;
    }
    case ExprKind::Call:
        return r.ret_targets[static_cast<std::size_t>(e.ref)];
    case ExprKind::If: {
        Targets t = PointsTo(program, r, e.b);
        if (e.c >= 0) {
            Join(t, PointsTo(program, r, e.c));
        }
        return t;
    }
    case ExprKind::Match: {
        Targets t = NoTargets(program);
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId body = program.clauses[e.first + i].body;
            if (IsPointerType(program, ExprOf(program, body).type)) {
                Join(t, PointsTo(program, r, body));
            }
        }
        return t;
    }
    case ExprKind::Do:
        if (e.count == 0) {
            return AnyTargets(program);
        }
        return PointsTo(program, r, ListItem(program, e, e.count - 1));
    default:
        return AnyTargets(program);
    }
}

namespace {

bool IsPointerExpr(const Program& program, ExprId id) {
    return id >= 0 && IsPointerType(program, ExprOf(program, id).type);
}

// one round over every function: pointer values into what may hold them
bool PointerRound(const Program& program, ValueRanges& r) {
    bool grew = false;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        const Function& fn = program.functions[f];
        auto visit = [&](ExprId, const Expr& e) {
            switch (e.kind) {
            case ExprKind::Let:
                if (e.a >= 0 && IsPointerExpr(program, e.a)) {
                    grew = Join(r.var_targets[static_cast<std::size_t>(e.ref)],
                                PointsTo(program, r, e.a)) ||
                           grew;
                }
                return;
            case ExprKind::Set: {
                if (!IsPointerExpr(program, e.b)) {
                    return;
                }
                const Expr& place = ExprOf(program, e.a);
                Targets value = PointsTo(program, r, e.b);
                if (place.kind == ExprKind::VarRef) {
                    std::size_t v = static_cast<std::size_t>(place.ref);
                    grew = Join(r.var_targets[v], value) || grew;
                    if (program.vars[v].address_taken) {
                        grew = Join(r.heap, value) || grew;
                    }
                    return;
                }
                grew = Join(r.heap, value) || grew;
                return;
            }
            case ExprKind::Store:
            case ExprKind::StoreField:
                if (IsPointerExpr(program, e.b)) {
                    grew = Join(r.heap, PointsTo(program, r, e.b)) || grew;
                }
                return;
            case ExprKind::StructLit:
                for (std::uint32_t i = 0; i < e.count; i++) {
                    ExprId field = ListItem(program, e, i);
                    if (IsPointerExpr(program, field)) {
                        grew = Join(r.heap, PointsTo(program, r, field)) || grew;
                    }
                }
                return;
            case ExprKind::Call: {
                const Function& callee = program.functions[static_cast<std::size_t>(e.ref)];
                for (std::uint32_t i = 0; i < e.count && i < callee.params.size(); i++) {
                    ExprId arg = ListItem(program, e, i);
                    if (IsPointerExpr(program, arg)) {
                        std::size_t p = static_cast<std::size_t>(callee.params[i]);
                        grew = Join(r.var_targets[p], PointsTo(program, r, arg)) || grew;
                    }
                }
                return;
            }
            case ExprKind::Return:
                if (e.a >= 0 && IsPointerExpr(program, e.a)) {
                    grew = Join(r.ret_targets[f], PointsTo(program, r, e.a)) || grew;
                }
                return;
            default:
                return;
            }
        };
        Walk(program, fn.body, visit);
        if (IsPointerType(program, fn.ret) && IsPointerExpr(program, fn.body)) {
            grew = Join(r.ret_targets[f], PointsTo(program, r, fn.body)) || grew;
        }
    }
    return grew;
}

} // namespace

void FindPointerTargets(const Program& program, ValueRanges& r) {
    std::size_t var_count = program.vars.size();
    r.var_targets.assign(var_count, NoTargets(program));
    r.ret_targets.assign(program.functions.size(), NoTargets(program));
    r.heap = NoTargets(program);
    // a global pointer's starting value is a number: memory no variable uses, or for 0 no
    // memory at all (going through it before setting it is undefined)
    for (std::size_t v = 0; v < var_count; v++) {
        const Var& var = program.vars[v];
        if (var.kind != VarKind::Global || !IsPointerType(program, var.type)) {
            continue;
        }
        for (std::uint8_t byte : var.init) {
            r.var_targets[v].undeclared = r.var_targets[v].undeclared || byte != 0;
        }
    }
    // a function nothing in the program calls is called from outside, with any pointers
    std::vector<bool> called(program.functions.size(), false);
    for (const Function& fn : program.functions) {
        for (std::int32_t callee : fn.callees) {
            called[static_cast<std::size_t>(callee)] = true;
        }
    }
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (called[f]) {
            continue;
        }
        for (std::int32_t param : program.functions[f].params) {
            r.var_targets[static_cast<std::size_t>(param)] = AnyTargets(program);
        }
    }
    for (int round = 0; PointerRound(program, r); round++) {
        if (round > kMaxRounds) {
            // not settled: every pointer may point anywhere
            r.heap = AnyTargets(program);
            for (Targets& t : r.var_targets) {
                t = AnyTargets(program);
            }
            for (Targets& t : r.ret_targets) {
                t = AnyTargets(program);
            }
            break;
        }
    }
}

} // namespace tisple
