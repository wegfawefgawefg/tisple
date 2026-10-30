// Const data in the mid-level passes, beside the constant walk (consts_eval.cpp reads values at
// known places):
//
// - A pointer or function pointer read at a constant place in const data, where the link fills
//   in an address (an AddrSlot), becomes that address: (at NAMES 2) is the string's
//   (addr_of (at str#2 0)), (at STEPS 1) is the function. Then loads through it fold too, and
//   the backend sees a constant address.
// - A read at places its indexes' ranges allow (the ranges pass) gives the least and the most
//   of the values there, and when there are few, the values themselves, so (= (at T i kind) 3)
//   is false when no element's kind is 3 even inside that range.

#include "check/arith.hpp"
#include "opt/ranges.hpp"

#include <algorithm>

namespace tisple::opt {

namespace {

// the reads one look at a const place may make, at most (indexes' ranges multiplied out)
constexpr std::size_t kMaxPlaces = 4096;
// distinct values a read's set keeps; more and it is only a range
constexpr std::size_t kMaxValues = 16;

ExprId NewNode(Pass& p, ExprKind kind, TypeId type, ExprId a, ExprId b, const Loc& loc) {
    Expr e;
    e.kind = kind;
    e.type = type;
    e.a = a;
    e.b = b;
    e.loc = loc;
    return NewExpr(p, e);
}

// a place in var whose bytes start at offset and whose type is want: fields and elements down
// from the var's own type, with literal indexes; -1 when no place starts there with that type
ExprId PlaceAt(Pass& p, std::int32_t var, std::uint32_t offset, TypeId want, const Loc& loc) {
    TypeId type = VarOf(p, var).type;
    Expr root;
    root.kind = ExprKind::VarRef;
    root.type = type;
    root.ref = var;
    root.loc = loc;
    ExprId place = NewExpr(p, root);
    while (type != want || offset != 0) {
        const Type& t = TypeOf(*p.program, type);
        if (t.kind == TypeKind::Array) {
            std::uint32_t size = TypeOf(*p.program, t.elem).size;
            std::uint32_t index = offset / size;
            if (index >= t.count) {
                return -1;
            }
            Expr literal;
            literal.kind = ExprKind::Literal;
            literal.type = t.count <= 256 ? kU8Type : kU16Type;
            literal.value = index;
            literal.loc = loc;
            place = NewNode(p, ExprKind::Index, t.elem, place, NewExpr(p, literal), loc);
            offset -= index * size;
            type = t.elem;
            continue;
        }
        if (t.kind != TypeKind::Struct) {
            return -1;
        }
        const StructDecl& sd = p.program->structs[static_cast<std::size_t>(t.decl)];
        std::size_t field = sd.fields.size();
        for (std::size_t i = 0; i < sd.fields.size(); i++) {
            std::uint32_t size = TypeOf(*p.program, sd.fields[i].type).size;
            if (offset >= sd.fields[i].offset && offset < sd.fields[i].offset + size) {
                field = i;
            }
        }
        if (field == sd.fields.size()) {
            return -1;
        }
        ExprId access = NewNode(p, ExprKind::Field, sd.fields[field].type, place, -1, loc);
        p.program->exprs[static_cast<std::size_t>(access)].ref = static_cast<std::int32_t>(field);
        p.program->exprs[static_cast<std::size_t>(access)].value = sd.fields[field].offset;
        place = access;
        offset -= sd.fields[field].offset;
        type = sd.fields[field].type;
    }
    return place;
}

// the packed offsets in const data a place may read, each index anywhere its range allows;
// false when the place is not in const data, an index may leave its array, or there are too
// many
bool PlaceOffsets(const Pass& p, const Counters& counters, ExprId place, std::int32_t& var,
                  std::vector<std::uint32_t>& offsets) {
    const Expr& e = At(p, place);
    switch (e.kind) {
    case ExprKind::VarRef:
        var = e.ref;
        offsets.assign(1, 0);
        return VarOf(p, e.ref).kind == VarKind::Const;
    case ExprKind::Field:
        if (!PlaceOffsets(p, counters, e.a, var, offsets)) {
            return false;
        }
        for (std::uint32_t& offset : offsets) {
            offset += e.value;
        }
        return true;
    case ExprKind::Index: {
        const Type& array = TypeOf(*p.program, At(p, e.a).type);
        if (array.kind != TypeKind::Array || !PlaceOffsets(p, counters, e.a, var, offsets) ||
            !Quiet(p, e.b)) {
            return false;
        }
        Range k = ValueRange(p, counters, e.b);
        if (k.lo < 0 || k.hi >= static_cast<std::int64_t>(array.count) ||
            offsets.size() * static_cast<std::size_t>(k.hi - k.lo + 1) > kMaxPlaces) {
            return false;
        }
        std::uint32_t size = TypeOf(*p.program, array.elem).size;
        std::vector<std::uint32_t> out;
        for (std::uint32_t offset : offsets) {
            for (std::int64_t i = k.lo; i <= k.hi; i++) {
                out.push_back(offset + static_cast<std::uint32_t>(i) * size);
            }
        }
        offsets = out;
        return true;
    }
    default:
        return false;
    }
}

} // namespace

bool FoldAddressRead(Pass& p, ExprId id) {
    const Expr e = At(p, id);
    TypeKind kind = KindOf(*p.program, e.type);
    std::int32_t var = -1;
    std::uint32_t offset = 0;
    bool address = kind == TypeKind::Ptr || kind == TypeKind::Fn;
    if ((e.kind != ExprKind::Index && e.kind != ExprKind::Field) || !address ||
        !ConstPlace(*p.program, id, var, offset)) {
        return false;
    }
    const AddrSlot* found = SlotOverlapping(VarOf(p, var), offset, 2);
    if (!found || found->offset != offset) {
        return false;
    }
    AddrSlot slot = *found;
    Expr with;
    with.type = e.type;
    with.loc = e.loc;
    if (slot.function >= 0) {
        with.kind = ExprKind::FnRef;
        with.ref = slot.function;
    } else {
        TypeId elem = TypeOf(*p.program, e.type).elem;
        ExprId place = PlaceAt(p, slot.var, slot.at, elem, e.loc);
        if (place < 0) {
            return false;
        }
        with.kind = ExprKind::AddrOf;
        with.a = place;
    }
    if (!Allow(p, "address read from const data", id, -1)) {
        return false;
    }
    // as ReplaceWith, from a node never added: id names the new place, and lets go of its own
    EachChild(p, with, [&](ExprId child) { Retain(p, child); });
    p.program->exprs[static_cast<std::size_t>(id)] = with;
    EachChild(p, e, [&](ExprId child) { Release(p, child); });
    return true;
}

bool ConstDataRange(const Pass& p, const Counters& counters, ExprId read, Range& range,
                    std::vector<std::int64_t>* values) {
    const Expr& e = At(p, read);
    TypeKind kind = KindOf(*p.program, e.type);
    std::uint32_t size = TypeOf(*p.program, e.type).size;
    std::int32_t var = -1;
    std::vector<std::uint32_t> offsets;
    bool scalar = IsIntegerKind(kind) || kind == TypeKind::Bool || kind == TypeKind::Enum;
    if (!scalar || !PlaceOffsets(p, counters, read, var, offsets)) {
        return false;
    }
    std::vector<std::int64_t> seen;
    for (std::size_t i = 0; i < offsets.size(); i++) {
        std::uint16_t bits = 0;
        if (!InitBits(VarOf(p, var), offsets[i], size, bits)) {
            return false;
        }
        std::int64_t v = SignedValue(kind, MaskToKind(kind, bits));
        range = i == 0 ? Range{v, v} : Range{std::min(range.lo, v), std::max(range.hi, v)};
        if (seen.size() <= kMaxValues && std::find(seen.begin(), seen.end(), v) == seen.end()) {
            seen.push_back(v);
        }
    }
    if (values) {
        values->clear();
        if (seen.size() <= kMaxValues) {
            *values = seen;
        }
    }
    return !offsets.empty();
}

} // namespace tisple::opt
