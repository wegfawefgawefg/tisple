// Reads of a const :in aux by name (docs/language.md, Aux memory code). Its bytes are only in
// aux memory, so a read by name, an absolute or indexed load, gets them only with RAMRD on:
//
// - where RAMRD is surely on (AuxModes::sure): inside a (with_aux true ...), or in a function
//   every call of which is made there;
// - anywhere, a number read at constant indexes: it is folded while compiling, at -O0 too,
//   and reads nothing at run time;
// - anywhere, the root of an addr_of: its aux address.
// Anything else is an error that says where to read it.

#include "check/aux_mode.hpp"
#include "check/checker.hpp"

namespace tisple {

namespace {

struct Reads {
    Checker* ck = nullptr;
    const AuxModes* modes = nullptr;
};

const Expr& At(const Reads& r, ExprId id) {
    return r.ck->out->exprs[static_cast<std::size_t>(id)];
}

bool IsPlace(ExprKind kind) {
    return kind == ExprKind::VarRef || kind == ExprKind::Index || kind == ExprKind::Field;
}

// the VarRef a place chain starts at, or -1 (a Load under it)
ExprId RootOf(const Reads& r, ExprId id) {
    while (At(r, id).kind == ExprKind::Index || At(r, id).kind == ExprKind::Field) {
        id = At(r, id).a;
    }
    return At(r, id).kind == ExprKind::VarRef ? id : -1;
}

bool IsAuxConst(const Reads& r, ExprId ref) {
    const Var& var = r.ck->out->vars[static_cast<std::size_t>(At(r, ref).ref)];
    return var.kind == VarKind::Const && var.placement.in_aux;
}

void Report(Reads& r, ExprId root) {
    const Expr& e = At(r, root);
    const Var& var = r.ck->out->vars[static_cast<std::size_t>(At(r, root).ref)];
    std::string name = var.name.substr(var.name.rfind('.') + 1);
    std::string message =
        name +
        " is in aux memory: read it by name inside (with_aux true false ...), or in a "
        "function only such code calls; outside, (addr_of " +
        name + " ...) is its aux address";
    if ((AuxStateAt(*r.modes, root) & kAuxReads) != 0) {
        message += " (here RAMRD may be off: the with_aux's reads is not the constant true, or "
                   "this function is also called with it off)";
    }
    AddError(*r.ck->errors, e.loc, message);
}

void Scan(Reads& r, ExprId id);

// the indexes of a place chain, and a Load at its base
void ScanIndexes(Reads& r, ExprId id) {
    while (At(r, id).kind == ExprKind::Index || At(r, id).kind == ExprKind::Field) {
        if (At(r, id).kind == ExprKind::Index) {
            Scan(r, At(r, id).b);
        }
        id = At(r, id).a;
    }
    if (At(r, id).kind != ExprKind::VarRef) {
        Scan(r, id);
    }
}

void ScanRead(Reads& r, ExprId place) {
    ExprId root = RootOf(r, place);
    std::uint16_t value = 0;
    bool ok = root < 0 || !IsAuxConst(r, root) || (AuxSureAt(*r.modes, root) & kAuxReads) != 0 ||
              ConstValue(*r.ck->out, place, value);
    if (!ok) {
        Report(r, root);
    }
    ScanIndexes(r, place);
}

void Scan(Reads& r, ExprId id) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(r, id);
    if (e.kind == ExprKind::AddrOf) {
        ScanIndexes(r, e.a);
        return;
    }
    if (IsPlace(e.kind)) {
        ScanRead(r, id);
        return;
    }
    Scan(r, e.a);
    Scan(r, e.b);
    Scan(r, e.c);
    bool items = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ||
                 e.kind == ExprKind::StructLit || e.kind == ExprKind::Do;
    for (std::uint32_t i = 0; items && i < e.count; i++) {
        Scan(r, r.ck->out->lists[e.first + i]);
    }
    for (std::uint32_t i = 0; e.kind == ExprKind::Match && i < e.count; i++) {
        Scan(r, r.ck->out->clauses[e.first + i].body);
    }
}

bool AnyAuxConst(const Program& program) {
    for (const Var& var : program.vars) {
        if (var.kind == VarKind::Const && var.placement.in_aux) {
            return true;
        }
    }
    return false;
}

} // namespace

void CheckAuxConstReads(Checker& ck, const AuxModes& modes) {
    const Program& program = *ck.out;
    if (!AnyAuxConst(program)) {
        return;
    }
    Reads r;
    r.ck = &ck;
    r.modes = &modes;
    for (const Function& fn : program.functions) {
        Scan(r, fn.body);
    }
}

} // namespace tisple
