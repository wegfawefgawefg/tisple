#include "backend/ranges.hpp"

#include "backend/analysis.hpp"

#include <algorithm>

namespace tisple {

namespace {

// rounds until the ranges settle; after that every range still growing is taken as anything
constexpr int kMaxRounds = 12;

bool IsPointerType(const Program& program, TypeId type) {
    return KindOf(program, type) == TypeKind::Ptr;
}

// a byte type or an array of them, at any depth
bool IsByteData(const Program& program, TypeId type) {
    const Type& t = TypeOf(program, type);
    if (t.kind == TypeKind::Array) {
        return IsByteData(program, t.elem);
    }
    return IsScalarKind(t.kind) && t.size == 1;
}

Targets NoTargets(const Program& program) {
    Targets t;
    t.vars.assign(program.vars.size(), false);
    return t;
}

Targets AnyTargets(const Program& program) {
    Targets t = NoTargets(program);
    t.any = true;
    t.undeclared = true;
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

std::int32_t RootVar(const Program& program, ExprId id) {
    while (id >= 0) {
        const Expr& e = ExprOf(program, id);
        if (e.kind == ExprKind::VarRef) {
            return e.ref;
        }
        if (e.kind != ExprKind::Index && e.kind != ExprKind::Field) {
            return -1;
        }
        id = e.a;
    }
    return -1;
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

std::uint32_t Limit(const Program& program, ExprId id) {
    return ExprSize(program, id) == 1 ? 0xFF : 0xFFFF;
}

// the smallest all-ones value at least v
std::uint32_t Fill(std::uint32_t v) {
    std::uint32_t f = 0;
    while (f < v) {
        f = (f << 1) | 1;
    }
    return f;
}

std::uint32_t TargetsMax(const ValueRanges& r, const Targets& t) {
    if (t.any || t.undeclared) {
        return 0xFF;
    }
    std::uint32_t m = 0;
    for (std::size_t v = 0; v < t.vars.size(); v++) {
        if (t.vars[v]) {
            m = std::max<std::uint32_t>(m, r.var_max[v]);
        }
    }
    return m;
}

std::uint32_t BinaryMax(const Program& program, const ValueRanges& r, const Expr& e,
                        std::uint32_t limit) {
    std::uint32_t a = ExprMax(program, r, e.a);
    std::uint32_t b = ExprMax(program, r, e.b);
    bool is_signed = IsSignedKind(ExprKindOf(program, e.a));
    std::uint16_t c = 0;
    bool fixed = ConstValue(program, e.b, c);
    switch (e.op) {
    case Op::Add:
        return a + b <= limit ? a + b : limit;
    case Op::Mul:
        return a * b <= limit ? a * b : limit;
    case Op::And:
        return std::min(a, b);
    case Op::Or:
    case Op::Xor:
        return std::min(Fill(std::max(a, b)), limit);
    case Op::Shr:
        if (fixed && !is_signed) {
            return c >= 16 ? 0 : a >> c;
        }
        return is_signed ? limit : a;
    case Op::Shl:
        if (fixed && c < 16 && (a << c) <= limit) {
            return a << c;
        }
        return limit;
    case Op::Div:
        // a divisor of 0 gives all ones (check/arith.hpp)
        if (fixed && c != 0 && !is_signed) {
            return a / c;
        }
        return limit;
    case Op::Mod:
        // a divisor of 0 gives the dividend
        if (fixed && c != 0 && !is_signed) {
            return std::min<std::uint32_t>(a, c - 1u);
        }
        return is_signed ? limit : a;
    case Op::Eq:
    case Op::Ne:
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
        return 1;
    default:
        return limit;
    }
}

std::uint32_t ConvertMax(const Program& program, const ValueRanges& r, const Expr& e,
                         std::uint32_t limit) {
    std::uint32_t from = ExprSize(program, e.a);
    std::uint32_t a = ExprMax(program, r, e.a);
    if (from == 2 && limit == 0xFF) {
        return a <= 0xFF ? a : 0xFF;
    }
    if (from == 1 && limit == 0xFFFF && IsSignedKind(ExprKindOf(program, e.a))) {
        return a <= 0x7F ? a : 0xFFFF;
    }
    return std::min(a, limit);
}

// one round over every write to a ranged variable; true if a range grew
bool RangeRound(const Program& program, ValueRanges& r) {
    bool grew = false;
    auto raise = [&](std::size_t v, std::uint32_t m) {
        if (!r.candidate[v]) {
            return;
        }
        std::uint8_t byte = static_cast<std::uint8_t>(std::min<std::uint32_t>(m, 0xFF));
        if (byte > r.var_max[v]) {
            r.var_max[v] = byte;
            grew = true;
        }
    };
    auto raise_targets = [&](const Targets& t, std::uint32_t m) {
        for (std::size_t v = 0; v < program.vars.size(); v++) {
            if (t.vars[v] || (t.any && program.vars[v].address_taken)) {
                raise(v, m);
            }
        }
    };
    for (const Function& fn : program.functions) {
        auto visit = [&](ExprId, const Expr& e) {
            switch (e.kind) {
            case ExprKind::Let:
                if (e.a >= 0) {
                    raise(static_cast<std::size_t>(e.ref), ExprMax(program, r, e.a));
                }
                return;
            case ExprKind::Set: {
                std::int32_t root = RootVar(program, e.a);
                if (root < 0) {
                    return;
                }
                std::uint32_t m = ExprSize(program, e.a) == 1 ? ExprMax(program, r, e.b) : 0xFF;
                raise(static_cast<std::size_t>(root), m);
                return;
            }
            case ExprKind::Store:
            case ExprKind::StoreField: {
                std::uint32_t m = ExprSize(program, e.b) == 1 ? ExprMax(program, r, e.b) : 0xFF;
                raise_targets(PointsTo(program, r, e.a), m);
                return;
            }
            case ExprKind::IoWrite: {
                std::uint16_t address = 0;
                if (!ConstValue(program, e.a, address)) {
                    raise_targets(AnyTargets(program), 0xFF);
                }
                return;
            }
            default:
                return;
            }
        };
        Walk(program, fn.body, visit);
    }
    return grew;
}

} // namespace

ValueRanges FindValueRanges(const Program& program) {
    ValueRanges r;
    std::size_t var_count = program.vars.size();
    r.var_max.assign(var_count, 0xFF);
    r.candidate.assign(var_count, false);
    r.var_targets.assign(var_count, NoTargets(program));
    r.ret_targets.assign(program.functions.size(), NoTargets(program));
    r.heap = NoTargets(program);
    for (std::size_t v = 0; v < var_count; v++) {
        const Var& var = program.vars[v];
        bool data = var.kind == VarKind::Global || var.kind == VarKind::ConstArray ||
                    (var.kind == VarKind::Local && !var.loop_var);
        if (!data || var.placement.has_address || !IsByteData(program, var.type)) {
            continue;
        }
        r.candidate[v] = true;
        std::uint8_t m = 0;
        for (std::uint8_t byte : var.init) {
            m = std::max(m, byte);
        }
        r.var_max[v] = m;
    }
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
        if (round > 4 * kMaxRounds) {
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
    for (int round = 0;; round++) {
        std::vector<std::uint8_t> before = r.var_max;
        if (!RangeRound(program, r)) {
            break;
        }
        if (round >= kMaxRounds) {
            // what still grows may grow to anything
            for (std::size_t v = 0; v < var_count; v++) {
                if (r.var_max[v] != before[v]) {
                    r.var_max[v] = 0xFF;
                }
            }
        }
    }
    return r;
}

std::uint32_t ExprMax(const Program& program, const ValueRanges& r, ExprId id) {
    std::uint32_t limit = Limit(program, id);
    const Expr& e = ExprOf(program, id);
    switch (e.kind) {
    case ExprKind::Literal:
        return e.value & limit;
    case ExprKind::VarRef:
    case ExprKind::Index: {
        std::int32_t root = RootVar(program, id);
        if (root < 0 || limit != 0xFF || !r.candidate[static_cast<std::size_t>(root)]) {
            return limit;
        }
        return r.var_max[static_cast<std::size_t>(root)];
    }
    case ExprKind::Load:
        if (limit != 0xFF) {
            return limit;
        }
        return TargetsMax(r, PointsTo(program, r, e.a));
    case ExprKind::Binary:
        return BinaryMax(program, r, e, limit);
    case ExprKind::Unary:
        return e.op == Op::Not ? 1 : limit;
    case ExprKind::And:
    case ExprKind::Or:
        return 1;
    case ExprKind::Convert:
        return ConvertMax(program, r, e, limit);
    case ExprKind::If: {
        if (e.c < 0) {
            return limit;
        }
        std::uint32_t m = 0;
        for (ExprId arm : {e.b, e.c}) {
            if (ExprOf(program, arm).type != kNeverType) {
                m = std::max(m, std::min(ExprMax(program, r, arm), limit));
            }
        }
        return m;
    }
    case ExprKind::Match: {
        std::uint32_t m = 0;
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId body = program.clauses[e.first + i].body;
            if (ExprOf(program, body).type == kNeverType) {
                continue;
            }
            if (ExprSize(program, body) != ExprSize(program, id)) {
                return limit;
            }
            m = std::max(m, std::min(ExprMax(program, r, body), limit));
        }
        return m;
    }
    case ExprKind::Do:
        if (e.count == 0) {
            return limit;
        }
        return ExprMax(program, r, ListItem(program, e, e.count - 1));
    default:
        return limit;
    }
}

} // namespace tisple
