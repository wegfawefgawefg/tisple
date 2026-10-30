#include "backend/gen.hpp"

#include <algorithm>

// Page pointers (gen_index.cpp has the other loop pointers): (at table i) with a u16 i in
// memory reads as (page pointer),Y, Y the index's low byte, the pointer's low byte the
// table's (set once where the function starts) and its high byte the table's plus i's, worked
// out at the access. A u16 element of a split array takes one a plane. A second access of the
// same table and index reads through the pointer the first set up while its high byte holds
// (Gen::page_known: until a label, a call or a write to the index's high byte or the pointer).

namespace tisple {

namespace {

bool SameByteValue(const AsmValue& a, const AsmValue& b) {
    return a.sym == b.sym && a.offset == b.offset && a.part == b.part;
}

// a function keeps this many page pointers at most (they live in zero page)
constexpr std::size_t kMaxPagePointers = 6;

// the known pointer at base's page for this high byte, or sym -1
AsmValue KnownPage(const Gen& g, const AsmValue& base, const ByteRef& high) {
    for (const PageKnown& k : g.page_known) {
        if (SameByteValue(k.base, base) && k.high.kind == high.kind &&
            SameByteValue(k.high.value, high.value)) {
            return k.ptr;
        }
    }
    return AsmValue();
}

// does the site need a page pointer it has not got yet for base
bool NeedsNewPage(const Gen& g, ExprId site, const AsmValue& base, const ByteRef& high) {
    if (KnownPage(g, base, high).sym >= 0) {
        return false;
    }
    for (const PagePointer& p : g.page_ptrs) {
        if (p.site == site && SameByteValue(p.base, base)) {
            return false;
        }
    }
    return true;
}

// the page pointer for base indexed by index (its high byte worked out now, unless known)
AsmValue PagePointerAt(Gen& g, ExprId site, const AsmValue& base, const Opnd& index) {
    const ByteRef& high = index.b[1];
    AsmValue known = KnownPage(g, base, high);
    if (known.sym >= 0) {
        return known;
    }
    const PagePointer* found = nullptr;
    for (const PagePointer& p : g.page_ptrs) {
        if (p.site == site && SameByteValue(p.base, base)) {
            found = &p;
        }
    }
    if (!found) {
        PagePointer p;
        p.site = site;
        p.base = base;
        // its low byte is set once at the function's start
        p.ptr = NewLoopPointer(g, true);
        g.page_ptrs.push_back(p);
        found = &g.page_ptrs.back();
    }
    AsmValue ptr = found->ptr;
    std::size_t which = static_cast<std::size_t>(found - g.page_ptrs.data());
    if (!FollowPagePointer(g, index, which)) {
        LoadA(g, index, 1);
        Emit(g, Mn::CLC, Mode::Imp);
        OpA(g, Mn::ADC, SymImmOpnd(base, 2), 1);
        Emit(g, Mn::STA, Mode::Abs, SymValue(ptr.sym, ptr.offset + 1));
    }
    PageKnown k;
    k.ptr = ptr;
    k.base = base;
    k.high = high;
    g.page_known.push_back(k);
    return ptr;
}

} // namespace

bool UsePagePointer(Gen& g, PlaceRef& place, TypeId type) {
    const Opnd& index = place.index;
    if (place.pointer || !place.has_index || place.mult != 1 || place.delta != 0 ||
        place.site < 0 || index.size != 2 || index.in_a || index.index.used) {
        return false;
    }
    // a byte, or a u16 of a split array: its high byte a plane away, through a pointer of its own
    std::uint32_t size = TypeOf(*g.program, type).size;
    bool word = size == 2 && place.cursor.split && place.cursor.stride > 1;
    if (!IsScalarKind(KindOf(*g.program, type)) || (size != 1 && !word)) {
        return false;
    }
    ByteKind high = index.b[1].kind;
    if (index.b[0].kind != ByteKind::Mem || (high != ByteKind::Mem && high != ByteKind::Imm) ||
        IsSteppedIndex(g, index)) {
        return false;
    }
    AsmValue base = place.base;
    base.offset = static_cast<std::int32_t>((base.offset + place.cursor.address) & 0xFFFF);
    AsmValue base2 = base;
    base2.offset = static_cast<std::int32_t>((base.offset + place.cursor.stride) & 0xFFFF);
    std::size_t needed = NeedsNewPage(g, place.site, base, index.b[1]) ? 1 : 0;
    if (word && NeedsNewPage(g, place.site, base2, index.b[1])) {
        needed++;
    }
    if (g.page_ptrs.size() + needed > kMaxPagePointers) {
        return false;
    }
    // (a read of (at came_from next), then its store: the same pointer)
    AsmValue ptr = PagePointerAt(g, place.site, base, index);
    AsmValue ptr2 = word ? PagePointerAt(g, place.site, base2, index) : AsmValue();
    Opnd low = index;
    low.size = 1;
    place.pointer = true;
    place.ptr = ptr;
    place.ptr2 = ptr2;
    place.cursor.address = 0;
    place.index = low;
    place.counter = -1;
    return true;
}

void InitPagePointers(Gen& g) {
    if (g.page_ptrs.empty()) {
        return;
    }
    std::size_t before = g.out->size();
    for (const PagePointer& p : g.page_ptrs) {
        AsmValue low = p.base;
        low.part = Part::Low;
        Emit(g, Mn::LDA, Mode::Imm, low);
        Emit(g, Mn::STA, Mode::Abs, p.ptr);
    }
    // just after the function's entry label
    std::rotate(g.out->begin() + 1, g.out->begin() + static_cast<std::ptrdiff_t>(before),
                g.out->end());
}

} // namespace tisple
