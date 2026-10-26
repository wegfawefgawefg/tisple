#include "backend/analysis.hpp"

#include "check/arith.hpp"

namespace tisple {

namespace {

// visits id and every sub-expression; stops early when fn returns true
template <typename Fn> bool AnyExpr(const Program& program, ExprId id, Fn fn) {
    if (id < 0) {
        return false;
    }
    const Expr& e = ExprOf(program, id);
    if (fn(e)) {
        return true;
    }
    if (AnyExpr(program, e.a, fn) || AnyExpr(program, e.b, fn) || AnyExpr(program, e.c, fn)) {
        return true;
    }
    switch (e.kind) {
    case ExprKind::Call:
    case ExprKind::StructLit:
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (AnyExpr(program, ListItem(program, e, i), fn)) {
                return true;
            }
        }
        return false;
    case ExprKind::Match:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (AnyExpr(program, program.clauses[e.first + i].body, fn)) {
                return true;
            }
        }
        return false;
    default:
        return false;
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

} // namespace

const Expr& ExprOf(const Program& program, ExprId id) {
    return program.exprs[static_cast<std::size_t>(id)];
}

ExprId ListItem(const Program& program, const Expr& e, std::uint32_t i) {
    return program.lists[e.first + i];
}

TypeKind ExprKindOf(const Program& program, ExprId id) {
    return KindOf(program, ExprOf(program, id).type);
}

std::uint32_t ExprSize(const Program& program, ExprId id) {
    return TypeOf(program, ExprOf(program, id).type).size;
}

bool HasWrites(const Program& program, ExprId id) {
    return AnyExpr(program, id, [](const Expr& e) {
        return e.kind == ExprKind::Call || e.kind == ExprKind::Store ||
               e.kind == ExprKind::StoreField || e.kind == ExprKind::Set;
    });
}

bool HasCalls(const Program& program, ExprId id) {
    return AnyExpr(program, id, [](const Expr& e) { return e.kind == ExprKind::Call; });
}

bool HasEffects(const Program& program, ExprId id) {
    return AnyExpr(program, id, [](const Expr& e) {
        switch (e.kind) {
        case ExprKind::Call:
        case ExprKind::Store:
        case ExprKind::StoreField:
        case ExprKind::Set:
        case ExprKind::Let:
        case ExprKind::IoRead:
        case ExprKind::IoWrite:
        case ExprKind::Return:
        case ExprKind::Break:
        case ExprKind::Continue:
        case ExprKind::While:
        case ExprKind::For:
            return true;
        case ExprKind::Binary:
            // a division by zero stops the interpreter, so keep it
            return e.op == Op::Div || e.op == Op::Mod;
        default:
            return false;
        }
    });
}

bool MayWriteVar(const Program& program, ExprId id, std::int32_t var) {
    const Var& v = program.vars[static_cast<std::size_t>(var)];
    bool shared = v.function < 0 || v.address_taken;
    return AnyExpr(program, id, [&](const Expr& e) {
        if (e.kind == ExprKind::Set && RootVar(program, e.a) == var) {
            return true;
        }
        if (e.kind == ExprKind::Let && e.ref == var) {
            return true;
        }
        return shared && (e.kind == ExprKind::Call || e.kind == ExprKind::Store ||
                          e.kind == ExprKind::StoreField);
    });
}

bool ReadsVar(const Program& program, ExprId id, std::int32_t var) {
    return AnyExpr(program, id,
                   [&](const Expr& e) { return e.kind == ExprKind::VarRef && e.ref == var; });
}

bool HasLoads(const Program& program, ExprId id) {
    return AnyExpr(program, id, [](const Expr& e) {
        return e.kind == ExprKind::Load || e.kind == ExprKind::LoadField;
    });
}

bool ConstValue(const Program& program, ExprId id, std::uint16_t& value) {
    const Expr& e = ExprOf(program, id);
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    switch (e.kind) {
    case ExprKind::Literal:
        value = static_cast<std::uint16_t>(e.value);
        return true;
    case ExprKind::Unary:
        if (!ConstValue(program, e.a, a)) {
            return false;
        }
        value = EvalUnaryOp(e.op, KindOf(program, e.type), a);
        return true;
    case ExprKind::Binary: {
        if (!ConstValue(program, e.a, a) || !ConstValue(program, e.b, b)) {
            return false;
        }
        bool div_by_zero = false;
        value = EvalBinaryOp(e.op, ExprKindOf(program, e.a), a, b, div_by_zero);
        return !div_by_zero;
    }
    case ExprKind::Convert:
        if (!ConstValue(program, e.a, a)) {
            return false;
        }
        value = ConvertBits(ExprKindOf(program, e.a), KindOf(program, e.type), a);
        return true;
    case ExprKind::Reinterpret:
        return ConstValue(program, e.a, value);
    case ExprKind::And:
    case ExprKind::Or: {
        if (!ConstValue(program, e.a, a)) {
            return false;
        }
        // the short circuit decides without b
        if ((e.kind == ExprKind::And && a == 0) || (e.kind == ExprKind::Or && a != 0)) {
            value = e.kind == ExprKind::Or ? 1 : 0;
            return true;
        }
        if (!ConstValue(program, e.b, b)) {
            return false;
        }
        value = b != 0 ? 1 : 0;
        return true;
    }
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub: {
        if (!ConstValue(program, e.a, a) || !ConstValue(program, e.b, b)) {
            return false;
        }
        std::uint32_t bytes = static_cast<std::uint32_t>(b) * e.value;
        std::uint32_t sum = e.kind == ExprKind::PtrAdd ? a + bytes : a - bytes;
        value = static_cast<std::uint16_t>(sum & 0xFFFF);
        return true;
    }
    default:
        return false;
    }
}

bool SameExpr(const Program& program, ExprId a, ExprId b) {
    if (a == b) {
        return true;
    }
    if (a < 0 || b < 0) {
        return false;
    }
    const Expr& x = ExprOf(program, a);
    const Expr& y = ExprOf(program, b);
    if (x.kind != y.kind || x.type != y.type || x.op != y.op || x.ref != y.ref ||
        x.value != y.value) {
        return false;
    }
    switch (x.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
        return true;
    case ExprKind::Index:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return SameExpr(program, x.a, y.a) && SameExpr(program, x.b, y.b);
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        return SameExpr(program, x.a, y.a);
    default:
        return false;
    }
}

std::vector<bool> FindDerefPointers(const Program& program) {
    std::vector<bool> deref(program.vars.size(), false);
    for (const Expr& e : program.exprs) {
        if (e.kind != ExprKind::Load && e.kind != ExprKind::LoadField &&
            e.kind != ExprKind::Store && e.kind != ExprKind::StoreField) {
            continue;
        }
        ExprId p = e.a;
        while (p >= 0) {
            const Expr& pe = ExprOf(program, p);
            if (pe.kind == ExprKind::PtrAdd || pe.kind == ExprKind::PtrSub) {
                p = pe.a;
                continue;
            }
            if (pe.kind == ExprKind::VarRef &&
                program.vars[static_cast<std::size_t>(pe.ref)].kind != VarKind::ConstArray) {
                deref[static_cast<std::size_t>(pe.ref)] = true;
            }
            break;
        }
    }
    return deref;
}

} // namespace tisple
