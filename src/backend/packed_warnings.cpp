// Warnings for arrays an addr_of keeps packed (link/layout.hpp): a pointer sees packed bytes,
// so one addr_of into an array of multibyte elements stores the whole array packed, and
// every (at a i) on it works out i times the element's size where the split layout indexes
// each byte plane by i directly. The Catwiz game's redraw ran about twice as slow from one
// such addr_of on an element of an array of structs. One warning per array, at its first
// addr_of, naming how many more there are.

#include "backend/analysis.hpp"
#include "backend/backend.hpp"

#include <map>

namespace tisple {

namespace {

// the packed and the split layout of this type differ: it holds an array of elements of
// more than one byte
bool SplitDiffers(const Program& program, TypeId type) {
    const Type& t = TypeOf(program, type);
    if (t.kind == TypeKind::Array) {
        return TypeOf(program, t.elem).size > 1 || SplitDiffers(program, t.elem);
    }
    if (t.kind != TypeKind::Struct) {
        return false;
    }
    for (const Field& field : program.structs[static_cast<std::size_t>(t.decl)].fields) {
        if (SplitDiffers(program, field.type)) {
            return true;
        }
    }
    return false;
}

// the variable an addr_of's place is in, or -1
std::int32_t PlaceVar(const Program& program, ExprId id) {
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

struct Sites {
    Loc first;
    std::uint32_t count = 0;
};

} // namespace

std::vector<Diagnostic> PackedArrayWarnings(const Program& program) {
    std::map<std::int32_t, Sites> sites; // by var
    for (const Expr& e : program.exprs) {
        if (e.kind != ExprKind::AddrOf) {
            continue;
        }
        std::int32_t var = PlaceVar(program, e.a);
        if (var < 0 || !SplitDiffers(program, program.vars[static_cast<std::size_t>(var)].type)) {
            continue;
        }
        Sites& s = sites[var];
        if (s.count == 0) {
            s.first = e.loc;
        }
        s.count++;
    }
    std::vector<Diagnostic> out;
    for (const auto& [var, s] : sites) {
        const Var& v = program.vars[static_cast<std::size_t>(var)];
        const Type& t = TypeOf(program, v.type);
        // a struct with arrays in it
        std::string message = "warning: this addr_of stores all of " + v.name + " packed (" +
                              TypeName(program, v.type) +
                              "), so the arrays in it are not split into byte planes";
        if (t.kind == TypeKind::Array) {
            std::string size = std::to_string(TypeOf(program, t.elem).size);
            message = "warning: this addr_of stores all of " + v.name + " packed (" +
                      std::to_string(t.count) + " elements of " + size +
                      " bytes), so every element access multiplies its index by " + size +
                      " instead of indexing split byte planes";
        }
        if (s.count > 1) {
            message += " (and " + std::to_string(s.count - 1) + " more addr_of)";
        }
        Diagnostic d;
        d.loc = s.first;
        d.message = message;
        out.push_back(d);
    }
    return out;
}

} // namespace tisple
