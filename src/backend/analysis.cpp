#include "backend/analysis.hpp"

#include "check/arith.hpp"

#include <algorithm>

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
               e.kind == ExprKind::StoreField || e.kind == ExprKind::Set || e.kind == ExprKind::Jsr;
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
        case ExprKind::Jsr:
        case ExprKind::WaitCycles:
        case ExprKind::Return:
        case ExprKind::Break:
        case ExprKind::Continue:
        case ExprKind::While:
        case ExprKind::For:
            return true;
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
                          e.kind == ExprKind::StoreField || e.kind == ExprKind::Jsr);
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

std::uint32_t CountExprs(const Program& program, ExprId id) {
    std::uint32_t count = 0;
    AnyExpr(program, id, [&](const Expr&) {
        count++;
        return false;
    });
    return count;
}

std::vector<InlineKind> FindInlines(const Program& program, const std::vector<std::string>& never) {
    std::size_t count = program.functions.size();
    std::vector<std::uint32_t> calls(count, 0);
    for (const Function& f : program.functions) {
        AnyExpr(program, f.body, [&](const Expr& e) {
            if (e.kind == ExprKind::Call) {
                calls[static_cast<std::size_t>(e.ref)]++;
            }
            return false;
        });
    }
    std::vector<InlineKind> out(count, InlineKind::None);
    for (std::size_t f = 0; f < count; f++) {
        const Function& fn = program.functions[f];
        if (std::find(never.begin(), never.end(), fn.name) != never.end()) {
            continue;
        }
        if (CountExprs(program, fn.body) <= kMaxInlineExprs) {
            out[f] = InlineKind::Always;
        } else if (calls[f] == 1) {
            out[f] = InlineKind::Once;
        }
    }
    return out;
}

std::vector<bool> FindDerefPointers(const Program& program,
                                    const std::vector<InlineKind>& inlines) {
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
    // a pointer handed to such a parameter of a function that is inlined: the inlined body
    // reads the argument where it is (gen_expr.cpp, CanBind), so it is the one used
    for (bool changed = true; changed;) {
        changed = false;
        for (const Expr& e : program.exprs) {
            if (e.kind != ExprKind::Call ||
                inlines[static_cast<std::size_t>(e.ref)] == InlineKind::None) {
                continue;
            }
            const Function& fn = program.functions[static_cast<std::size_t>(e.ref)];
            for (std::uint32_t i = 0; i < e.count && i < fn.params.size(); i++) {
                const Expr& arg = ExprOf(program, ListItem(program, e, i));
                std::size_t param = static_cast<std::size_t>(fn.params[i]);
                if (!deref[param] || arg.kind != ExprKind::VarRef) {
                    continue;
                }
                std::size_t var = static_cast<std::size_t>(arg.ref);
                if (!deref[var] && program.vars[var].kind != VarKind::ConstArray) {
                    deref[var] = true;
                    changed = true;
                }
            }
        }
    }
    return deref;
}

std::vector<bool> FindNamedTables(const Program& program) {
    std::size_t count = program.functions.size();
    std::vector<bool> reached(count, program.main < 0);
    std::vector<std::int32_t> work;
    if (program.main >= 0) {
        reached[static_cast<std::size_t>(program.main)] = true;
        work.push_back(program.main);
    }
    while (!work.empty()) {
        const Function& f = program.functions[static_cast<std::size_t>(work.back())];
        work.pop_back();
        for (std::int32_t callee : f.callees) {
            if (!reached[static_cast<std::size_t>(callee)]) {
                reached[static_cast<std::size_t>(callee)] = true;
                work.push_back(callee);
            }
        }
    }
    std::vector<bool> named(program.vars.size(), false);
    for (std::size_t f = 0; f < count; f++) {
        if (!reached[f]) {
            continue;
        }
        AnyExpr(program, program.functions[f].body, [&](const Expr& e) {
            if (e.kind == ExprKind::VarRef &&
                program.vars[static_cast<std::size_t>(e.ref)].kind == VarKind::ConstArray) {
                named[static_cast<std::size_t>(e.ref)] = true;
            }
            return false;
        });
    }
    return named;
}

} // namespace tisple
