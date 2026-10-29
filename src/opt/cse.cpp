// Common subexpressions (docs/midlevel-optimizer.md, pass 6): a value worked out again while
// its first working out still holds (cse_walk.cpp) is worked out once, by a let of a new local
// just before the item the first sits in, and read from the local at every place:
//
//   (set g (+ (* x y) 1)) (set h (- (* x y) 2))
//   -> (let t (* x y)) (set g (+ t 1)) (set h (- t 2))
//
// - Only when it saves cycles: k later readings save k times the value's estimate (licm_cost.cpp)
//   less the let's store and k + 1 reads.
// - (/ a b) and (% a b) of the same operands: the second's let goes right after the first's,
//   so the backend works both out from one division. Not when only straight code (no branch,
//   loop, call or routine) sits between them in one do: the backend shares that division as
//   it is, and the second is a value like any other.
// - Never in nodes more than one parent names (constant folding leaves some).

#include "opt/cse.hpp"

#include "opt/licm.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

// cycles keeping a value must save
constexpr std::int32_t kMinSaving = 12;

struct Insert {
    std::uint32_t stmt = 0;
    ExprId let = -1;
};

std::int32_t Word(const Pass& p, ExprId id) {
    return TypeOf(*p.program, At(p, id).type).size > 1 ? 2 : 1;
}

// k readings of a local in place of k more workings out of value
bool Saves(const Pass& p, ExprId value, std::size_t k) {
    if (k == 0) {
        return false;
    }
    std::int32_t n = static_cast<std::int32_t>(k);
    std::int32_t cost = EstimatedCycles(p, value);
    return n * cost - 3 * Word(p, value) * (n + 2) >= kMinSaving;
}

// code the backend keeps a division's results across: sets and lets of plain arithmetic, with
// no branch, loop, call or routine (each leaves a label or a jsr between)
bool Straight(const Pass& p, ExprId id) {
    if (id < 0) {
        return true;
    }
    const Expr& e = At(p, id);
    switch (e.kind) {
    case ExprKind::Literal:
    case ExprKind::VarRef:
        return true;
    case ExprKind::Binary: {
        std::uint16_t c = 0;
        bool routine = e.op == Op::Mul || e.op == Op::Div || e.op == Op::Mod;
        bool shift = e.op == Op::Shl || e.op == Op::Shr;
        bool compare = KindOf(*p.program, e.type) == TypeKind::Bool;
        if (routine || compare || (shift && !ConstValue(*p.program, e.b, c))) {
            return false;
        }
        break;
    }
    case ExprKind::Do:
        for (std::uint32_t i = 0; i < e.count; i++) {
            if (!Straight(p, Item(p, e, i))) {
                return false;
            }
        }
        return true;
    case ExprKind::Set:
    case ExprKind::Let:
    case ExprKind::Index:
    case ExprKind::Field:
    case ExprKind::Load:
    case ExprKind::LoadField:
    case ExprKind::Store:
    case ExprKind::StoreField:
    case ExprKind::AddrOf:
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub:
    case ExprKind::Unary:
    case ExprKind::Convert:
    case ExprKind::Reinterpret:
        break;
    default:
        return false;
    }
    return Straight(p, e.a) && Straight(p, e.b);
}

// a partner the backend already works out from the same division: in the same do as the
// first, with only straight items between
bool SharedAlready(const Pass& p, const CseFound& found, const CseEntry& e, const CseAt& at) {
    if (at.block < 0 || at.block != e.at.block) {
        return false;
    }
    const CseBlock& b = found.blocks[static_cast<std::size_t>(at.block)];
    if (b.slot) {
        return true; // one item
    }
    const Expr& d = At(p, b.id);
    for (std::uint32_t i = e.at.stmt + 1; i < at.stmt; i++) {
        if (!Straight(p, Item(p, d, i))) {
            return false;
        }
    }
    return true;
}

struct Rewriter {
    Pass* p = nullptr;
    std::int32_t function = -1;
    const CseFound* found = nullptr;
    std::vector<std::vector<Insert>> inserts; // by block
    std::vector<bool> joins;                  // by entry: its let goes next to its partner's
    std::vector<bool> joined;                 // by entry: a partner's let goes next to its own
    std::vector<ExprId> lets;                 // by entry: its let, or -1
    std::uint64_t changes = 0;
};

// which divisions' lets go next to their partners': where the backend does not share the
// division already, and working out the second (and its later readings) from the first's
// division saves enough
void PairDivisions(Rewriter& r) {
    const Pass& p = *r.p;
    const std::vector<CseEntry>& entries = r.found->entries;
    r.joins.assign(entries.size(), false);
    r.joined.assign(entries.size(), false);
    for (std::size_t i = 0; i < entries.size(); i++) {
        const CseEntry& e = entries[i];
        if (e.partner < 0) {
            continue;
        }
        const CseEntry& first = entries[static_cast<std::size_t>(e.partner)];
        if (SharedAlready(p, *r.found, first, e.at) || !Saves(p, e.first, e.later.size() + 1)) {
            continue;
        }
        r.joins[i] = true;
        r.joined[static_cast<std::size_t>(e.partner)] = true;
    }
}

// a let of e's value goes before the item at (after the let `after` when it is one), and e's
// first and later workings out read it
void MakeLet(Rewriter& r, std::size_t entry, const CseAt& at, ExprId after) {
    Pass& p = *r.p;
    const CseEntry& e = r.found->entries[entry];
    std::int32_t var = -1;
    ExprId let = MoveToLet(p, r.function, e.first, At(p, e.first).loc, var);
    std::vector<Insert>& lets = r.inserts[static_cast<std::size_t>(at.block)];
    auto pos = lets.end();
    for (auto it = lets.begin(); it != lets.end(); ++it) {
        if (it->let == after) {
            pos = it + 1;
        }
    }
    lets.insert(pos, Insert{at.stmt, let});
    for (ExprId later : e.later) {
        BecomeRead(p, later, var);
    }
    r.lets[entry] = let;
    r.changes++;
}

// a part of e's value has a let of its own, before e's item: e's let cannot go earlier
bool PartsKept(const Rewriter& r, std::size_t i) {
    for (std::size_t k = r.found->entries[i].parts_from; k < i; k++) {
        if (r.lets[k] >= 0) {
            return true;
        }
    }
    return false;
}

void Rewrite(Rewriter& r, std::size_t i) {
    Pass& p = *r.p;
    const CseEntry& e = r.found->entries[i];
    if (r.joins[i] && r.lets[static_cast<std::size_t>(e.partner)] >= 0 && !PartsKept(r, i) &&
        Allow(p, "division pair", e.first, -1)) {
        const CseEntry& first = r.found->entries[static_cast<std::size_t>(e.partner)];
        MakeLet(r, i, first.at, r.lets[static_cast<std::size_t>(e.partner)]);
        return;
    }
    bool worth = r.joined[i] || Saves(p, e.first, e.later.size());
    if (e.own && worth && Allow(p, "common subexpression", e.first, -1)) {
        MakeLet(r, i, e.at, -1);
    }
}

// the lets before their items of a do
void InsertInDo(Rewriter& r, ExprId id, const std::vector<Insert>& lets) {
    Pass& p = *r.p;
    Expr d = At(p, id);
    std::vector<ExprId> items;
    for (std::uint32_t i = 0; i < d.count; i++) {
        for (const Insert& ins : lets) {
            if (ins.stmt == i) {
                Retain(p, ins.let);
                items.push_back(ins.let);
            }
        }
        items.push_back(Item(p, d, i));
    }
    Expr& out = p.program->exprs[static_cast<std::size_t>(id)];
    out.first = static_cast<std::uint32_t>(p.program->lists.size());
    out.count = static_cast<std::uint32_t>(items.size());
    p.program->lists.insert(p.program->lists.end(), items.begin(), items.end());
}

// a void arm that is not a do: a do of the lets and the arm stands in its place
void InsertInSlot(Rewriter& r, const CseBlock& b, const std::vector<Insert>& lets) {
    Pass& p = *r.p;
    std::vector<ExprId> items;
    for (const Insert& ins : lets) {
        items.push_back(ins.let);
    }
    items.push_back(b.id);
    ExprId wrap = NewList(p, ExprKind::Do, At(p, b.id).type, items, At(p, b.id).loc);
    Retain(p, wrap);
    Release(p, b.id); // the do names it now
    Expr& parent = p.program->exprs[static_cast<std::size_t>(b.parent)];
    if (parent.kind == ExprKind::If) {
        (b.which == 0 ? parent.b : parent.c) = wrap;
    } else {
        p.program->clauses[parent.first + b.which - 2].body = wrap;
    }
}

} // namespace

std::uint64_t CommonSubexpressions(Pass& p, std::int32_t function) {
    CseFound found;
    FindCommon(p, function, found);
    Rewriter r;
    r.p = &p;
    r.function = function;
    r.found = &found;
    r.inserts.resize(found.blocks.size());
    r.lets.assign(found.entries.size(), -1);
    PairDivisions(r);
    // parts before wholes: a let of a part comes before the let of a value made from it
    for (std::size_t i = 0; i < found.entries.size(); i++) {
        Rewrite(r, i);
    }
    for (std::size_t b = 0; b < found.blocks.size(); b++) {
        const std::vector<Insert>& lets = r.inserts[b];
        if (lets.empty()) {
            continue;
        }
        if (found.blocks[b].slot) {
            InsertInSlot(r, found.blocks[b], lets);
        } else {
            InsertInDo(r, found.blocks[b].id, lets);
        }
    }
    return r.changes;
}

} // namespace tisple::opt
