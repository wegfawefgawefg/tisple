// Pointers to known places. A global pointer nothing sets, whose start value is the address of a
// variable (or an element of an array) of its element type, is that address wherever it is
// read: (let ptr_glob (ptr Rec) (addr_of glob_rec)) ... (load ptr_glob int_comp) is
// (load (addr_of glob_rec) int_comp), which the backend reads directly (Dhrystone). So is a
// pointer local one let sets to the address of a variable of its element type (an inlined
// call's pointer parameter).
//
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
// Not where a with_aux may have the switch on (check/aux_mode.hpp): with RAMRD on a load
// through the pointer reaches aux memory, with RAMWRT on a store does, not the element.

#include "check/aux_mode.hpp"
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
    std::unordered_map<std::int32_t, Element> named;      // by pointer local
    std::unordered_map<std::int32_t, int> writes;         // by local: its lets and sets
    std::unordered_map<std::int32_t, std::int32_t> whole; // by pointer local: the var it names
    std::uint64_t changes = 0;
    AuxModes aux; // where the aux memory switches may be on
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
    // (let q (ptr t) (addr_of x)), x a variable of type t: q is that address everywhere
    if (value.kind == ExprKind::AddrOf && At(p, value.a).kind == ExprKind::VarRef &&
        At(p, value.a).type == TypeOf(*p.program, v.type).elem &&
        !Hardware(VarOf(p, At(p, value.a).ref))) {
        ps.whole[e.ref] = At(p, value.a).ref;
        return;
    }
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
    // with RAMRD on a load reaches aux memory, with RAMWRT on a store does
    std::uint8_t aux = AuxStateAt(ps.aux, id);
    bool access = (e.kind == ExprKind::Load && (aux & kAuxReads) == 0) ||
                  (e.kind == ExprKind::Store && (aux & kAuxWrites) == 0);
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

// a global pointer nothing ever sets, whose start value is the address of a variable of its
// element type, or of an element of an array of them: the address of that place
struct FixedPointer {
    std::int32_t target = -1;
    bool element = false;
    std::uint32_t index = 0;
};

void CountGlobalSets(const Pass& p, ExprId id, std::vector<bool>& set) {
    const Expr& e = At(p, id);
    if (e.kind == ExprKind::Set) {
        std::int32_t root = PlaceRoot(p, e.a);
        if (root >= 0) {
            set[static_cast<std::size_t>(root)] = true;
        }
    }
    EachChild(p, e, [&](ExprId child) { CountGlobalSets(p, child, set); });
}

// by var: where each fixed pointer points (target -1 for the rest). Only with a main (with none,
// any global may be written between calls from outside) and no machine code
std::vector<FixedPointer> FindFixedPointers(const Pass& p) {
    const Program& program = *p.program;
    std::vector<FixedPointer> out(program.vars.size());
    if (program.main < 0) {
        return out;
    }
    for (const Summary& s : p.summaries) {
        if (s.jsr || s.unknown) {
            return out;
        }
    }
    std::vector<bool> set(program.vars.size(), false);
    for (const Function& fn : program.functions) {
        if (fn.body >= 0) {
            CountGlobalSets(p, fn.body, set);
        }
    }
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (var.kind != VarKind::Global || KindOf(program, var.type) != TypeKind::Ptr || set[v] ||
            var.address_taken || Hardware(var) || var.addr_slots.size() != 1 ||
            var.addr_slots[0].offset != 0 || var.addr_slots[0].var < 0) {
            continue;
        }
        const AddrSlot& slot = var.addr_slots[0];
        TypeId elem = TypeOf(program, var.type).elem;
        const Var& target = program.vars[static_cast<std::size_t>(slot.var)];
        const Type& t = TypeOf(program, target.type);
        if (target.type == elem && slot.at == 0) {
            out[v].target = slot.var;
            continue;
        }
        std::uint32_t size = TypeOf(program, elem).size;
        if (t.kind == TypeKind::Array && t.elem == elem && size > 0 && slot.at % size == 0 &&
            slot.at / size < t.count) {
            out[v] = FixedPointer{slot.var, true, slot.at / size};
        }
    }
    return out;
}

// reads of the fixed pointers become (addr_of x) or (addr_of (at a k)), which the backend
// reads and writes directly
void RewriteFixed(Pass& p, const std::vector<FixedPointer>& fixed, ExprId id, bool shared,
                  std::uint64_t& changes) {
    shared = shared || Shared(p, id);
    Expr e = At(p, id);
    if (e.kind == ExprKind::VarRef && !shared &&
        fixed[static_cast<std::size_t>(e.ref)].target >= 0 &&
        Allow(p, "read of a pointer that never changes", id, e.ref)) {
        const FixedPointer& f = fixed[static_cast<std::size_t>(e.ref)];
        ExprId place = -1;
        if (f.element) {
            place = ElementPlace(p, Element{f.target, f.index}, e.loc);
        } else {
            Expr root;
            root.kind = ExprKind::VarRef;
            root.type = VarOf(p, f.target).type;
            root.ref = f.target;
            root.loc = e.loc;
            place = NewExpr(p, root);
        }
        Expr addr;
        addr.kind = ExprKind::AddrOf;
        addr.type = e.type;
        addr.a = place;
        addr.loc = e.loc;
        ExprId made = NewExpr(p, addr);
        Retain(p, made);
        ReplaceWith(p, id, made);
        Release(p, made);
        changes++;
        return;
    }
    EachChild(p, e, [&](ExprId child) { RewriteFixed(p, fixed, child, shared, changes); });
}

} // namespace

std::uint64_t PointersToElements(Pass& p) {
    Pointers ps;
    ps.p = &p;
    ps.aux = FindAuxModes(*p.program);
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
    // the fixed pointers: the globals, then each function's locals too
    std::vector<FixedPointer> fixed = FindFixedPointers(p);
    for (std::size_t f = 0; f < p.program->functions.size(); f++) {
        ExprId body = p.program->functions[f].body;
        if (body < 0) {
            continue;
        }
        ps.named.clear();
        ps.writes.clear();
        ps.whole.clear();
        CountWrites(ps, body);
        FindLets(ps, body);
        std::vector<FixedPointer> here = fixed;
        bool any = false;
        for (const FixedPointer& g : fixed) {
            any = any || g.target >= 0;
        }
        for (const auto& [local, target] : ps.whole) {
            here[static_cast<std::size_t>(local)].target = target;
            any = true;
        }
        if (any) {
            RewriteFixed(p, here, body, false, ps.changes);
        }
    }
    return ps.changes;
}

} // namespace tisple::opt
