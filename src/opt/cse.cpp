// Common subexpressions (docs/midlevel-optimizer.md, pass 6): a value worked out again while
// its first working out still holds (cse_walk.cpp) is worked out once, by a let of a new local
// just before the item the first sits in, and read from the local at every place:
//
//   (set g (+ (* x y) 1)) (set h (- (* x y) 2))
//   -> (let t (* x y)) (set g (+ t 1)) (set h (- t 2))
//
// - Only when it saves cycles: k later readings save k times the value's estimate (licm_cost.cpp)
//   less the let's store and k + 1 reads.
// - (/ a b) and (% a b) of the same operands: both lets go together, so the backend works
//   both out from one division. Not when they already sit in the same or the next item of one
//   do: the backend shares that division as it is.
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

// a partner the backend already works out from the same division: in the item of the first,
// or the next one, of the same do
bool SharedAlready(const CseEntry& e, const CseAt& at) {
    return at.block == e.at.block && at.stmt <= e.at.stmt + 1;
}

struct Rewriter {
    Pass* p = nullptr;
    std::int32_t function = -1;
    std::vector<std::vector<Insert>> inserts; // by block
    std::uint64_t changes = 0;
};

// the partners worth a let of their own, and whether they are
std::vector<ExprId> UsefulPartners(const Pass& p, const CseEntry& e) {
    std::vector<ExprId> out;
    for (std::size_t i = 0; i < e.partners.size(); i++) {
        if (!SharedAlready(e, e.partners_at[i])) {
            out.push_back(e.partners[i]);
        }
    }
    // the first partner is worked out once (from the division the first already does), the
    // rest are read
    if (!out.empty() && !Saves(p, out[0], out.size())) {
        out.clear();
    }
    return out;
}

void Rewrite(Rewriter& r, const CseEntry& e) {
    Pass& p = *r.p;
    bool later = Saves(p, e.first, e.later.size());
    std::vector<ExprId> partners = UsefulPartners(p, e);
    if (!later && partners.empty()) {
        return;
    }
    if (!Allow(p, later ? "common subexpression" : "division pair", e.first, -1)) {
        return;
    }
    Loc loc = At(p, e.first).loc;
    std::vector<Insert>& at = r.inserts[static_cast<std::size_t>(e.at.block)];
    std::int32_t var = -1;
    at.push_back(Insert{e.at.stmt, MoveToLet(p, r.function, e.first, loc, var)});
    r.changes++;
    for (std::size_t i = 0; later && i < e.later.size(); i++) {
        BecomeRead(p, e.later[i], var);
    }
    if (partners.empty() || (later && !Allow(p, "division pair", partners[0], -1))) {
        return;
    }
    std::int32_t rest = -1;
    at.push_back(Insert{e.at.stmt, MoveToLet(p, r.function, partners[0], loc, rest)});
    for (std::size_t i = 1; i < partners.size(); i++) {
        BecomeRead(p, partners[i], rest);
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
    r.inserts.resize(found.blocks.size());
    // parts before wholes: a let of a part comes before the let of a value made from it
    for (const CseEntry& e : found.entries) {
        Rewrite(r, e);
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
