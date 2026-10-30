// What a loop body reads and writes, for loop collapse (collapse_plan.cpp decides): cells (a
// variable's bytes, or one element or field of it at a constant index), loads and stores
// through pointers, calls.
//
// - Variables a let or for inside the body declares are its own: set afresh on every pass.
// - Calls read and write what their summaries say, whole variables.
// - Loads and stores through pointers reach what provenance says (collapse_memory.cpp):
//   no store may reach a cell the body reads or loads, no load a cell it writes. Memory no
//   variable uses (hardware may be behind it) is never loaded or stored in a collapsed loop.
// - io_read, io_write, jsr, wait_cycles, :address, aux and :reserve variables (hardware may
//   be behind them) and nodes not known here count as every effect: never collapsed; nor
//   is a body of more than kMaxScan nodes.
// - For a body that reads its counter (collapse_plan.cpp): which writes land in the same place
//   on every pass: not under a branch on the counter or the body's own variables, not in a
//   loop inside that may make different passes, at places that do not depend on them.

#include "opt/collapse.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

bool Hardware(const Var& v) {
    return v.placement.has_address || v.placement.in_aux || v.placement.reserve || v.irq_shared;
}

namespace {

Cell WholeVar(const Pass& p, std::int32_t var) {
    Cell c;
    c.var = var;
    c.hi = TypeOf(*p.program, VarOf(p, var).type).size;
    return c;
}

// the cell a place rooted at a variable names: one element or field when every index on the
// way is a literal in range, else the whole variable; false for a place behind a pointer
bool PlaceCell(const Pass& p, ExprId place, Cell& cell, bool& exact) {
    const Expr& e = At(p, place);
    switch (e.kind) {
    case ExprKind::VarRef:
        cell = WholeVar(p, e.ref);
        exact = true;
        return true;
    case ExprKind::Index: {
        if (!PlaceCell(p, e.a, cell, exact)) {
            return false;
        }
        const Type& array = TypeOf(*p.program, At(p, e.a).type);
        std::uint16_t index = 0;
        if (!exact || !ConstValue(*p.program, e.b, index) || index >= array.count) {
            exact = false;
            cell = WholeVar(p, cell.var);
            return true;
        }
        cell.lo += index * TypeOf(*p.program, e.type).size;
        cell.hi = cell.lo + TypeOf(*p.program, e.type).size;
        return true;
    }
    case ExprKind::Field:
        if (!PlaceCell(p, e.a, cell, exact)) {
            return false;
        }
        if (exact) {
            cell.lo += e.value;
            cell.hi = cell.lo + TypeOf(*p.program, e.type).size;
        }
        return true;
    default:
        return false;
    }
}

} // namespace

bool Own(const Access& a, std::int32_t var) {
    return a.own[static_cast<std::size_t>(var)];
}

// does id read the counter or a variable of the body's own
namespace {

bool Differs(const Access& a, std::int32_t var) {
    return var == a.counter || (Own(a, var) && !a.steady[static_cast<std::size_t>(var)]);
}

// the globals a call reads: for one whose value is unused, not those only its value reads
const std::vector<bool>& CallReads(const Summary& s, bool unused) {
    return unused ? s.effect_reads : s.reads;
}

bool CallVaries(const Pass& p, const Access& a, const Expr& call, bool unused = false) {
    const Summary& s = p.summaries[static_cast<std::size_t>(call.ref)];
    if (a.own_pointed && s.loads) {
        return true;
    }
    for (std::int32_t var : a.own_globals) {
        if (CallReads(s, unused)[static_cast<std::size_t>(var)] && Differs(a, var)) {
            return true;
        }
    }
    return false;
}

} // namespace

bool Varies(const Pass& p, const Access& a, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && Differs(a, e.ref)) {
        return true;
    }
    bool load = e.kind == ExprKind::Load || e.kind == ExprKind::LoadField;
    if ((load && a.own_pointed) || (e.kind == ExprKind::Call && CallVaries(p, a, e))) {
        return true;
    }
    bool varies = false;
    EachChild(p, e, [&](ExprId child) { varies = varies || Varies(p, a, child); });
    return varies;
}

namespace {

// scans the parts of id that may not run on every pass, under varying when the test does (a
// loop as the test: always)
void ScanUnder(const Pass& p, Access& a, ExprId test, ExprId id) {
    bool was = a.varying;
    ExprKind kind = test >= 0 ? At(p, test).kind : ExprKind::Literal;
    bool loop = kind == ExprKind::While || kind == ExprKind::For;
    a.varying = a.varying || loop || Varies(p, a, test);
    Scan(p, a, id);
    a.varying = was;
}

// a place read (write false) or written: its cell, and the index expressions it reads
void ScanPlace(const Pass& p, Access& a, ExprId place, bool write) {
    Cell cell;
    bool exact = false;
    if (PlaceCell(p, place, cell, exact)) {
        if (Hardware(VarOf(p, cell.var))) {
            a.all = true;
        }
        cell.steady = !a.varying && (exact || !Varies(p, a, place));
        (write ? a.writes : a.reads).push_back(cell);
    }
    // the indexes on the way, and a pointer the place may sit behind
    for (ExprId at = place; at >= 0;) {
        const Expr& e = At(p, at);
        if (e.kind == ExprKind::Index) {
            Scan(p, a, e.b);
        } else if (e.kind == ExprKind::Load || e.kind == ExprKind::LoadField) {
            (write ? a.memory.store_ptrs : a.memory.load_ptrs).push_back(e.a);
            a.unsteady = a.unsteady || (write && (a.varying || Varies(p, a, place)));
            Scan(p, a, e.a);
            return;
        } else if (e.kind != ExprKind::Field) {
            return;
        }
        at = e.a;
    }
}

// an addr_of's place: only its indexes are worked out
void ScanAddress(const Pass& p, Access& a, ExprId place) {
    for (ExprId at = place; at >= 0;) {
        const Expr& e = At(p, at);
        if (e.kind == ExprKind::Index) {
            Scan(p, a, e.b);
        } else if (e.kind == ExprKind::Load || e.kind == ExprKind::LoadField) {
            Scan(p, a, e.a);
            return;
        } else if (e.kind != ExprKind::Field) {
            return;
        }
        at = e.a;
    }
}

// unused: a statement, whose value nobody reads
void ScanCall(const Pass& p, Access& a, const Expr& e, bool unused) {
    const Summary& s = p.summaries[static_cast<std::size_t>(e.ref)];
    const std::vector<bool>& reads = CallReads(s, unused);
    if (s.io || s.jsr || s.unknown) {
        a.all = true;
        return;
    }
    // with the same arguments on every pass a call does the same (what it reads no pass
    // writes, or the loop does not collapse): its writes land where they did
    bool same = !a.varying && !CallVaries(p, a, e, unused);
    for (std::uint32_t i = 0; i < e.count && same; i++) {
        same = !Varies(p, a, Item(p, e, i));
    }
    for (std::size_t v = 0; v < s.reads.size(); v++) {
        std::int32_t var = static_cast<std::int32_t>(v);
        if (reads[v]) {
            a.reads.push_back(WholeVar(p, var));
        }
        if (s.writes[v]) {
            Cell cell = WholeVar(p, var);
            cell.steady = same;
            a.writes.push_back(cell);
        }
        if ((s.reads[v] || s.writes[v]) && Hardware(VarOf(p, var))) {
            a.all = true;
        }
    }
    if (s.loads || s.stores) {
        a.memory.calls.push_back(e.ref);
    }
    a.unsteady = a.unsteady || (s.stores && !same);
    for (std::uint32_t i = 0; i < e.count; i++) {
        Scan(p, a, Item(p, e, i));
    }
}

} // namespace

void Scan(const Pass& p, Access& a, ExprId id) {
    if (id < 0 || a.all) {
        return;
    }
    if (--a.budget < 0) {
        a.all = true;
        return;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
        ScanPlace(p, a, id, false);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        a.memory.load_ptrs.push_back(e.a);
        Scan(p, a, e.a);
        return;
    case ExprKind::Store:
    case ExprKind::StoreField:
        a.memory.store_ptrs.push_back(e.a);
        a.unsteady = a.unsteady || a.varying || Varies(p, a, e.a);
        Scan(p, a, e.a);
        Scan(p, a, e.b);
        return;
    case ExprKind::AddrOf:
        ScanAddress(p, a, e.a);
        return;
    case ExprKind::Set:
        ScanPlace(p, a, e.a, true);
        Scan(p, a, e.b);
        return;
    case ExprKind::Let:
        a.steady[static_cast<std::size_t>(e.ref)] =
            !a.varying && !Varies(p, a, e.a) && CountSets(p, a.body, e.ref) == 1;
        a.own[static_cast<std::size_t>(e.ref)] = true;
        a.own_pointed = a.own_pointed || VarOf(p, e.ref).address_taken;
        break;
    case ExprKind::For: {
        // a for inside makes the same passes on every pass of the loop when its range is the
        // same and nothing ends it early
        bool same = !a.varying && !Varies(p, a, e.a) && !Varies(p, a, e.b) &&
                    !MayLeave(p, e.c, 0) && !VarOf(p, e.ref).address_taken;
        a.steady[static_cast<std::size_t>(e.ref)] = same;
        a.own[static_cast<std::size_t>(e.ref)] = true;
        Scan(p, a, e.a);
        Scan(p, a, e.b);
        if (same) {
            Scan(p, a, e.c);
        } else {
            ScanUnder(p, a, id, e.c);
        }
        return;
    }
    case ExprKind::While:
        ScanUnder(p, a, id, e.a);
        ScanUnder(p, a, id, e.b);
        return;
    case ExprKind::If:
        Scan(p, a, e.a);
        ScanUnder(p, a, e.a, e.b);
        ScanUnder(p, a, e.a, e.c);
        return;
    case ExprKind::And:
    case ExprKind::Or:
        Scan(p, a, e.a);
        ScanUnder(p, a, e.a, e.b);
        return;
    case ExprKind::Match:
        Scan(p, a, e.a);
        for (std::uint32_t i = 0; i < e.count; i++) {
            ScanUnder(p, a, e.a, p.program->clauses[e.first + i].body);
        }
        return;
    case ExprKind::Call:
        ScanCall(p, a, e, false);
        return;
    case ExprKind::Do:
        // a call as an item whose value is unused reads only what its effects need
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId item = Item(p, e, i);
            bool unused = i + 1 < e.count || e.type == kVoidType;
            if (unused && At(p, item).kind == ExprKind::Call && !a.all && --a.budget >= 0) {
                ScanCall(p, a, At(p, item), true);
            } else {
                Scan(p, a, item);
            }
        }
        return;
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::StructLit:
    case ExprKind::Return:
        break;
    default:
        // I/O, jsr, wait_cycles and anything not known here
        a.all = true;
        return;
    }
    EachChild(p, e, [&](ExprId child) { Scan(p, a, child); });
}

} // namespace tisple::opt
