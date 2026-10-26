#include "link/layout.hpp"

#include <cstdio>

namespace tisple {

namespace {

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value);
    return text;
}

std::string ShortName(const Var& var) {
    std::size_t dot = var.name.rfind('.');
    return dot == std::string::npos ? var.name : var.name.substr(dot + 1);
}

// the declared variable whose bytes hold address, or -1
std::int32_t VarAt(const Program& program, const Layout& layout, std::uint32_t address) {
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const VarPlace& place = layout.vars[v];
        if (place.size > 0 && address >= place.address && address < place.address + place.size) {
            return static_cast<std::int32_t>(v);
        }
    }
    return -1;
}

} // namespace

bool CheckConstantPointers(const Program& program, const Layout& layout,
                           std::vector<Diagnostic>& errors) {
    bool ok = true;
    for (const Expr& e : program.exprs) {
        if (e.kind != ExprKind::Reinterpret || e.type <= kErrorType ||
            KindOf(program, e.type) != TypeKind::Ptr) {
            continue;
        }
        std::uint16_t address = 0;
        if (!ConstValue(program, e.a, address)) {
            continue;
        }
        std::int32_t var = VarAt(program, layout, address);
        if (var < 0) {
            continue;
        }
        const Var& v = program.vars[static_cast<std::size_t>(var)];
        const VarPlace& place = layout.vars[static_cast<std::size_t>(var)];
        std::string name = ShortName(v);
        AddError(errors, e.loc,
                 "the constant address " + Hex(address) + " is inside " + name + " (" +
                     Hex(place.address) + "-" + Hex(place.address + place.size - 1) +
                     "); a pointer into a variable comes from addr_of, as in (addr_of " + name +
                     ")");
        ok = false;
    }
    return ok;
}

} // namespace tisple
