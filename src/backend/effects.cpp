#include "backend/effects.hpp"

#include "backend/analysis.hpp"
#include "backend/frames.hpp"

namespace tisple {

namespace {

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

// visits every expression under id
template <typename Fn> void Walk(const Program& program, ExprId id, Fn& fn) {
    if (id < 0) {
        return;
    }
    const Expr& e = ExprOf(program, id);
    fn(e);
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

Effects ComputeEffects(const Program& program) {
    std::size_t fn_count = program.functions.size();
    Effects fx;
    fx.writes.assign(fn_count, std::vector<bool>(program.vars.size(), false));
    fx.stores.assign(fn_count, false);
    for (std::size_t f = 0; f < fn_count; f++) {
        std::vector<bool>& writes = fx.writes[f];
        bool stores = false;
        bool jsr = false;
        auto visit = [&](const Expr& e) {
            if (e.kind == ExprKind::Set) {
                std::int32_t root = RootVar(program, e.a);
                if (root >= 0) {
                    writes[static_cast<std::size_t>(root)] = true;
                }
            } else if (e.kind == ExprKind::Let || e.kind == ExprKind::For) {
                writes[static_cast<std::size_t>(e.ref)] = true;
            } else if (e.kind == ExprKind::Store || e.kind == ExprKind::StoreField) {
                stores = true;
            } else if (e.kind == ExprKind::Jsr) {
                jsr = true;
            }
        };
        Walk(program, program.functions[f].body, visit);
        // machine code may change any global, and anything a pointer reaches
        for (std::size_t v = 0; v < program.vars.size() && jsr; v++) {
            writes[v] = writes[v] || program.vars[v].function < 0;
        }
        stores = stores || jsr;
        // a call writes the callee's parameters
        for (std::int32_t callee : program.functions[f].callees) {
            for (std::int32_t param : program.functions[static_cast<std::size_t>(callee)].params) {
                writes[static_cast<std::size_t>(param)] = true;
            }
        }
        fx.stores[f] = stores;
    }
    // callees first, so each function takes in everything below it
    std::vector<std::int32_t> order = CallersFirst(program);
    for (std::size_t i = order.size(); i-- > 0;) {
        std::size_t f = static_cast<std::size_t>(order[i]);
        for (std::int32_t callee : program.functions[f].callees) {
            std::size_t c = static_cast<std::size_t>(callee);
            for (std::size_t v = 0; v < program.vars.size(); v++) {
                if (fx.writes[c][v]) {
                    fx.writes[f][v] = true;
                }
            }
            fx.stores[f] = fx.stores[f] || fx.stores[c];
        }
    }
    return fx;
}

bool CodeMayWrite(const Program& program, const Effects& effects, ExprId code, std::int32_t var) {
    if (code < 0) {
        return false;
    }
    const Var& v = program.vars[static_cast<std::size_t>(var)];
    if (v.irq_shared) {
        return true; // the interrupt handler may change it while any code runs
    }
    bool reachable = v.address_taken;
    bool found = false;
    auto visit = [&](const Expr& e) {
        if (found) {
            return;
        }
        switch (e.kind) {
        case ExprKind::Set:
            found = RootVar(program, e.a) == var;
            return;
        case ExprKind::Let:
        case ExprKind::For:
            found = e.ref == var;
            return;
        case ExprKind::Store:
        case ExprKind::StoreField:
            found = reachable;
            return;
        case ExprKind::Jsr:
            found = reachable || v.function < 0;
            return;
        case ExprKind::Call: {
            std::size_t f = static_cast<std::size_t>(e.ref);
            found = effects.writes[f][static_cast<std::size_t>(var)] ||
                    (reachable && effects.stores[f]);
            return;
        }
        default:
            return;
        }
    };
    Walk(program, code, visit);
    return found;
}

} // namespace tisple
