#include "backend/analysis.hpp"

// Locals that only ever hold 1 or -1: a Bresenham step, a direction. (set x (+ x step))
// then needs no add: the step's sign picks INC or DEC (GenSet, gen_stmt.cpp).

namespace tisple {

namespace {

// 1 or -1 in a size-byte integer
bool IsUnit(const Program& program, ExprId id, std::uint32_t size) {
    std::uint16_t c = 0;
    if (id < 0 || !ConstValue(program, id, c)) {
        return false;
    }
    std::uint16_t minus_one = size == 1 ? 0xFF : 0xFFFF;
    return c == 1 || c == minus_one;
}

} // namespace

std::vector<bool> FindUnitSteps(const Program& program) {
    std::vector<bool> unit(program.vars.size(), false);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        TypeKind kind = KindOf(program, var.type);
        bool integer = kind == TypeKind::U8 || kind == TypeKind::I8 || kind == TypeKind::U16 ||
                       kind == TypeKind::I16;
        unit[v] = var.kind == VarKind::Local && !var.address_taken && !var.loop_var &&
                  !var.placement.has_address && integer;
    }
    // every let and set of the variable gives it 1 or -1
    for (const Expr& e : program.exprs) {
        std::int32_t var = -1;
        ExprId value = -1;
        if (e.kind == ExprKind::Let) {
            var = e.ref;
            value = e.a;
        } else if (e.kind == ExprKind::Set && ExprOf(program, e.a).kind == ExprKind::VarRef) {
            var = ExprOf(program, e.a).ref;
            value = e.b;
        }
        if (var < 0 || !unit[static_cast<std::size_t>(var)]) {
            continue;
        }
        std::uint32_t size = TypeOf(program, program.vars[static_cast<std::size_t>(var)].type).size;
        if (!IsUnit(program, value, size)) {
            unit[static_cast<std::size_t>(var)] = false;
        }
    }
    // a local no let names is not one (there is always one, but be sure)
    std::vector<bool> named(program.vars.size(), false);
    for (const Expr& e : program.exprs) {
        if (e.kind == ExprKind::Let) {
            named[static_cast<std::size_t>(e.ref)] = true;
        }
    }
    for (std::size_t v = 0; v < unit.size(); v++) {
        unit[v] = unit[v] && named[v];
    }
    return unit;
}

} // namespace tisple
