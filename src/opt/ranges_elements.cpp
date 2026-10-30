// Elements no store reaches (docs/midlevel-optimizer.md, slice 5): ranges_vars.cpp's walk notes
// the packed bytes each set rooted at an array or struct global may write, from its index
// ranges; a read of bytes none of them reaches gives the variable's initial value. A global
// (not :address, aux or :reserve) is covered when no store through a pointer may reach it
// (collapse_memory.cpp's provenance) and the program has a main (with none, memory may be
// written from outside between calls). Off when some set's index may fall outside its array:
// it would write a neighbour.

#include "check/arith.hpp"
#include "opt/collapse.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

// an element read over more indexes than this is not looked at (the compile stays fast)
constexpr std::int64_t kMaxElements = 64;

// the packed bytes a place rooted at a variable may cover; false when it is behind a pointer
bool PlaceBytes(const Pass& p, const Counters& counters, ExprId place, std::int32_t& root,
                Range& bytes, bool& in_bounds) {
    const Expr& e = At(p, place);
    switch (e.kind) {
    case ExprKind::VarRef: {
        root = e.ref;
        std::int64_t size = TypeOf(*p.program, VarOf(p, e.ref).type).size;
        bytes = Range{0, size - 1};
        return size > 0;
    }
    case ExprKind::Index: {
        Range base;
        if (!PlaceBytes(p, counters, e.a, root, base, in_bounds)) {
            return false;
        }
        const Type& array = TypeOf(*p.program, At(p, e.a).type);
        std::int64_t size = TypeOf(*p.program, array.elem).size;
        std::int64_t count = array.count;
        Range i = ValueRange(p, counters, e.b);
        if (i.lo < 0 || i.hi >= count) {
            in_bounds = false;
            i = Range{0, count - 1};
        }
        bytes = Range{base.lo + i.lo * size, base.hi - (count - 1 - i.hi) * size};
        return true;
    }
    case ExprKind::Field: {
        Range base;
        if (!PlaceBytes(p, counters, e.a, root, base, in_bounds)) {
            return false;
        }
        std::int64_t whole = TypeOf(*p.program, At(p, e.a).type).size;
        std::int64_t size = TypeOf(*p.program, e.type).size;
        bytes = Range{base.lo + e.value, base.hi - (whole - e.value - size)};
        return true;
    }
    default:
        return false;
    }
}

} // namespace

void NoteStore(const Pass& p, const Counters& counters, ExprId place, ElementStores& stores) {
    std::int32_t root = -1;
    Range bytes;
    if (PlaceBytes(p, counters, place, root, bytes, stores.in_bounds)) {
        stores.stored[static_cast<std::size_t>(root)].push_back(bytes);
    }
}

void SettleStores(Pass& p, const ElementStores& stores) {
    VarRanges& r = p.ranges;
    std::size_t vars = p.program->vars.size();
    r.stored.assign(vars, {});
    r.unstored.assign(vars, false);
    if (!stores.in_bounds || p.program->main < 0) {
        return;
    }
    MemoryFacts memory;
    for (std::size_t var = 0; var < vars; var++) {
        const Var& v = p.program->vars[var];
        TypeKind kind = KindOf(*p.program, v.type);
        bool aggregate = kind == TypeKind::Array || kind == TypeKind::Struct;
        if (!aggregate || v.kind != VarKind::Global || Hardware(v)) {
            continue;
        }
        bool reached = false;
        for (std::size_t f = 0; f < p.program->functions.size() && !reached; f++) {
            reached = stores.runs[f] && StoresMayReach(p, memory, static_cast<std::int32_t>(f),
                                                       static_cast<std::int32_t>(var));
        }
        if (reached) {
            continue;
        }
        std::vector<Range> bytes = stores.stored[var];
        std::sort(bytes.begin(), bytes.end(),
                  [](const Range& a, const Range& b) { return a.lo < b.lo; });
        for (const Range& b : bytes) {
            if (!r.stored[var].empty() && b.lo <= r.stored[var].back().hi + 1) {
                r.stored[var].back().hi = std::max(r.stored[var].back().hi, b.hi);
            } else {
                r.stored[var].push_back(b);
            }
        }
        r.unstored[var] = true;
    }
}

ExprId ReadIndex(const Pass& p, const Expr& read) {
    const Expr* e = &read;
    while (e->kind == ExprKind::Field) {
        e = &At(p, e->a);
    }
    return e->kind == ExprKind::Index ? e->b : -1;
}

bool UnstoredRead(const Pass& p, const Expr& read, Range index, Range& out) {
    const VarRanges& r = p.ranges;
    if (!r.elements) {
        return false;
    }
    // fields down to an element of an array variable, or to a struct variable
    std::int64_t offset = 0;
    const Expr* e = &read;
    while (e->kind == ExprKind::Field) {
        offset += e->value;
        e = &At(p, e->a);
    }
    std::int64_t size = 0;
    std::int32_t var = -1;
    if (e->kind == ExprKind::Index && At(p, e->a).kind == ExprKind::VarRef) {
        var = At(p, e->a).ref;
        const Type& array = TypeOf(*p.program, VarOf(p, var).type);
        size = TypeOf(*p.program, array.elem).size;
        if (array.kind != TypeKind::Array || index.lo < 0 || index.hi >= array.count) {
            return false;
        }
    } else if (e->kind == ExprKind::VarRef) {
        var = e->ref;
        index = Range{0, 0};
    }
    if (var < 0 || static_cast<std::size_t>(var) >= r.unstored.size() ||
        !r.unstored[static_cast<std::size_t>(var)]) {
        return false;
    }
    TypeKind kind = KindOf(*p.program, read.type);
    std::int64_t width = TypeOf(*p.program, read.type).size;
    const Var& v = VarOf(p, var);
    if (index.hi - index.lo >= kMaxElements) {
        // (slice 6) many elements, all zero to begin with, no store reaching the bytes between
        // the first and the last (seed 841's scan of wb at an index from a counter)
        std::int64_t first = index.lo * size + offset;
        std::int64_t last = index.hi * size + offset + width - 1;
        for (const Range& b : r.stored[static_cast<std::size_t>(var)]) {
            if (b.lo <= last && first <= b.hi) {
                return false;
            }
        }
        if (!v.init.empty()) {
            return false;
        }
        out = Range{0, 0};
        return true;
    }
    bool seen = false;
    for (std::int64_t k = index.lo; k <= index.hi; k++) {
        std::int64_t at = k * size + offset;
        for (const Range& b : r.stored[static_cast<std::size_t>(var)]) {
            if (b.lo <= at + width - 1 && at <= b.hi) {
                return false;
            }
        }
        std::uint32_t bits = 0;
        for (std::int64_t i = 0; i < width; i++) {
            std::size_t byte = static_cast<std::size_t>(at + i);
            std::uint32_t value = byte < v.init.size() ? v.init[byte] : 0;
            bits |= value << (8 * i);
        }
        std::int64_t value = SignedValue(kind, MaskToKind(kind, bits));
        out = seen ? Range{std::min(out.lo, value), std::max(out.hi, value)} : Range{value, value};
        seen = true;
    }
    return seen;
}

} // namespace tisple::opt
