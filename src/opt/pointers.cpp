// Pointers to known elements (docs/midlevel-optimizer.md, slice 6): a pointer local one let
// sets to the address of an element of a global array at a literal index, never set again nor
// its address taken, names that element; a load or store through it, or through it plus or
// minus a literal count, inside the array, is a read or set of the element there:
//   (let q (ptr u8) (addr_of (at wb 3))) ... (store (+ q 254) 0) ... (load q)
//   -> ... (set (at wb 257) 0) ... (at wb 3)
// so the other passes follow it (collapse's cells, dead stores, constants: seed 1812 73 to 21
// cycles, 1287 679 to 247). A let of such a pointer, (let r (ptr u8) q), is one too. The
// element type must be the pointer's. The pointer's let stays for what else reads it; dead
// code takes it once nothing does. Not for counts worked out at run time: a u16 index costs the
// backend an address worked out per access where the pointer is set once (seed 583 69439 to
// 138860 cycles), and (+ k (as u16 (+ x c))) as an index loses k on main at -O0 (a backend bug,
// reported with slice 6).

#include "opt/collapse.hpp"

#include <unordered_map>

namespace tisple::opt {

namespace {

// the element a pointer local names
struct Element {
    std::int32_t array = -1;
    std::uint32_t index = 0;
};

struct Pointers {
    Pass* p = nullptr;
    std::int32_t function = -1;
    std::unordered_map<std::int32_t, Element> named; // by pointer local
    std::unordered_map<std::int32_t, int> writes;    // by local: its lets and sets
    std::uint64_t changes = 0;
};

// a pointer local's let: (addr_of (at A k)), k a literal, of a global array of the pointer's
// element type, or a read of a pointer local that names one
void NoteLet(Pointers& ps, const Expr& e) {
    const Pass& p = *ps.p;
    const Var& v = VarOf(p, e.ref);
    if (v.kind != VarKind::Local || v.address_taken ||
        KindOf(*p.program, v.type) != TypeKind::Ptr || ps.writes[e.ref] != 1) {
        return;
    }
    const Expr& value = At(p, e.a);
    if (value.kind == ExprKind::VarRef) {
        auto from = ps.named.find(value.ref);
        if (from != ps.named.end() && VarOf(p, value.ref).type == v.type) {
            ps.named[e.ref] = from->second;
        }
        return;
    }
    if (value.kind != ExprKind::AddrOf || At(p, value.a).kind != ExprKind::Index) {
        return;
    }
    const Expr& place = At(p, value.a);
    const Expr& root = At(p, place.a);
    std::uint16_t k = 0;
    if (root.kind != ExprKind::VarRef || VarOf(p, root.ref).kind != VarKind::Global ||
        Hardware(VarOf(p, root.ref)) || place.type != TypeOf(*p.program, v.type).elem ||
        !ConstValue(*p.program, place.b, k)) {
        return;
    }
    ps.named[e.ref] = Element{root.ref, k};
}

// the lets and sets of each local under id
void CountWrites(Pointers& ps, ExprId id) {
    const Expr& e = At(*ps.p, id);
    if (e.kind == ExprKind::Let || e.kind == ExprKind::For) {
        ps.writes[e.ref]++;
    }
    if (e.kind == ExprKind::Set) {
        ps.writes[PlaceRoot(*ps.p, e.a)]++;
    }
    EachChild(*ps.p, e, [&](ExprId child) { CountWrites(ps, child); });
}

// lets come before their reads, in the order a walk meets them
void FindLets(Pointers& ps, ExprId id) {
    const Expr& e = At(*ps.p, id);
    if (e.kind == ExprKind::Let) {
        NoteLet(ps, e);
    }
    EachChild(*ps.p, e, [&](ExprId child) { FindLets(ps, child); });
}

// the element an access's pointer reaches: q, (p+ q n) or (p- q n), n a literal, inside the
// array; false for none
bool Reaches(const Pointers& ps, ExprId ptr, Element& out) {
    const Pass& p = *ps.p;
    const Expr& e = At(p, ptr);
    std::int32_t var = -1;
    std::int64_t step = 0;
    if (e.kind == ExprKind::VarRef) {
        var = e.ref;
    } else if ((e.kind == ExprKind::PtrAdd || e.kind == ExprKind::PtrSub) &&
               At(p, e.a).kind == ExprKind::VarRef) {
        std::uint16_t n = 0;
        if (!ConstValue(*p.program, e.b, n)) {
            return false;
        }
        var = At(p, e.a).ref;
        step = e.kind == ExprKind::PtrAdd ? n : -static_cast<std::int64_t>(n);
    }
    auto found = ps.named.find(var);
    if (var < 0 || found == ps.named.end()) {
        return false;
    }
    std::int64_t at = static_cast<std::int64_t>(found->second.index) + step;
    const Type& array = TypeOf(*p.program, VarOf(p, found->second.array).type);
    if (at < 0 || at >= static_cast<std::int64_t>(array.count)) {
        return false;
    }
    out.array = found->second.array;
    out.index = static_cast<std::uint32_t>(at);
    return true;
}

ExprId ElementPlace(Pass& p, const Element& el, const Loc& loc) {
    const Type& array = TypeOf(*p.program, VarOf(p, el.array).type);
    Expr root;
    root.kind = ExprKind::VarRef;
    root.type = VarOf(p, el.array).type;
    root.ref = el.array;
    root.loc = loc;
    Expr index;
    index.kind = ExprKind::Literal;
    index.type = array.count <= 256 ? kU8Type : kU16Type; // as the checker types it
    index.value = el.index;
    index.loc = loc;
    Expr place;
    place.kind = ExprKind::Index;
    place.type = array.elem;
    place.a = NewExpr(p, root);
    place.b = NewExpr(p, index);
    place.loc = loc;
    return NewExpr(p, place);
}

// loads and stores through the pointers that name an element become its reads and sets
void Rewrite(Pointers& ps, ExprId id, bool shared) {
    Pass& p = *ps.p;
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    Element el;
    bool access = e.kind == ExprKind::Load || e.kind == ExprKind::Store;
    if (access && !shared && Reaches(ps, e.a, el) &&
        Allow(p, e.kind == ExprKind::Load ? "load of a known element" : "store to a known element",
              id, el.array)) {
        ExprId made = ElementPlace(p, el, e.loc);
        if (e.kind == ExprKind::Store) {
            Expr set;
            set.kind = ExprKind::Set;
            set.type = kVoidType;
            set.a = made;
            set.b = e.b;
            set.loc = e.loc;
            made = NewExpr(p, set);
        }
        Retain(p, made);
        ReplaceWith(p, id, made);
        Release(p, made);
        ps.changes++;
        e = At(p, id);
    }
    EachChild(p, e, [&](ExprId child) { Rewrite(ps, child, shared); });
}

} // namespace

std::uint64_t PointersToElements(Pass& p) {
    Pointers ps;
    ps.p = &p;
    for (std::size_t f = 0; f < p.program->functions.size(); f++) {
        ExprId body = p.program->functions[f].body;
        if (body < 0) {
            continue;
        }
        ps.function = static_cast<std::int32_t>(f);
        ps.named.clear();
        ps.writes.clear();
        CountWrites(ps, body);
        FindLets(ps, body);
        if (!ps.named.empty()) {
            Rewrite(ps, body, false);
        }
    }
    return ps.changes;
}

} // namespace tisple::opt
