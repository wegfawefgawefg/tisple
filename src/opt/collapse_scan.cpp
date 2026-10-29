// Which loop bodies do the same on every pass (collapse.cpp runs them once). A pass does what
// the one before did when nothing it reads was written by an earlier pass: the reads and
// writes are cells (a variable's bytes, or one element or field of it at a constant index),
// and a body is the same on every pass when no cell it writes overlaps one it reads.
//
// - Variables a let or for inside the body declares are its own: set afresh on every pass.
// - Calls read and write what their summaries say, whole variables.
// - Loads and stores through pointers reach what provenance says (collapse_memory.cpp):
//   no store may reach a cell the body reads or loads, no load a cell it writes. Memory no
//   variable uses (hardware may be behind it) is never loaded or stored in a collapsed loop.
// - io_read, io_write, jsr, wait_cycles, :address, aux and :reserve variables (hardware may
//   be behind them) and nodes not known here count as every effect: never collapsed.
// - Accumulators (collapse.hpp) are taken out first; their steps are read like the rest.
// - A body that reads its counter (the last pass decides, collapse.hpp) also needs its writes
//   to land in the same places on every pass: not under a branch on the counter or the
//   body's own variables, not in a loop inside, at places that do not depend on them, and no
//   store through a pointer or call that writes there.

#include "check/arith.hpp"
#include "opt/collapse.hpp"
#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

// bytes [lo, hi) of a variable, in its packed layout
struct Cell {
    std::int32_t var = -1;
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    bool steady = true; // a write every pass makes to the same place (see Access)
};

struct Access {
    std::vector<Cell> reads;
    std::vector<Cell> writes;
    std::vector<bool> own; // by var: let or looped over inside the body
    MemoryUse memory;      // through pointers
    bool all = false;      // I/O, jsr, wait_cycles, hardware variables, nodes not known here
    // for a body that reads its loop's counter: what may differ from pass to pass is the
    // counter and the body's own variables; varying is set under a branch on them and in loops
    // inside, where a write may not happen on every pass; unsteady: a store or a call's write
    // that may not land in the same place on every pass
    std::int32_t counter = -1;
    bool varying = false;
    bool unsteady = false;
};

bool Hardware(const Var& v) {
    return v.placement.has_address || v.placement.in_aux || v.placement.reserve;
}

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

void Scan(const Pass& p, Access& a, ExprId id);

bool Own(const Access& a, std::int32_t var) {
    return a.own[static_cast<std::size_t>(var)];
}

// does id read the counter or a variable of the body's own
bool Varies(const Pass& p, const Access& a, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::VarRef && (e.ref == a.counter || Own(a, e.ref))) {
        return true;
    }
    bool varies = false;
    EachChild(p, e, [&](ExprId child) { varies = varies || Varies(p, a, child); });
    return varies;
}

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

void ScanCall(const Pass& p, Access& a, const Expr& e) {
    const Summary& s = p.summaries[static_cast<std::size_t>(e.ref)];
    if (s.io || s.jsr || s.unknown) {
        a.all = true;
        return;
    }
    for (std::size_t v = 0; v < s.reads.size(); v++) {
        std::int32_t var = static_cast<std::int32_t>(v);
        if (s.reads[v]) {
            a.reads.push_back(WholeVar(p, var));
        }
        if (s.writes[v]) {
            a.writes.push_back(WholeVar(p, var));
            a.unsteady = true; // the callee may write it on some calls only
        }
        if ((s.reads[v] || s.writes[v]) && Hardware(VarOf(p, var))) {
            a.all = true;
        }
    }
    if (s.loads || s.stores) {
        a.memory.calls.push_back(e.ref);
    }
    a.unsteady = a.unsteady || s.stores;
    for (std::uint32_t i = 0; i < e.count; i++) {
        Scan(p, a, Item(p, e, i));
    }
}

void Scan(const Pass& p, Access& a, ExprId id) {
    if (id < 0 || a.all) {
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
        a.own[static_cast<std::size_t>(e.ref)] = true;
        break;
    case ExprKind::For:
        // an inner loop may make a different number of passes each time
        a.own[static_cast<std::size_t>(e.ref)] = true;
        Scan(p, a, e.a);
        Scan(p, a, e.b);
        ScanUnder(p, a, id, e.c);
        return;
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
        ScanCall(p, a, e);
        return;
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::StructLit:
    case ExprKind::Do:
    case ExprKind::Return:
        break;
    default:
        // I/O, jsr, wait_cycles and anything not known here
        a.all = true;
        return;
    }
    EachChild(p, e, [&](ExprId child) { Scan(p, a, child); });
}

bool Overlap(const Cell& x, const Cell& y) {
    return x.var == y.var && x.lo < y.hi && y.lo < x.hi;
}

// does a pass read a cell some pass writes (the body's own variables aside)
bool Conflict(const Pass& p, MemoryFacts& m, const Access& a) {
    if (a.all) {
        return true;
    }
    for (const Cell& w : a.writes) {
        for (const Cell& r : a.reads) {
            if (!Own(a, w.var) && Overlap(w, r)) {
                return true;
            }
        }
    }
    if (a.memory.load_ptrs.empty() && a.memory.store_ptrs.empty() && a.memory.calls.empty()) {
        return false;
    }
    MemoryUse use = a.memory;
    for (const Cell& w : a.writes) {
        if (!Own(a, w.var)) {
            use.written.push_back(w.var);
        }
    }
    for (const Cell& r : a.reads) {
        if (!Own(a, r.var)) {
            use.read.push_back(r.var);
        }
    }
    return MemoryClash(p, m, use);
}

bool Mentions(const std::vector<Cell>& cells, std::int32_t var) {
    for (const Cell& c : cells) {
        if (c.var == var) {
            return true;
        }
    }
    return false;
}

bool IsVar(const Pass& p, ExprId id, std::int32_t var) {
    const Expr& e = At(p, id);
    return e.kind == ExprKind::VarRef && e.ref == var;
}

// (set x (op x k)), or (op k x) for an op that commutes; false for anything else
bool AsAccumulator(const Pass& p, ExprId stmt, Accumulator& acc) {
    const Expr& set = At(p, stmt);
    if (set.kind != ExprKind::Set || At(p, set.a).kind != ExprKind::VarRef) {
        return false;
    }
    std::int32_t x = At(p, set.a).ref;
    const Var& v = VarOf(p, x);
    const Expr& value = At(p, set.b);
    if (!IsIntegerKind(KindOf(*p.program, v.type)) || Hardware(v) || v.address_taken ||
        value.kind != ExprKind::Binary) {
        return false;
    }
    bool commutes = value.op != Op::Sub;
    ExprId step = -1;
    if (IsVar(p, value.a, x)) {
        step = value.b;
    } else if (commutes && IsVar(p, value.b, x)) {
        step = value.a;
    }
    switch (value.op) {
    case Op::Add:
    case Op::Sub:
    case Op::Xor:
    case Op::Or:
    case Op::And:
        break;
    default:
        return false;
    }
    if (step < 0 || !IsPure(p, step)) {
        return false;
    }
    acc.stmt = stmt;
    acc.var = x;
    acc.op = value.op;
    acc.step = step;
    return true;
}

// every write outside the body's own variables lands in the same place on every pass, and
// each accumulator's step is the same
bool LastPassDecides(const Pass& p, const Access& a, const BodyPlan& plan) {
    if (a.unsteady) {
        return false;
    }
    for (const Cell& w : a.writes) {
        if (!w.steady && !Own(a, w.var)) {
            return false;
        }
    }
    for (const Accumulator& acc : plan.accumulators) {
        if (Varies(p, a, acc.step)) {
            return false;
        }
    }
    return true;
}

} // namespace

void PassStatements(const Pass& p, ExprId body, std::vector<ExprId>& out) {
    const Expr& e = At(p, body);
    if (e.kind != ExprKind::Do || e.type != kVoidType || Shared(p, body)) {
        out.push_back(body);
        return;
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        PassStatements(p, Item(p, e, i), out);
    }
}

BodyPlan PlanBody(const Pass& p, MemoryFacts& m, ExprId body, std::int32_t counter, ExprId skip) {
    BodyPlan plan;
    if (MayLeave(p, body, 0)) {
        return plan;
    }
    Access a;
    a.own.assign(p.program->vars.size(), false);
    a.counter = counter;
    std::vector<ExprId> items;
    PassStatements(p, body, items);
    for (ExprId item : items) {
        Accumulator acc;
        if (item == skip) {
            continue;
        }
        if (AsAccumulator(p, item, acc) && acc.var != counter && !Own(a, acc.var)) {
            plan.accumulators.push_back(acc);
            Scan(p, a, acc.step);
            continue;
        }
        Scan(p, a, item);
    }
    if (Conflict(p, m, a)) {
        return plan;
    }
    if (counter >= 0 &&
        (Mentions(a.writes, counter) || Own(a, counter) || VarOf(p, counter).address_taken)) {
        return plan;
    }
    // a body that reads the counter: the last pass decides, when every pass writes the same
    // places and the accumulators add the same each pass
    plan.last = counter >= 0 && Mentions(a.reads, counter);
    if (plan.last && !LastPassDecides(p, a, plan)) {
        return plan;
    }
    for (std::size_t i = 0; i < plan.accumulators.size(); i++) {
        std::int32_t x = plan.accumulators[i].var;
        if (Mentions(a.reads, x) || Mentions(a.writes, x) || Own(a, x)) {
            return plan;
        }
        for (std::size_t j = i + 1; j < plan.accumulators.size(); j++) {
            if (plan.accumulators[j].var == x) {
                return plan;
            }
        }
    }
    plan.same = true;
    return plan;
}

} // namespace tisple::opt
