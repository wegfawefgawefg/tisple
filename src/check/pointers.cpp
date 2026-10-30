#include "check/checker.hpp"

#include <cstdio>

namespace tisple {

namespace {

TypeKind Kind(const Checker& ck, TypeId type) {
    return KindOf(*ck.out, type);
}

std::string FieldList(const StructDecl& sd) {
    std::string text;
    for (const Field& field : sd.fields) {
        text += (text.empty() ? "" : " ") + field.name;
    }
    return text;
}

ExprId CheckPointer(Checker& ck, const Node& node, const char* form) {
    if (IsBareLiteral(node)) {
        return Fail(ck, node,
                    std::string(form) + " needs a pointer; write (" + form + " (as (ptr u8) " +
                        node.text + ") ...)");
    }
    ExprId p = CheckExpr(ck, node, -1);
    TypeId type = ExprType(ck, p);
    if (type == kErrorType || Kind(ck, type) == TypeKind::Ptr) {
        return p;
    }
    std::string fix = type == kU16Type ? "; write (as (ptr u8) " + Show(node) + ")" : "";
    return Fail(ck, node,
                std::string(form) + " goes through a pointer; " + Show(node) + " is " +
                    ShowType(ck, type) + fix);
}

// (load p pos x): walks the field names items[2..end) from what p points to
bool PointerFields(Checker& ck, const Node& node, std::size_t end, TypeId pointer, TypeId& type,
                   std::uint32_t& offset) {
    type = TypeOf(*ck.out, pointer).elem;
    offset = 0;
    for (std::size_t i = 2; i < end; i++) {
        const Node& field = node.items[i];
        if (field.kind != NodeKind::Symbol || field.text.find('.') != std::string::npos) {
            Err(ck, field,
                "(" + node.items[0].text + " p field ...) takes plain field names, like x");
            return false;
        }
        if (Kind(ck, type) != TypeKind::Struct) {
            Err(ck, field,
                Show(node.items[1]) + " points to " + ShowType(ck, type) + ", which has no field " +
                    field.text);
            return false;
        }
        std::int32_t index = FindFieldIndex(*ck.out, type, field.text);
        const StructDecl& sd =
            ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
        if (index < 0) {
            Err(ck, field,
                sd.name + " has no field " + field.text + "; its fields are " + FieldList(sd));
            return false;
        }
        Note(ck, field, 0, NameUseKind::Field, TypeOf(*ck.out, type).decl, index, false);
        offset += sd.fields[static_cast<std::size_t>(index)].offset;
        type = sd.fields[static_cast<std::size_t>(index)].type;
    }
    return true;
}

} // namespace

ExprId CheckLoad(Checker& ck, const Node& node) {
    if (node.items.size() < 2) {
        return Fail(ck, node,
                    "load takes a pointer and maybe field names; write (load p) or (load p x)");
    }
    ExprId p = CheckPointer(ck, node.items[1], "load");
    TypeId pointer = ExprType(ck, p);
    if (pointer == kErrorType) {
        return ErrorExpr(ck, node);
    }
    TypeId type = kErrorType;
    std::uint32_t offset = 0;
    if (!PointerFields(ck, node, node.items.size(), pointer, type, offset)) {
        return ErrorExpr(ck, node);
    }
    bool field = node.items.size() > 2;
    ExprId id = AddExpr(ck, field ? ExprKind::LoadField : ExprKind::Load, type, node);
    ExprAt(ck, id).a = p;
    ExprAt(ck, id).value = offset;
    return id;
}

ExprId CheckStore(Checker& ck, const Node& node) {
    if (node.items.size() < 3) {
        return Fail(ck, node,
                    "store takes a pointer, maybe field names, and a value; write (store p v) or "
                    "(store p x v)");
    }
    const Node& value_node = node.items.back();
    ExprId p = CheckPointer(ck, node.items[1], "store");
    TypeId pointer = ExprType(ck, p);
    if (pointer == kErrorType) {
        // the value is still checked, but a bare literal in it has no type to complain about
        CheckExpr(ck, value_node, kErrorType);
        return ErrorExpr(ck, node);
    }
    TypeId type = kErrorType;
    std::uint32_t offset = 0;
    if (!PointerFields(ck, node, node.items.size() - 1, pointer, type, offset)) {
        return ErrorExpr(ck, node);
    }
    if (Kind(ck, type) == TypeKind::Array) {
        return Fail(ck, node, "store writes one value; store arrays element by element");
    }
    ExprId value = CheckExpr(ck, value_node, type);
    std::string target = Show(node.items[1]);
    for (std::size_t i = 2; i + 1 < node.items.size(); i++) {
        target += " " + node.items[i].text;
    }
    RequireType(ck, value_node, value, type, "store through " + target);
    if (Kind(ck, type) == TypeKind::Struct) {
        RequireStructSource(ck, value_node, value);
    }
    bool field = node.items.size() > 3;
    ExprId id = AddExpr(ck, field ? ExprKind::StoreField : ExprKind::Store, kVoidType, node);
    Expr& expr = ExprAt(ck, id);
    expr.a = p;
    expr.b = value;
    expr.value = offset;
    expr.ref = type;
    return id;
}

ExprId CheckAddrOf(Checker& ck, const Node& node) {
    if (node.items.size() != 2) {
        return Fail(ck, node, "addr_of takes one place; write (addr_of x)");
    }
    const Node& place_node = node.items[1];
    ExprId place = CheckExpr(ck, place_node, -1);
    const Expr& expr = ExprAt(ck, place);
    if (expr.type == kErrorType) {
        return ErrorExpr(ck, node);
    }
    if (expr.kind == ExprKind::Literal) {
        return Fail(ck, place_node,
                    Show(place_node) +
                        " is a number with no address; give it storage with a top-level let, "
                        "or make it a const table");
    }
    if (expr.kind == ExprKind::FnRef) {
        return Fail(ck, place_node,
                    "a function's name is its address already; write " + Show(place_node));
    }
    bool through_load = false;
    std::int32_t root = PlaceRoot(ck, place, through_load);
    if (through_load) {
        return Fail(ck, place_node,
                    "addr_of does not go through pointers; compute the address from the pointer");
    }
    if (root < 0) {
        return Fail(ck, place_node,
                    "addr_of needs a variable, element or field; " + Show(place_node) +
                        " is not one");
    }
    // a const's address is read-only data: a store through it is undefined, like a string's
    Var& var = ck.out->vars[static_cast<std::size_t>(root)];
    if (var.loop_var) {
        return Fail(ck, place_node,
                    "the loop variable " + var.name + " has no address; copy it into a let first");
    }
    var.address_taken = true;
    ExprId id = AddExpr(ck, ExprKind::AddrOf, PtrType(ck, expr.type), node);
    ExprAt(ck, id).a = place;
    return id;
}

namespace {

// (as u8 x): integers and enum members to any integer type
ExprId CheckIntegerAs(Checker& ck, const Node& node, TypeId to) {
    const Node& arg = node.items[2];
    std::string to_text = ShowType(ck, to);
    if (IsBareLiteral(arg)) {
        return Fail(ck, node,
                    Show(node) +
                        " converts nothing: a literal takes its type from where it is used; "
                        "write " +
                        arg.text);
    }
    ExprId a = CheckExpr(ck, arg, -1);
    TypeId from = ExprType(ck, a);
    if (from <= kErrorType) {
        return ErrorExpr(ck, node);
    }
    TypeKind kind = Kind(ck, from);
    if (kind == TypeKind::Ptr) {
        if (to == kU16Type) {
            ExprId id = AddExpr(ck, ExprKind::Reinterpret, to, node);
            ExprAt(ck, id).a = a;
            return id;
        }
        return Fail(ck, node,
                    "a pointer converts only to u16; write (as " + to_text + " (as u16 " +
                        Show(arg) + "))");
    }
    if (kind == TypeKind::Bool) {
        return Fail(ck, node, "as converts integers, not bool; write (if " + Show(arg) + " 1 0)");
    }
    // an enum member converts to its number; nothing converts back
    if (kind == TypeKind::Fn) {
        return Fail(ck, node,
                    "a function pointer converts to nothing; call it, or compare it with = or !=");
    }
    if (!IsIntegerKind(kind) && kind != TypeKind::Enum) {
        return Fail(ck, node,
                    "as converts integers, enums and pointers, not " + ShowType(ck, from));
    }
    ExprId id = AddExpr(ck, ExprKind::Convert, to, node);
    ExprAt(ck, id).a = a;
    return id;
}

} // namespace

ExprId CheckAs(Checker& ck, const Node& node) {
    const char* help = "as takes a type and a value: (as u16 x), (as u8 Action.fire), "
                       "(as (ptr u8) addr) or (as u16 p)";
    if (node.items.size() != 3) {
        return Fail(ck, node, help);
    }
    const Node& type_node = node.items[1];
    if (type_node.kind == NodeKind::Symbol && type_node.text == "void") {
        return Fail(ck, node, help);
    }
    TypeId to = ResolveType(ck, type_node);
    if (to == kErrorType) {
        CheckExpr(ck, node.items[2], kErrorType);
        return ErrorExpr(ck, node);
    }
    TypeKind kind = Kind(ck, to);
    if (IsIntegerKind(kind)) {
        return CheckIntegerAs(ck, node, to);
    }
    if (kind == TypeKind::Enum) {
        const EnumDecl& ed = ck.out->enums[static_cast<std::size_t>(TypeOf(*ck.out, to).decl)];
        return Fail(ck, node,
                    "nothing converts to an enum; name a member, as in " + ed.name + "." +
                        ed.members[0].name + ", or match on the number");
    }
    if (kind == TypeKind::Bool) {
        return Fail(ck, node, "nothing converts to bool; write (!= " + Show(node.items[2]) + " 0)");
    }
    if (kind == TypeKind::Fn) {
        return Fail(ck, node,
                    "nothing converts to a function pointer; name a function, as in (let f " +
                        ShowType(ck, to) + " my_fn)");
    }
    if (kind != TypeKind::Ptr) {
        return Fail(ck, node, help);
    }
    const Node& value_node = node.items[2];
    ExprId value = CheckExpr(ck, value_node, kU16Type);
    if (!RequireType(ck, value_node, value, kU16Type, "(as " + ShowType(ck, to) + " ...)")) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::Reinterpret, to, node);
    ExprAt(ck, id).a = value;
    return id;
}

void CheckRegionUses(Checker& ck) {
    const Program& program = *ck.out;
    std::vector<bool> under_addr_of(program.exprs.size(), false);
    for (const Expr& e : program.exprs) {
        if (e.kind != ExprKind::AddrOf) {
            continue;
        }
        ExprId place = e.a;
        while (place >= 0 && (ExprAt(ck, place).kind == ExprKind::Index ||
                              ExprAt(ck, place).kind == ExprKind::Field)) {
            place = ExprAt(ck, place).a;
        }
        if (place >= 0) {
            under_addr_of[static_cast<std::size_t>(place)] = true;
        }
    }
    for (std::size_t i = 0; i < program.exprs.size(); i++) {
        const Expr& e = program.exprs[i];
        if (e.kind != ExprKind::VarRef || e.ref < 0) {
            continue;
        }
        const Var& var = program.vars[static_cast<std::size_t>(e.ref)];
        std::string name = var.name.substr(var.name.rfind('.') + 1);
        if (var.placement.reserve) {
            char address[16];
            std::snprintf(address, sizeof(address), "0x%04X", var.placement.address);
            AddError(*ck.errors, e.loc,
                     name +
                         " is :reserve memory, not a variable; reach it with a pointer, as in "
                         "(as (ptr u8) " +
                         address + ")");
        } else if (var.placement.in_aux && !under_addr_of[i]) {
            AddError(*ck.errors, e.loc,
                     name +
                         " is in aux memory, which code reaches only through lib/auxmem; copy "
                         "it with (auxmem.read (addr_of buffer) (addr_of " +
                         name + ") count)");
        }
    }
}

} // namespace tisple
