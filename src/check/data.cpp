// Static data: the value of a const or a top-level let folded into its packed bytes. Any type:
// numbers, structs and arrays built with make at any depth, a const's value (or a part of one)
// copied, and addresses the link knows. An address is an AddrSlot beside the bytes, which the
// layout fills in (link/image.cpp): a function's name, (addr_of x ...) of a top-level let or a
// const, a string literal (its table's first byte), a pointer moved by a constant count, and a
// pointer or function pointer read from const data.

#include "check/checker.hpp"

#include <algorithm>

namespace tisple {

namespace {

TypeKind Kind(const Checker& ck, TypeId type) {
    return KindOf(*ck.out, type);
}

void PutBytes(std::vector<std::uint8_t>& bytes, std::uint32_t offset, std::uint32_t size,
              std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
    if (size == 2) {
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }
}

// (make (array u16 192) 0x2000 0x2400 ...), shortened for a fix
std::string ArrayLiteralFix(const std::string& type_text) {
    return "(make " + type_text + " v0 v1 ...)";
}

// the var and packed byte offset of a place in a top-level let or a const, its indexes
// constant; false (an error said why, or nothing when it is not such a place)
bool PlaceOffset(Checker& ck, ExprId place, std::int32_t& var, std::uint32_t& offset) {
    const Expr e = ExprAt(ck, place);
    switch (e.kind) {
    case ExprKind::VarRef: {
        const Var& v = ck.out->vars[static_cast<std::size_t>(e.ref)];
        var = e.ref;
        offset = 0;
        if (v.placement.reserve) {
            AddError(*ck.errors, e.loc,
                     v.name + " is :reserve, not a variable; point at its memory with (as (ptr "
                              "u8) address)");
            return false;
        }
        return v.function < 0 && (v.kind == VarKind::Global || v.kind == VarKind::Const);
    }
    case ExprKind::Field:
        if (!PlaceOffset(ck, e.a, var, offset)) {
            return false;
        }
        offset += e.value;
        return true;
    case ExprKind::Index: {
        std::uint16_t index = 0;
        if (!PlaceOffset(ck, e.a, var, offset) || !FoldExpr(ck, e.b, index)) {
            return false;
        }
        const Type& array = TypeOf(*ck.out, ExprType(ck, e.a));
        if (index >= array.count) {
            AddError(*ck.errors, e.loc,
                     "index " + std::to_string(index) + " is past the end of " +
                         ShowType(ck, ExprType(ck, e.a)));
            return false;
        }
        offset += static_cast<std::uint32_t>(index) * TypeOf(*ck.out, array.elem).size;
        return true;
    }
    default:
        return false;
    }
}

// const data read while it is being built: its bytes are not there yet
bool ReadsItsOwnValue(Checker& ck, std::int32_t var, Loc loc) {
    if (std::find(ck.folding.begin(), ck.folding.end(), var) == ck.folding.end()) {
        return false;
    }
    const Var& v = ck.out->vars[static_cast<std::size_t>(var)];
    AddError(*ck.errors, loc,
             v.name + " reads its own value while it is being built; only its address, "
                      "(addr_of ...), may be used in it");
    return true;
}

// a pointer or function pointer read from const data: the slot there
bool CopySlot(Checker& ck, ExprId id, AddrSlot& slot) {
    std::int32_t var = -1;
    std::uint32_t offset = 0;
    if (!ConstPlace(*ck.out, id, var, offset) || ReadsItsOwnValue(ck, var, ExprAt(ck, id).loc)) {
        return false;
    }
    const AddrSlot* found = SlotOverlapping(ck.out->vars[static_cast<std::size_t>(var)], offset, 2);
    if (!found || found->offset != offset) {
        return false;
    }
    slot = *found;
    return true;
}

// a value that is not a make: a const of the type, or a part of one, copied
bool CopyConstValue(Checker& ck, const Node& node, TypeId type, const std::string& what,
                    std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    ExprId value = CheckExpr(ck, node, type);
    if (!RequireType(ck, node, value, type, what)) {
        return false;
    }
    std::int32_t var = -1;
    std::uint32_t from = 0;
    if (!ConstPlace(*ck.out, value, var, from)) {
        Err(ck, node,
            Show(node) + " is not constant; " + what + " is built with (make " +
                ShowType(ck, type) + " ...) or from a const");
        return false;
    }
    if (ReadsItsOwnValue(ck, var, NodeLoc(ck, node))) {
        return false;
    }
    const Var source = ck.out->vars[static_cast<std::size_t>(var)];
    std::uint32_t size = TypeOf(*ck.out, type).size;
    for (std::uint32_t i = 0; i < size; i++) {
        std::size_t at = static_cast<std::size_t>(from + i);
        bytes[offset + i] = at < source.init.size() ? source.init[at] : 0;
    }
    for (const AddrSlot& slot : source.addr_slots) {
        if (slot.offset >= from && slot.offset < from + size) {
            AddrSlot copy = slot;
            copy.offset = slot.offset - from + offset;
            ck.pending_slots.push_back(copy);
        }
    }
    return true;
}

bool FoldArrayValue(Checker& ck, const Node& node, TypeId type, const std::string& what,
                    std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    std::string type_text = ShowType(ck, type);
    const Type array = TypeOf(*ck.out, type);
    TypeId made = ResolveType(ck, node.items[1]);
    if (made == kErrorType) {
        return false;
    }
    if (made != type) {
        Err(ck, node.items[1],
            "the value is " + ShowType(ck, made) + " but " + what + " is " + type_text +
                "; write " + ArrayLiteralFix(type_text));
        return false;
    }
    std::size_t given = node.items.size() - 2;
    if (given != array.count) {
        Err(ck, node,
            what + " is " + type_text + " but its value has " + std::to_string(given) +
                " values; give exactly " + std::to_string(array.count));
        return false;
    }
    std::uint32_t elem_size = TypeOf(*ck.out, array.elem).size;
    bool ok = true;
    for (std::size_t i = 0; i < given; i++) {
        std::uint32_t at = offset + static_cast<std::uint32_t>(i) * elem_size;
        ok = FoldValue(ck, node.items[2 + i], array.elem, "an element of " + what, bytes, at) && ok;
    }
    return ok;
}

bool FoldStructValue(Checker& ck, const Node& node, TypeId type, const std::string& what,
                     std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    TypeId made = ResolveType(ck, node.items[1]);
    if (made == kErrorType) {
        return false;
    }
    if (made != type) {
        Err(ck, node.items[1],
            "the value is " + ShowType(ck, made) + " but " + what + " is " + ShowType(ck, type));
        return false;
    }
    std::vector<const Node*> ordered;
    if (!OrderStructFields(ck, node, type, ordered)) {
        return false;
    }
    const StructDecl sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    bool ok = true;
    for (std::size_t i = 0; i < sd.fields.size(); i++) {
        const Field& field = sd.fields[i];
        std::string field_what = "field " + field.name + " of " + sd.name;
        ok = FoldValue(ck, *ordered[i], field.type, field_what, bytes, offset + field.offset) && ok;
    }
    return ok;
}

} // namespace

bool FoldScalarValue(Checker& ck, ExprId value, TypeId type, std::vector<std::uint8_t>& bytes,
                     std::uint32_t offset) {
    TypeKind kind = Kind(ck, type);
    std::uint16_t raw = 0;
    std::size_t before = ck.errors->size();
    if (kind != TypeKind::Fn && FoldExpr(ck, value, raw)) {
        PutBytes(bytes, offset, TypeOf(*ck.out, type).size, raw);
        return true;
    }
    if (ck.errors->size() != before || (kind != TypeKind::Ptr && kind != TypeKind::Fn)) {
        return false;
    }
    AddrSlot slot;
    if (!FoldAddress(ck, value, slot)) {
        return false;
    }
    slot.offset = offset;
    ck.pending_slots.push_back(slot);
    return true;
}

bool FoldValue(Checker& ck, const Node& node, TypeId type, const std::string& what,
               std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    TypeKind kind = Kind(ck, type);
    bool make = HeadName(node) == "make" && node.items.size() >= 2;
    bool named = node.kind == NodeKind::Symbol || HeadName(node) == "at";
    if (kind == TypeKind::Array && !make && !named) {
        std::string type_text = ShowType(ck, type);
        Err(ck, node,
            what + " is " + type_text + ", so its value is " + ArrayLiteralFix(type_text));
        return false;
    }
    if ((kind == TypeKind::Array || kind == TypeKind::Struct) && !make) {
        return CopyConstValue(ck, node, type, what, bytes, offset);
    }
    if (kind == TypeKind::Array) {
        return FoldArrayValue(ck, node, type, what, bytes, offset);
    }
    if (kind == TypeKind::Struct) {
        return FoldStructValue(ck, node, type, what, bytes, offset);
    }
    ExprId value = CheckExpr(ck, node, type);
    if (!RequireType(ck, node, value, type, what)) {
        return false;
    }
    std::size_t before = ck.errors->size();
    if (FoldScalarValue(ck, value, type, bytes, offset)) {
        return true;
    }
    if (ck.errors->size() == before) {
        Err(ck, node,
            Show(node) + " is not constant; " + what +
                " is built from numbers, constants, operators and addresses: a function's name, "
                "a string, (addr_of x)");
    }
    return false;
}

ExprId CloneExpr(Checker& ck, ExprId id, const Node& at) {
    if (id < 0) {
        return -1;
    }
    Expr copy = ExprAt(ck, id);
    copy.a = CloneExpr(ck, copy.a, at);
    copy.b = CloneExpr(ck, copy.b, at);
    copy.c = CloneExpr(ck, copy.c, at);
    // a constant pointer's tree is places, operators and literals; no lists or clauses
    copy.loc = NodeLoc(ck, at);
    ExprId out = AddExpr(ck, copy.kind, copy.type, at);
    ExprAt(ck, out) = copy;
    return out;
}

bool FoldAddress(Checker& ck, ExprId id, AddrSlot& slot) {
    const Expr e = ExprAt(ck, id);
    switch (e.kind) {
    case ExprKind::FnRef:
        slot = AddrSlot();
        slot.function = e.ref;
        return true;
    case ExprKind::AddrOf: {
        std::int32_t var = -1;
        std::uint32_t offset = 0;
        if (!PlaceOffset(ck, e.a, var, offset)) {
            return false;
        }
        slot = AddrSlot();
        slot.var = var;
        slot.at = offset;
        return true;
    }
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub: {
        std::uint16_t count = 0;
        if (!FoldAddress(ck, e.a, slot) || slot.var < 0 || !FoldExpr(ck, e.b, count)) {
            return false;
        }
        std::int64_t step = static_cast<std::int64_t>(count) * e.value;
        std::int64_t at = slot.at + (e.kind == ExprKind::PtrAdd ? step : -step);
        const Var& target = ck.out->vars[static_cast<std::size_t>(slot.var)];
        std::int64_t size = TypeOf(*ck.out, target.type).size;
        if (at < 0 || at > size) {
            AddError(*ck.errors, e.loc,
                     "this pointer is outside " + target.name + " (" + std::to_string(size) +
                         " bytes); a constant pointer stays inside what it points to");
            return false;
        }
        slot.at = static_cast<std::uint32_t>(at);
        return true;
    }
    case ExprKind::Index:
    case ExprKind::Field:
        return CopySlot(ck, id, slot);
    default:
        return false;
    }
}

} // namespace tisple
