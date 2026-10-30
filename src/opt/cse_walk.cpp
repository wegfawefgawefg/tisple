// Common subexpressions (docs/midlevel-optimizer.md, pass 6), the search: one walk of a
// function in the order it runs (operands left to right, a set's place before its value),
// keeping the values worked out so far that still hold. A value worked out again while it
// holds is noted as a later occurrence of the first.
//
// - Values: what cse_values.cpp takes (arithmetic and pure calls over plain variables, worth
//   enough cycles to keep). Not array reads: the backend reaches an element again cheaply.
// - A value stops holding when something may change what it reads: a set or let of one of
//   its variables, a call whose summary writes one (or does I/O, jsr, or is not known), a store
//   through a pointer when it reads an address-taken variable, io_read, io_write, jsr, a node
//   not known here. A loop changes, from its start, everything it changes anywhere in it.
// - The tree makes dominance simple: a value first worked out in a do's item (not in an arm,
//   the right side of an and/or, or a loop's condition) can be worked out by a let just
//   before that item, when nothing earlier in the item changes what it reads. It holds for
//   the rest of the do, arms and loops in it included; values first worked out in an arm or a
//   loop's body hold only there. A void arm that is not a do is a place for lets too.
// - Not a value first worked out in a loop that never changes its inputs: code motion moves
//   that out of the loop when it runs on every pass, and the backend hoists byte work from
//   arms too, which a let in the loop would hide.
// - (/ a b) and (% a b) of the same operands share one division (the backend works out both
//   from one when they sit next to each other): the second is a value of its own that knows
//   the first, its partner.

#include "opt/cse.hpp"
#include "opt/licm.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

struct Walker {
    const Pass* p = nullptr;
    CseFound* out = nullptr;
    std::vector<std::int32_t> live; // entries whose value still holds
    std::vector<std::int32_t> open; // blocks around the walk, innermost last; -1: no lets here
    std::vector<CseKill> log;       // what may have changed, in walk order
    std::vector<LoopWrites> loops;  // what the loops around the walk change, innermost last
};

bool ReadsVar(const CseEntry& e, std::int32_t var) {
    return std::find(e.reads.begin(), e.reads.end(), var) != e.reads.end();
}

// may what k changes be what e reads
bool Hits(const CseKill& k, const CseEntry& e) {
    if (k.all || (k.stores && e.pointed)) {
        return true;
    }
    return k.var >= 0 && ReadsVar(e, k.var);
}

void Kill(Walker& w, const CseKill& k) {
    w.log.push_back(k);
    std::vector<std::int32_t> kept;
    for (std::int32_t i : w.live) {
        if (!Hits(k, w.out->entries[static_cast<std::size_t>(i)])) {
            kept.push_back(i);
        }
    }
    w.live = kept;
}

void KillVar(Walker& w, std::int32_t var) {
    CseKill k;
    k.var = var;
    Kill(w, k);
}

void KillStores(Walker& w) {
    CseKill k;
    k.stores = true;
    Kill(w, k);
}

void KillAll(Walker& w) {
    CseKill k;
    k.all = true;
    Kill(w, k);
}

// what a call may change: the globals its summary writes, address-taken variables when it
// stores through pointers, everything when it does I/O or jsr
void KillCall(Walker& w, std::int32_t function) {
    const Summary& s = w.p->summaries[static_cast<std::size_t>(function)];
    if (s.io || s.jsr || s.unknown) {
        KillAll(w);
        return;
    }
    for (std::size_t v = 0; v < s.writes.size(); v++) {
        if (s.writes[v]) {
            KillVar(w, static_cast<std::int32_t>(v));
        }
    }
    if (s.stores) {
        KillStores(w);
    }
}

bool HasIoRead(const Pass& p, ExprId id) {
    if (id < 0) {
        return false;
    }
    const Expr& e = At(p, id);
    bool found = e.kind == ExprKind::IoRead;
    EachChild(p, e, [&](ExprId child) { found = found || HasIoRead(p, child); });
    return found;
}

// may the loop change what e reads
bool LoopHits(const LoopWrites& lw, const CseEntry& e) {
    bool hit = lw.all || (e.pointed && lw.stores);
    for (std::int32_t var : e.reads) {
        hit = hit || lw.vars[static_cast<std::size_t>(var)];
    }
    return hit;
}

// a loop changes, from its start, what it changes anywhere in it (its body logs those as the
// walk goes through it); the walk inside it knows what it changes
void EnterLoop(Walker& w, ExprId loop) {
    LoopWrites lw = FindLoopWrites(*w.p, loop);
    lw.all = lw.all || HasIoRead(*w.p, loop);
    if (lw.all) {
        KillAll(w);
    }
    std::vector<std::int32_t> kept;
    for (std::int32_t i : w.live) {
        if (!LoopHits(lw, w.out->entries[static_cast<std::size_t>(i)])) {
            kept.push_back(i);
        }
    }
    w.live = kept;
    w.loops.push_back(lw);
}

CseAt Here(const Walker& w) {
    CseAt at;
    if (!w.open.empty() && w.open.back() >= 0) {
        at.block = w.open.back();
        at.stmt = w.out->blocks[static_cast<std::size_t>(at.block)].stmt;
    }
    return at;
}

// a let of e may go before the item the walk is in: nothing earlier in it changed e's inputs
bool CanStart(const Walker& w, const CseEntry& e) {
    if (e.at.block < 0) {
        return false;
    }
    const CseBlock& b = w.out->blocks[static_cast<std::size_t>(e.at.block)];
    for (std::size_t i = b.log_start; i < w.log.size(); i++) {
        if (Hits(w.log[i], e)) {
            return false;
        }
    }
    return true;
}

void Walk(Walker& w, ExprId id, bool shared);

// a value worth keeping: the same as one that holds, or a new one (its parts first, so a let
// of a part comes before a let of the whole), maybe the partner of a division that holds.
// False: not one
bool Value(Walker& w, ExprId id) {
    const Pass& p = *w.p;
    CseEntry entry;
    entry.first = id;
    if (!CseValue(p, id, entry)) {
        return false;
    }
    std::vector<CseEntry>& entries = w.out->entries;
    for (std::int32_t i : w.live) {
        if (SameValue(p, entries[static_cast<std::size_t>(i)].first, id)) {
            entries[static_cast<std::size_t>(i)].later.push_back(id);
            return true;
        }
    }
    for (std::int32_t i : w.live) {
        if (entry.partner < 0 &&
            DivisionPartners(p, entries[static_cast<std::size_t>(i)].first, id)) {
            entry.partner = i;
        }
    }
    Expr e = At(p, id);
    entry.parts_from = entries.size();
    EachChild(p, e, [&](ExprId child) { Walk(w, child, false); });
    entry.at = Here(w);
    // work the loop around never changes the inputs of is code motion's to move out of it, or
    // the backend's (a let in the loop would hide that it holds); it may still join its
    // partner's let
    bool invariant = !w.loops.empty() && !LoopHits(w.loops.back(), entry);
    entry.own = !invariant && CanStart(w, entry);
    if (entry.own || entry.partner >= 0) {
        w.live.push_back(static_cast<std::int32_t>(entries.size()));
        entries.push_back(entry);
    }
    return true;
}

// values made from mark on (in a block that ends) hold no more
void EndBlock(Walker& w, std::size_t mark) {
    std::vector<std::int32_t> kept;
    for (std::int32_t i : w.live) {
        if (static_cast<std::size_t>(i) < mark) {
            kept.push_back(i);
        }
    }
    w.live = kept;
}

// what holds after every one of outs, of what held before
std::vector<std::int32_t> HoldsAfterAll(const std::vector<std::int32_t>& before,
                                        const std::vector<std::vector<std::int32_t>>& outs) {
    std::vector<std::int32_t> kept;
    for (std::int32_t i : before) {
        bool everywhere = true;
        for (const std::vector<std::int32_t>& out : outs) {
            everywhere = everywhere && std::find(out.begin(), out.end(), i) != out.end();
        }
        if (everywhere) {
            kept.push_back(i);
        }
    }
    return kept;
}

void WalkDo(Walker& w, ExprId id, bool shared) {
    std::int32_t b = -1;
    if (!shared) {
        b = static_cast<std::int32_t>(w.out->blocks.size());
        CseBlock block;
        block.id = id;
        w.out->blocks.push_back(block);
    }
    std::size_t mark = w.out->entries.size();
    w.open.push_back(b);
    Expr e = At(*w.p, id);
    for (std::uint32_t i = 0; i < e.count; i++) {
        if (b >= 0) {
            w.out->blocks[static_cast<std::size_t>(b)].stmt = i;
            w.out->blocks[static_cast<std::size_t>(b)].log_start = w.log.size();
        }
        Walk(w, Item(*w.p, e, i), shared);
    }
    w.open.pop_back();
    EndBlock(w, mark);
}

// code that may run or not (an arm, the right side of an and/or, a loop's condition): lets
// may only go in dos inside it, or in it when it is a void arm (slot)
void WalkMaybe(Walker& w, ExprId id, bool shared, ExprId parent, std::uint32_t which, bool slot) {
    const Pass& p = *w.p;
    shared = shared || Shared(p, id);
    if (At(p, id).kind == ExprKind::Do) {
        Walk(w, id, shared);
        return;
    }
    std::int32_t b = -1;
    if (slot && !shared) {
        b = static_cast<std::int32_t>(w.out->blocks.size());
        CseBlock block;
        block.id = id;
        block.slot = true;
        block.parent = parent;
        block.which = which;
        block.log_start = w.log.size();
        w.out->blocks.push_back(block);
    }
    std::size_t mark = w.out->entries.size();
    w.open.push_back(b);
    Walk(w, id, shared);
    w.open.pop_back();
    EndBlock(w, mark);
}

void WalkIf(Walker& w, ExprId id, bool shared) {
    Expr e = At(*w.p, id);
    Walk(w, e.a, shared);
    bool slot = e.type == kVoidType;
    std::vector<std::int32_t> before = w.live;
    std::vector<std::vector<std::int32_t>> outs;
    for (std::uint32_t arm = 0; arm < 2; arm++) {
        ExprId body = arm == 0 ? e.b : e.c;
        w.live = before;
        if (body >= 0) {
            WalkMaybe(w, body, shared, id, arm, slot);
        }
        outs.push_back(w.live);
    }
    w.live = HoldsAfterAll(before, outs);
}

void WalkMatch(Walker& w, ExprId id, bool shared) {
    Expr e = At(*w.p, id);
    Walk(w, e.a, shared);
    bool slot = e.type == kVoidType;
    std::vector<std::int32_t> before = w.live;
    std::vector<std::vector<std::int32_t>> outs;
    for (std::uint32_t i = 0; i < e.count; i++) {
        w.live = before;
        ExprId body = w.p->program->clauses[e.first + i].body;
        WalkMaybe(w, body, shared, id, 2 + i, slot);
        outs.push_back(w.live);
    }
    w.live = HoldsAfterAll(before, outs);
}

void WalkShortCircuit(Walker& w, const Expr& e, bool shared) {
    Walk(w, e.a, shared);
    std::vector<std::int32_t> before = w.live;
    WalkMaybe(w, e.b, shared, -1, 0, false);
    w.live = HoldsAfterAll(before, {w.live});
}

// the index expressions of a place being written (its root is not read)
void WalkPlace(Walker& w, ExprId id, bool shared) {
    shared = shared || Shared(*w.p, id);
    Expr e = At(*w.p, id);
    switch (e.kind) {
    case ExprKind::VarRef:
        return;
    case ExprKind::Index:
        WalkPlace(w, e.a, shared);
        Walk(w, e.b, shared);
        return;
    case ExprKind::Field:
        WalkPlace(w, e.a, shared);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
        Walk(w, e.a, shared);
        return;
    default:
        Walk(w, id, shared);
        return;
    }
}

void WalkChildren(Walker& w, const Expr& e, bool shared) {
    EachChild(*w.p, e, [&](ExprId child) { Walk(w, child, shared); });
}

void Walk(Walker& w, ExprId id, bool shared) {
    if (id < 0) {
        return;
    }
    const Pass& p = *w.p;
    shared = shared || Shared(p, id);
    if (!shared && Value(w, id)) {
        return;
    }
    Expr e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
    case ExprKind::FnRef:
    case ExprKind::Break:
    case ExprKind::Continue:
        return;
    case ExprKind::Do:
        WalkDo(w, id, shared);
        return;
    case ExprKind::If:
        WalkIf(w, id, shared);
        return;
    case ExprKind::Match:
        WalkMatch(w, id, shared);
        return;
    case ExprKind::And:
    case ExprKind::Or:
        WalkShortCircuit(w, e, shared);
        return;
    case ExprKind::While:
        EnterLoop(w, id);
        WalkMaybe(w, e.a, shared, -1, 0, false);
        WalkMaybe(w, e.b, shared, -1, 0, false);
        w.loops.pop_back();
        return;
    case ExprKind::For:
        Walk(w, e.a, shared);
        Walk(w, e.b, shared);
        KillVar(w, e.ref);
        EnterLoop(w, id);
        WalkMaybe(w, e.c, shared, -1, 0, false);
        w.loops.pop_back();
        return;
    case ExprKind::Let:
        Walk(w, e.a, shared);
        KillVar(w, e.ref);
        return;
    case ExprKind::Set: {
        WalkPlace(w, e.a, shared);
        Walk(w, e.b, shared);
        std::int32_t root = PlaceRoot(p, e.a);
        if (root >= 0) {
            KillVar(w, root);
        } else {
            KillStores(w);
        }
        return;
    }
    case ExprKind::Index:
        WalkPlace(w, e.a, shared);
        Walk(w, e.b, shared);
        return;
    case ExprKind::Field:
    case ExprKind::AddrOf:
        WalkPlace(w, e.a, shared);
        return;
    case ExprKind::Store:
    case ExprKind::StoreField:
        WalkChildren(w, e, shared);
        KillStores(w);
        return;
    case ExprKind::Call:
        WalkChildren(w, e, shared);
        KillCall(w, e.ref);
        return;
    case ExprKind::CallPtr:
        // the pointer and the arguments, then whatever the function it holds may change
        WalkChildren(w, e, shared);
        KillAll(w);
        return;
    case ExprKind::IoRead:
    case ExprKind::IoWrite:
    case ExprKind::Jsr:
        WalkChildren(w, e, shared);
        KillAll(w);
        return;
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Unary:
    case ExprKind::Binary:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
    case ExprKind::JsrRegs:
    case ExprKind::StructLit:
    case ExprKind::Return:
        WalkChildren(w, e, shared);
        return;
    default:
        KillAll(w); // not known here: it may change anything
        return;
    }
}

} // namespace

void FindCommon(const Pass& p, std::int32_t function, CseFound& out) {
    out = CseFound();
    ExprId body = p.program->functions[static_cast<std::size_t>(function)].body;
    if (body < 0) {
        return;
    }
    Walker w;
    w.p = &p;
    w.out = &out;
    Walk(w, body, false);
}

} // namespace tisple::opt
