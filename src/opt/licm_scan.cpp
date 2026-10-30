// What loop-invariant code motion may move (licm.cpp moves it).
//
// - Inputs: variables no set, let or for in the loop writes and no call in it writes (its
//   effect summary); address-taken variables also need no store through a pointer in the
//   loop; const arrays always hold. A loop with jsr, io_write, a call doing I/O or a node not
//   known here keeps everything. Pointer loads, :address and aux variables never move
//   (hardware may be behind them), nor calls to functions that load through pointers.
// - Worth it (licm_cost.cpp): divisions, multiplies, shifts by a variable, wide arithmetic,
//   computed indexes, calls; never a bool, which a branch tests directly.
// - Only what runs whenever the loop's body runs: not an arm of an if or match, nor the right
//   side of an and/or, nor the body of a loop inside that may run zero times. Moving those
//   would be safe (they are pure, and division by zero has a defined result) but costs
//   cycles each time the arm or the loop does not run, which in practice is most of the time.

#include "opt/licm.hpp"

namespace tisple::opt {

namespace {

void ScanWrites(const Pass& p, ExprId id, LoopWrites& w) {
    if (id < 0 || w.all) {
        return;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Set: {
        std::int32_t root = PlaceRoot(p, e.a);
        if (root < 0) {
            w.stores = true;
        } else {
            w.vars[static_cast<std::size_t>(root)] = true;
        }
        break;
    }
    case ExprKind::Let:
    case ExprKind::For:
        w.vars[static_cast<std::size_t>(e.ref)] = true;
        break;
    case ExprKind::Store:
    case ExprKind::StoreField:
        w.stores = true;
        break;
    case ExprKind::Call:
    case ExprKind::CallPtr: {
        const Summary& s = CallSummary(p, e);
        if (s.io || s.jsr || s.unknown) {
            w.all = true;
            return;
        }
        for (std::size_t v = 0; v < s.writes.size(); v++) {
            if (s.writes[v]) {
                w.vars[v] = true;
            }
        }
        w.stores = w.stores || s.stores;
        break;
    }
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
    case ExprKind::Interrupts:
        w.all = true;
        return;
    case ExprKind::Literal:
    case ExprKind::FnRef:
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::IoRead:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::If:
    case ExprKind::Do:
    case ExprKind::While:
    case ExprKind::Match:
    case ExprKind::Return:
    case ExprKind::Break:
    case ExprKind::Continue:
        break;
    default:
        w.all = true; // not known here
        return;
    }
    EachChild(p, e, [&](ExprId child) { ScanWrites(p, child, w); });
}

// a variable that holds its value while the loop runs
bool Holds(const Pass& p, std::int32_t var, const LoopWrites& w) {
    const Var& v = VarOf(p, var);
    if (v.placement.has_address || v.placement.in_aux || v.placement.reserve || v.irq_shared) {
        return false;
    }
    if (v.kind == VarKind::Const) {
        return true;
    }
    if (w.all ||
        (static_cast<std::size_t>(var) < w.vars.size() && w.vars[static_cast<std::size_t>(var)])) {
        return false;
    }
    return !(v.address_taken && w.stores);
}

bool Invariant(const Pass& p, ExprId id, const LoopWrites& w);

// a place read as a value: its variable holds and its indexes are invariant
bool PlaceHolds(const Pass& p, ExprId id, const LoopWrites& w) {
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return Holds(p, e.ref, w);
    case ExprKind::Index:
        return PlaceHolds(p, e.a, w) && Invariant(p, e.b, w);
    case ExprKind::Field:
        return PlaceHolds(p, e.a, w);
    default:
        return false; // through a pointer
    }
}

// a place's address: its indexes are invariant (what it holds does not matter)
bool AddressHolds(const Pass& p, ExprId id, const LoopWrites& w) {
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return true;
    case ExprKind::Index:
        return AddressHolds(p, e.a, w) && Invariant(p, e.b, w);
    case ExprKind::Field:
        return AddressHolds(p, e.a, w);
    default:
        return false;
    }
}

bool CallHolds(const Pass& p, const Expr& e, const LoopWrites& w) {
    if (!PureFunction(p, e.ref)) {
        return false;
    }
    const Summary& s = p.summaries[static_cast<std::size_t>(e.ref)];
    if (s.loads) {
        return false;
    }
    for (std::size_t v = 0; v < s.reads.size(); v++) {
        if (s.reads[v] && !Holds(p, static_cast<std::int32_t>(v), w)) {
            return false;
        }
    }
    for (std::uint32_t i = 0; i < e.count; i++) {
        if (!Invariant(p, Item(p, e, i), w)) {
            return false;
        }
    }
    return true;
}

bool Invariant(const Pass& p, ExprId id, const LoopWrites& w) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
        return true;
    case ExprKind::VarRef:
        return IsScalarKind(KindOf(*p.program, e.type)) && Holds(p, e.ref, w);
    case ExprKind::Index:
        return PlaceHolds(p, e.a, w) && Invariant(p, e.b, w);
    case ExprKind::Field:
        return PlaceHolds(p, e.a, w);
    case ExprKind::AddrOf:
        return AddressHolds(p, e.a, w);
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        return Invariant(p, e.a, w);
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
        return Invariant(p, e.a, w) && Invariant(p, e.b, w);
    case ExprKind::If:
        return Invariant(p, e.a, w) && Invariant(p, e.b, w) && Invariant(p, e.c, w);
    case ExprKind::Call:
        return CallHolds(p, e, w);
    default:
        return false; // loads, effects, control
    }
}

} // namespace

bool MayLeave(const Pass& p, ExprId id, int depth) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Return:
        return true;
    case ExprKind::Break:
    case ExprKind::Continue:
        return depth == 0;
    case ExprKind::While:
        return MayLeave(p, e.a, depth) || MayLeave(p, e.b, depth + 1);
    case ExprKind::For:
        return MayLeave(p, e.a, depth) || MayLeave(p, e.b, depth) || MayLeave(p, e.c, depth + 1);
    case ExprKind::Literal:
    case ExprKind::FnRef:
    case ExprKind::VarRef:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Store:
    case ExprKind::StoreField:
    case ExprKind::AddrOf:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::And:
    case ExprKind::Or:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Call:
    case ExprKind::CallPtr:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::If:
    case ExprKind::Do:
    case ExprKind::Match:
    case ExprKind::Let:
    case ExprKind::Set: {
        bool leaves = false;
        EachChild(p, e, [&](ExprId child) { leaves = leaves || MayLeave(p, child, depth); });
        return leaves;
    }
    default:
        return true; // not known here
    }
}

namespace {

struct Finder {
    const Pass* p = nullptr;
    const LoopWrites* w = nullptr;
    std::int64_t trips = -1;
    std::vector<ExprId>* out = nullptr;
};

void Candidates(Finder& f, ExprId id, bool shared, bool sometimes);

// the pointer a load or store goes through: (+ p i) stays whole, since the backend reaches
// it by an addressing mode ((p),Y) that costs no more than the moved pointer would
void PointerCandidates(Finder& f, ExprId id, bool shared, bool sometimes) {
    shared = shared || Shared(*f.p, id);
    const Expr& e = At(*f.p, id);
    if (e.kind == ExprKind::PtrAdd || e.kind == ExprKind::PtrSub) {
        Candidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes);
        return;
    }
    Candidates(f, id, shared, sometimes);
}

void PlaceCandidates(Finder& f, ExprId id, bool shared, bool sometimes) {
    shared = shared || Shared(*f.p, id);
    const Expr& e = At(*f.p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return;
    case ExprKind::Index:
        PlaceCandidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes);
        return;
    case ExprKind::Field:
        PlaceCandidates(f, e.a, shared, sometimes);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        PointerCandidates(f, e.a, shared, sometimes);
        return;
    default:
        Candidates(f, id, shared, sometimes);
        return;
    }
}

// the largest invariant expressions worth moving under id, outermost first; sometimes: id
// may not run when the loop's body does
void Candidates(Finder& f, ExprId id, bool shared, bool sometimes) {
    if (id < 0) {
        return;
    }
    const Pass& p = *f.p;
    shared = shared || Shared(p, id);
    const Expr& e = At(p, id);
    if (!shared && !sometimes && Invariant(p, id, *f.w) && IsPure(p, id) && Worth(p, id, f.trips)) {
        f.out->push_back(id);
        return;
    }
    switch (e.kind) {
    case ExprKind::Index:
        PlaceCandidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes);
        return;
    case ExprKind::Field:
    case ExprKind::AddrOf:
        PlaceCandidates(f, e.a, shared, sometimes);
        return;
    case ExprKind::Set:
        PlaceCandidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes);
        return;
    case ExprKind::And:
    case ExprKind::Or:
        Candidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, true);
        return;
    case ExprKind::If:
        Candidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, true);
        Candidates(f, e.c, shared, true);
        return;
    case ExprKind::Match:
        Candidates(f, e.a, shared, sometimes);
        for (std::uint32_t i = 0; i < e.count; i++) {
            Candidates(f, p.program->clauses[e.first + i].body, shared, true);
        }
        return;
    case ExprKind::While:
        Candidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes || !BodyRuns(p, id));
        return;
    case ExprKind::For:
        Candidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes);
        Candidates(f, e.c, shared, sometimes || !BodyRuns(p, id));
        return;
    case ExprKind::Do:
        // after a statement that may leave the pass, the rest may not run
        for (std::uint32_t i = 0; i < e.count; i++) {
            ExprId item = Item(p, e, i);
            Candidates(f, item, shared, sometimes);
            sometimes = sometimes || MayLeave(p, item, 0);
        }
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        PointerCandidates(f, e.a, shared, sometimes);
        return;
    case ExprKind::Store:
    case ExprKind::StoreField:
        PointerCandidates(f, e.a, shared, sometimes);
        Candidates(f, e.b, shared, sometimes);
        return;
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::Call:
    case ExprKind::CallPtr:
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Interrupts:
    case ExprKind::Jsr:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::Return:
    case ExprKind::Let:
        EachChild(p, e, [&](ExprId child) { Candidates(f, child, shared, sometimes); });
        return;
    default:
        return; // not known here
    }
}

} // namespace

LoopWrites FindLoopWrites(const Pass& p, ExprId loop) {
    LoopWrites w;
    w.vars.assign(p.program->vars.size(), false);
    ScanWrites(p, loop, w);
    return w;
}

void FindInvariants(const Pass& p, ExprId loop, const LoopWrites& w, std::int64_t trips,
                    Invariants& out) {
    const Expr& e = At(p, loop);
    Finder f;
    f.p = &p;
    f.w = &w;
    f.trips = trips;
    if (e.kind == ExprKind::While) {
        f.out = &out.head;
        Candidates(f, e.a, false, false);
    }
    f.out = &out.body;
    Candidates(f, e.kind == ExprKind::While ? e.b : e.c, false, false);
}

} // namespace tisple::opt
