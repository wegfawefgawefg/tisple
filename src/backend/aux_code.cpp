#include "backend/aux_code.hpp"

namespace tisple {

namespace {

struct Finder {
    const Program* program = nullptr;
    AuxCode* out = nullptr;
};

void Note(Finder& f, ExprId id, std::int32_t var) {
    if (var < 0) {
        return;
    }
    const Var& v = f.program->vars[static_cast<std::size_t>(var)];
    std::size_t at = static_cast<std::size_t>(var);
    if (v.function >= 0) {
        f.out->vars[at] = true;
    }
    if (v.kind == VarKind::Const && (AuxStateAt(f.out->modes, id) & kAuxReads) != 0) {
        f.out->consts[at] = true;
    }
}

void Scan(Finder& f, ExprId id) {
    if (id < 0) {
        return;
    }
    const Program& program = *f.program;
    const Expr& e = program.exprs[static_cast<std::size_t>(id)];
    if (e.kind == ExprKind::VarRef || e.kind == ExprKind::Let || e.kind == ExprKind::For) {
        Note(f, id, e.ref);
    }
    Scan(f, e.a);
    Scan(f, e.b);
    Scan(f, e.c);
    bool items = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ||
                 e.kind == ExprKind::StructLit || e.kind == ExprKind::Do;
    for (std::uint32_t i = 0; items && i < e.count; i++) {
        Scan(f, program.lists[e.first + i]);
    }
    for (std::uint32_t i = 0; e.kind == ExprKind::Match && i < e.count; i++) {
        Scan(f, program.clauses[e.first + i].body);
    }
}

} // namespace

AuxCode FindAuxCode(const Program& program) {
    AuxCode aux;
    aux.modes = FindAuxModes(program);
    aux.any = aux.modes.any;
    aux.vars.assign(program.vars.size(), false);
    aux.consts.assign(program.vars.size(), false);
    if (!aux.any) {
        return aux;
    }
    Finder f;
    f.program = &program;
    f.out = &aux;
    for (std::size_t fn = 0; fn < program.functions.size(); fn++) {
        std::int32_t id = static_cast<std::int32_t>(fn);
        if (!AuxFunction(aux.modes, id)) {
            continue;
        }
        aux.reads = aux.reads || AuxMirrored(aux.modes, id);
        const Function& function = program.functions[fn];
        for (std::int32_t v : function.params) {
            aux.vars[static_cast<std::size_t>(v)] = true;
        }
        for (std::int32_t v : function.locals) {
            aux.vars[static_cast<std::size_t>(v)] = true;
        }
        Scan(f, function.body);
    }
    return aux;
}

} // namespace tisple
