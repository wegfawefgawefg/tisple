// Liveness of one function's tracked variables, backwards over the tree. A variable is live
// at a point when some path from there may read it before writing it whole.
//
// - Tracked: the function's own locals and params, and globals with no address taken, no
//   :address and not in aux. Nothing else can reach them: a pointer only reaches
//   address-taken variables (pointer provenance), a call only what its summary says.
// - Every tracked global is live when the function returns: a caller, or whoever looks at
//   memory after main, may read it.
// - A call reads what its callee's summary reads; I/O, jsr and nodes this file does not know
//   read everything. Nothing but a whole set or a let kills (a partial set, s.x or
//   (at a i), keeps the rest of the variable).
// - Memory may be looked at while the program runs (a debugger, `tisple run --peek`), so a
//   while that may never end, or a call that may not come back, reads every global too.
// - Loops run to a fixed point; a node seen on several passes keeps the union of what was
//   live after it, which only ever errs towards live.

#include "opt/licm.hpp"
#include "opt/opt_internal.hpp"

namespace tisple::opt {

namespace {

struct Loop {
    Bits exit; // live after the loop: where break goes
    Bits head; // live at the next pass: where continue goes
};

struct Walker {
    const Pass* p = nullptr;
    Liveness* out = nullptr;
    Bits all;     // every tracked var
    Bits globals; // the tracked globals
    std::vector<Loop> loops;
    std::vector<Bits> callee_reads; // by function: the tracked globals it reads, when known
    std::vector<bool> have_reads;
};

void Walk(Walker& w, ExprId id, Bits& live);

void Gen(Walker& w, Bits& live, std::int32_t var) {
    std::int32_t bit = w.out->bit[static_cast<std::size_t>(var)];
    if (bit >= 0) {
        SetBit(live, bit);
    }
}

void Record(Walker& w, ExprId id, const Bits& live) {
    auto found = w.out->after.find(id);
    if (found == w.out->after.end()) {
        w.out->after.emplace(id, live);
    } else {
        OrBits(found->second, live);
    }
}

const Bits& CalleeReads(Walker& w, std::int32_t function) {
    std::size_t f = static_cast<std::size_t>(function);
    if (w.have_reads[f]) {
        return w.callee_reads[f];
    }
    const Summary& s = w.p->summaries[f];
    Bits reads(w.all.size(), 0);
    if (s.io || s.jsr || s.unknown || !s.terminates) {
        reads = w.globals;
    } else {
        for (std::size_t v = 0; v < s.reads.size(); v++) {
            if (s.reads[v]) {
                Gen(w, reads, static_cast<std::int32_t>(v));
            }
        }
    }
    w.callee_reads[f] = reads;
    w.have_reads[f] = true;
    return w.callee_reads[f];
}

// the index expressions of a place, worked out before the value (the root is not read)
void WalkPlace(Walker& w, ExprId id, Bits& live) {
    const Expr& e = At(*w.p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return;
    case ExprKind::Index:
        Walk(w, e.b, live);
        WalkPlace(w, e.a, live);
        return;
    case ExprKind::Field:
        WalkPlace(w, e.a, live);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        Walk(w, e.a, live);
        return;
    default:
        Walk(w, id, live);
        return;
    }
}

void WalkItems(Walker& w, const Expr& e, Bits& live) {
    for (std::uint32_t i = e.count; i-- > 0;) {
        Walk(w, Item(*w.p, e, i), live);
    }
}

void WalkWhile(Walker& w, ExprId id, Bits& live) {
    const Expr& e = At(*w.p, id);
    Bits exit = live;
    Record(w, id, exit);
    // while a loop that may never end runs, anyone may look at memory
    bool endless = !WhileEnds(*w.p, id);
    Bits head = exit;
    Walk(w, e.a, head);
    if (endless) {
        OrBits(head, w.globals);
    }
    while (true) {
        w.loops.push_back(Loop{exit, head});
        Bits body = head;
        Walk(w, e.b, body);
        w.loops.pop_back();
        OrBits(body, exit);
        Walk(w, e.a, body);
        OrBits(body, head);
        if (body == head) {
            break;
        }
        head = body;
    }
    live = head;
}

void WalkFor(Walker& w, ExprId id, Bits& live) {
    const Expr& e = At(*w.p, id);
    Bits exit = live;
    Record(w, id, exit);
    std::int32_t bit = w.out->bit[static_cast<std::size_t>(e.ref)];
    Bits head = exit;
    Bits first = exit; // live where the first pass starts
    while (true) {
        w.loops.push_back(Loop{exit, head});
        Bits body = head;
        Walk(w, e.c, body);
        w.loops.pop_back();
        // each pass starts by setting the loop variable
        if (bit >= 0) {
            ClearBit(body, bit);
        }
        first = body;
        OrBits(body, exit);
        OrBits(body, head);
        if (body == head) {
            break;
        }
        head = body;
    }
    // a range that surely makes a pass (slice 6): what the first pass sets before reading is
    // dead before the loop (seed 1278's g2)
    live = BodyRuns(*w.p, id) ? first : head;
    Walk(w, e.b, live);
    Walk(w, e.a, live);
}

void Walk(Walker& w, ExprId id, Bits& live) {
    if (id < 0) {
        return;
    }
    const Expr& e = At(*w.p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::FnRef:
        return;
    case ExprKind::VarRef:
        Gen(w, live, e.ref);
        return;
    case ExprKind::Let: {
        Record(w, id, live);
        std::int32_t bit = w.out->bit[static_cast<std::size_t>(e.ref)];
        if (bit >= 0) {
            ClearBit(live, bit);
        }
        Walk(w, e.a, live);
        return;
    }
    case ExprKind::Set: {
        Record(w, id, live);
        const Expr& place = At(*w.p, e.a);
        if (place.kind == ExprKind::VarRef) {
            std::int32_t bit = w.out->bit[static_cast<std::size_t>(place.ref)];
            if (bit >= 0) {
                ClearBit(live, bit);
            }
        }
        Walk(w, e.b, live);
        WalkPlace(w, e.a, live);
        return;
    }
    case ExprKind::AddrOf:
        WalkPlace(w, e.a, live);
        return;
    case ExprKind::And:
    case ExprKind::Or: {
        Bits maybe = live;
        Walk(w, e.b, maybe);
        OrBits(live, maybe);
        Walk(w, e.a, live);
        return;
    }
    case ExprKind::If: {
        Bits other = live;
        Walk(w, e.b, live);
        Walk(w, e.c, other);
        OrBits(live, other);
        Walk(w, e.a, live);
        return;
    }
    case ExprKind::Match: {
        Bits out = live; // no clause taken
        for (std::uint32_t i = 0; i < e.count; i++) {
            Bits arm = live;
            Walk(w, w.p->program->clauses[e.first + i].body, arm);
            OrBits(out, arm);
        }
        live = out;
        Walk(w, e.a, live);
        return;
    }
    case ExprKind::While:
        WalkWhile(w, id, live);
        return;
    case ExprKind::For:
        WalkFor(w, id, live);
        return;
    case ExprKind::Return:
        live = w.globals;
        Walk(w, e.a, live);
        return;
    case ExprKind::Break:
        live = w.loops.empty() ? w.all : w.loops.back().exit;
        return;
    case ExprKind::Continue:
        live = w.loops.empty() ? w.all : w.loops.back().head;
        return;
    case ExprKind::Call:
        OrBits(live, CalleeReads(w, e.ref));
        WalkItems(w, e, live);
        return;
    case ExprKind::CallPtr: {
        // what any function the pointer may hold reads; the pointer is worked out first
        const std::vector<std::int32_t>& targets = PointerTargets(*w.p, At(*w.p, e.a).type);
        for (std::int32_t target : targets) {
            OrBits(live, CalleeReads(w, target));
        }
        if (targets.empty()) {
            OrBits(live, w.globals);
        }
        WalkItems(w, e, live);
        Walk(w, e.a, live);
        return;
    }
    case ExprKind::StructLit:
    case ExprKind::Do:
        WalkItems(w, e, live);
        return;
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
    case ExprKind::Interrupts:
        OrBits(live, w.globals);
        break;
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Store:
    case ExprKind::StoreField:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::JsrRegs:
        break;
    default:
        // not known here: it may read anything, and nothing inside it is looked at
        live = w.all;
        return;
    }
    // children worked out in order a, b, c
    Walk(w, e.c, live);
    Walk(w, e.b, live);
    Walk(w, e.a, live);
}

} // namespace

bool TrackedVar(const Var& var, std::int32_t function) {
    if (var.address_taken || var.placement.has_address || var.placement.in_aux || var.irq_shared) {
        return false;
    }
    if (var.kind == VarKind::Global) {
        return true;
    }
    return (var.kind == VarKind::Local || var.kind == VarKind::Param) && var.function == function;
}

Liveness ComputeLiveness(const Pass& p, std::int32_t function) {
    const Program& program = *p.program;
    Liveness out;
    out.bit.assign(program.vars.size(), -1);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        if (TrackedVar(program.vars[v], function)) {
            out.bit[v] = out.count++;
        }
    }
    Walker w;
    w.p = &p;
    w.out = &out;
    std::size_t words = static_cast<std::size_t>(out.count + 63) / 64;
    w.all.assign(words, 0);
    w.globals.assign(words, 0);
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        std::int32_t bit = out.bit[v];
        if (bit < 0) {
            continue;
        }
        SetBit(w.all, bit);
        if (program.vars[v].kind == VarKind::Global) {
            SetBit(w.globals, bit);
        }
    }
    w.callee_reads.resize(program.functions.size());
    w.have_reads.assign(program.functions.size(), false);
    // falling off the end returns too
    Bits live = w.globals;
    Walk(w, program.functions[static_cast<std::size_t>(function)].body, live);
    return out;
}

bool LiveAfter(const Liveness& live, ExprId id, std::int32_t var) {
    std::int32_t bit = live.bit[static_cast<std::size_t>(var)];
    if (bit < 0) {
        return true;
    }
    auto found = live.after.find(id);
    return found == live.after.end() || HasBit(found->second, bit);
}

} // namespace tisple::opt
