// What code may do while the aux memory switches may be on: inside a with_aux, and in every
// function such code calls (check/aux_mode.hpp). Zero page, the stack and $C000 up are the
// same memory whatever the switches say; $0200-$BFFF is not.
//
// - RAMWRT on: no set of a global outside zero page (it would go to aux memory's byte at the
//   global's address).
// - RAMRD on: no read of a global outside zero page (it would read aux memory's). Const data
//   is mirrored by the link, so it reads the same; a const :in aux is only there
//   (aux_consts.cpp).
// - Either: no call through a function pointer, no jsr, io_read or io_write at an address
//   worked out at run time (each stores into its own code, which aux memory's copy would not
//   see), and no with_aux in a function that runs inside one.
// Frames are the backend's: every frame such code uses lives in zero page.

#include "check/aux_mode.hpp"
#include "check/checker.hpp"

#include <set>
#include <string>

namespace tisple {

namespace {

struct Rules {
    Checker* ck = nullptr;
    const AuxModes* modes = nullptr;
    std::int32_t function = -1;
    std::vector<std::int32_t> caller; // by function: who runs it inside a with_aux
    std::set<std::pair<std::int32_t, std::int32_t>> named; // (function, var) already reported
};

const Expr& At(const Rules& r, ExprId id) {
    return r.ck->out->exprs[static_cast<std::size_t>(id)];
}

bool InZeroPage(const Checker& ck, const Var& var) {
    std::uint32_t size = TypeOf(*ck.out, var.type).size;
    return var.placement.has_address && var.placement.address + size <= 0x100;
}

std::string ShortName(const Var& var) {
    return var.name.substr(var.name.rfind('.') + 1);
}

// " (f runs inside the with_aux of g)" for a function called from a region, else ""
std::string Where(const Rules& r) {
    std::size_t f = static_cast<std::size_t>(r.function);
    if (r.modes->entry[f] == 0 || r.caller[f] < 0) {
        return "";
    }
    const Program& program = *r.ck->out;
    return " (" + program.functions[f].name + " runs inside a with_aux, called from " +
           program.functions[static_cast<std::size_t>(r.caller[f])].name + ")";
}

void ReportGlobal(Rules& r, ExprId id, std::int32_t var_id, bool write) {
    const Var& var = r.ck->out->vars[static_cast<std::size_t>(var_id)];
    if (var.kind != VarKind::Global || InZeroPage(*r.ck, var)) {
        return;
    }
    if (!r.named.insert({r.function, var_id}).second) {
        return;
    }
    std::string name = ShortName(var);
    std::string message =
        write ? "(set " + name + " ...) with RAMWRT on would write aux memory's byte at " + name +
                    "'s address: " + name +
                    " is a global outside zero page; set it after the "
                    "with_aux, or give it an :address in zero page"
              : name + " is read with RAMRD on, which would read aux memory's byte at its "
                       "address: it is a global outside zero page; copy it into a local "
                       "before the with_aux, or give it an :address in zero page";
    AddError(*r.ck->errors, At(r, id).loc, message + Where(r));
}

void Scan(Rules& r, ExprId id);

// a place: its root is written (or, under addr_of, only named), its indexes are read
void ScanPlace(Rules& r, ExprId id, bool write) {
    const Expr& e = At(r, id);
    std::uint8_t state = AuxStateAt(*r.modes, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        if (write && (state & kAuxWrites) != 0) {
            ReportGlobal(r, id, e.ref, true);
        }
        return;
    case ExprKind::Index:
        ScanPlace(r, e.a, write);
        Scan(r, e.b);
        return;
    case ExprKind::Field:
        ScanPlace(r, e.a, write);
        return;
    default:
        Scan(r, id);
        return;
    }
}

void Fail(Rules& r, ExprId id, const std::string& message) {
    AddError(*r.ck->errors, At(r, id).loc, message + Where(r));
}

void ScanItems(Rules& r, const Expr& e) {
    for (std::uint32_t i = 0; i < e.count; i++) {
        Scan(r, r.ck->out->lists[e.first + i]);
    }
}

void Scan(Rules& r, ExprId id) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(r, id);
    std::uint8_t state = AuxStateAt(*r.modes, id);
    std::uint16_t address = 0;
    switch (e.kind) {
    case ExprKind::VarRef:
        if ((state & kAuxReads) != 0) {
            ReportGlobal(r, id, e.ref, false);
        }
        return;
    case ExprKind::Set:
        ScanPlace(r, e.a, true);
        Scan(r, e.b);
        return;
    case ExprKind::AddrOf:
        ScanPlace(r, e.a, false);
        return;
    case ExprKind::CallPtr:
        if (state != 0) {
            Fail(r, id,
                 "a call through a function pointer with the aux switches on: its dispatcher "
                 "stores the address into its own code; call the function by name");
        }
        break;
    case ExprKind::Jsr:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
        if (state != 0 && !ConstValue(*r.ck->out, e.a, address)) {
            Fail(r, id,
                 "an address worked out at run time with the aux switches on: the code stores "
                 "it into its own instruction, which aux memory's copy does not see; use a "
                 "constant address, or do it outside the with_aux");
        }
        break;
    case ExprKind::AuxSwitch:
        if (e.value != 0 && state != 0) {
            Fail(r, id,
                 "with_aux inside with_aux: the inner one would turn the switches off before "
                 "the outer one ends; give the outer one the switches both need");
        }
        break;
    default:
        break;
    }
    Scan(r, e.a);
    Scan(r, e.b);
    Scan(r, e.c);
    bool items = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ||
                 e.kind == ExprKind::StructLit || e.kind == ExprKind::Do;
    if (items) {
        ScanItems(r, e);
    }
    for (std::uint32_t i = 0; e.kind == ExprKind::Match && i < e.count; i++) {
        Scan(r, r.ck->out->clauses[e.first + i].body);
    }
}

// by function: a function that calls it with a switch on
std::vector<std::int32_t> FindCallers(const Program& program, const AuxModes& modes) {
    std::vector<std::int32_t> caller(program.functions.size(), -1);
    std::vector<std::int32_t> owner(program.exprs.size(), -1);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        std::vector<ExprId> work;
        if (program.functions[f].body >= 0) {
            work.push_back(program.functions[f].body);
        }
        while (!work.empty()) {
            ExprId id = work.back();
            work.pop_back();
            const Expr& e = program.exprs[static_cast<std::size_t>(id)];
            owner[static_cast<std::size_t>(id)] = static_cast<std::int32_t>(f);
            for (ExprId child : {e.a, e.b, e.c}) {
                if (child >= 0) {
                    work.push_back(child);
                }
            }
            bool items = e.kind == ExprKind::Call || e.kind == ExprKind::CallPtr ||
                         e.kind == ExprKind::StructLit || e.kind == ExprKind::Do;
            for (std::uint32_t i = 0; items && i < e.count; i++) {
                work.push_back(program.lists[e.first + i]);
            }
            for (std::uint32_t i = 0; e.kind == ExprKind::Match && i < e.count; i++) {
                work.push_back(program.clauses[e.first + i].body);
            }
        }
    }
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        ExprId site = modes.entry_site[f];
        if (site >= 0) {
            caller[f] = owner[static_cast<std::size_t>(site)];
        }
    }
    return caller;
}

} // namespace

void CheckAuxCode(Checker& ck) {
    const Program& program = *ck.out;
    AuxModes modes = FindAuxModes(program);
    CheckAuxConstReads(ck, modes);
    if (!modes.any) {
        return;
    }
    Rules r;
    r.ck = &ck;
    r.modes = &modes;
    r.caller = FindCallers(program, modes);
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        if (!AuxFunction(modes, static_cast<std::int32_t>(f))) {
            continue;
        }
        r.function = static_cast<std::int32_t>(f);
        Scan(r, program.functions[f].body);
    }
}

} // namespace tisple
