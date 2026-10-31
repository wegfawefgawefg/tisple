#include "backend/link_data.hpp"

#include "backend/analysis.hpp"
#include "link/image.hpp"

namespace tisple {

namespace {

// explicit-address initial values that are not in the file are stored by the startup
constexpr std::uint32_t kMaxStartupStores = 256;

// by var: a symbol of it is in the code of a kept function
std::vector<bool> NamedByKeptCode(const Backend& be, const LinkInput& in) {
    const Program& program = *be.program;
    std::vector<bool> named(program.vars.size(), false);
    for (std::size_t f = 0; f < be.code.size(); f++) {
        if (!(*in.kept)[f]) {
            continue;
        }
        for (const AsmItem& item : be.code[f]) {
            std::int32_t sym = item.value.sym;
            if (sym >= 0 && be.info[static_cast<std::size_t>(sym)].role == SymRole::Var) {
                named[static_cast<std::size_t>(be.info[static_cast<std::size_t>(sym)].owner)] =
                    true;
            }
        }
    }
    return named;
}

} // namespace

TableUse FindTableUse(const Backend& be, const LinkInput& in) {
    const Program& program = *be.program;
    std::size_t count = program.vars.size();
    std::vector<bool> named = NamedByKeptCode(be, in);
    std::vector<bool> binary(count, false);
    std::vector<bool> any(count, false);
    for (std::size_t v = 0; v < count; v++) {
        const Var& var = program.vars[v];
        binary[v] = var.kind != VarKind::Const || var.placement.has_address || named[v];
        // by the program as written: the vars the mid-level passes add come after its own
        bool source =
            in.source_tables != nullptr && v < in.source_tables->size() && (*in.source_tables)[v];
        any[v] = binary[v] || source;
    }
    AddSlotTargets(program, binary);
    AddSlotTargets(program, any);
    TableUse use;
    use.dropped.assign(count, false);
    use.interp_only.assign(count, false);
    for (std::size_t v = 0; v < count; v++) {
        // a const :in aux the program as written may take the address of is in the binary
        // (its aux segments): an address inside the code would be main memory's
        bool aux = program.vars[v].placement.in_aux;
        use.dropped[v] = !any[v];
        use.interp_only[v] = any[v] && !binary[v] && !aux;
    }
    return use;
}

bool PlaceInterpOnly(const Backend& be, const std::vector<bool>& interp_only,
                     const std::vector<AddressRange>& runs, std::vector<VarPlace>& places) {
    places.assign(interp_only.size(), VarPlace());
    std::size_t run = 0;
    std::uint32_t at = runs.empty() ? 0 : runs[0].start;
    for (std::size_t v = 0; v < interp_only.size(); v++) {
        if (!interp_only[v]) {
            continue;
        }
        VarPlace place = be.shapes.vars[v];
        while (run < runs.size() && at + place.size > runs[run].end) {
            run++;
            at = run < runs.size() ? runs[run].start : 0;
        }
        if (run == runs.size()) {
            places.assign(interp_only.size(), VarPlace());
            return false;
        }
        place.address = static_cast<std::uint16_t>(at);
        place.interp_only = true;
        places[v] = place;
        at += place.size;
    }
    return true;
}

bool StartupStores(const Backend& be, std::uint32_t start, std::uint32_t end,
                   std::vector<std::pair<std::uint16_t, std::uint8_t>>& stores,
                   std::vector<Diagnostic>& errors) {
    const Program& program = *be.program;
    std::vector<std::uint8_t> memory(0x10000, 0);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (!var.placement.has_address || var.init.empty() || var.placement.in_aux) {
            continue;
        }
        std::uint32_t a = var.placement.address;
        std::uint32_t size = be.shapes.vars[v].size;
        if (a >= start && a + size <= end) {
            continue;
        }
        if (!var.addr_slots.empty()) {
            // the startup's stores are made before the layout gives the addresses
            AddError(errors, var.loc,
                     var.name + " is outside the program's memory, so its value is numbers "
                                "only; set its addresses at the start of main");
            return false;
        }
        Layout placed = be.shapes;
        placed.vars[v].address = static_cast<std::uint16_t>(a);
        WriteVarInit(program, placed, static_cast<std::int32_t>(v), memory);
        for (std::uint32_t i = 0; i < size; i++) {
            std::uint16_t at = static_cast<std::uint16_t>((a + i) & 0xFFFF);
            stores.push_back({at, memory[at]});
        }
    }
    if (stores.size() > kMaxStartupStores) {
        AddError(errors, Loc(),
                 "more than 256 bytes of initial values live outside the binary (explicit "
                 ":address); give those globals no value and fill them in main");
        return false;
    }
    return true;
}

} // namespace tisple
