#include "check/checker.hpp"

namespace tisple {

namespace {

bool IsPlaceLike(ExprKind kind) {
    return IsPlaceKind(kind) || kind == ExprKind::Load || kind == ExprKind::LoadField;
}

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

void AddCallee(Checker& ck, std::int32_t function) {
    if (ck.at.function < 0) {
        return;
    }
    std::vector<std::int32_t>& callees =
        ck.out->functions[static_cast<std::size_t>(ck.at.function)].callees;
    for (std::int32_t callee : callees) {
        if (callee == function) {
            return;
        }
    }
    callees.push_back(function);
}

std::string ParamText(const Checker& ck, const Function& fn) {
    std::string text;
    for (std::int32_t param : fn.params) {
        const Var& var = ck.out->vars[static_cast<std::size_t>(param)];
        text += (text.empty() ? "" : " ") + var.name;
    }
    return text;
}

} // namespace

std::int32_t PlaceRoot(Checker& ck, ExprId place, bool& through_load) {
    through_load = false;
    while (true) {
        const Expr& expr = ExprAt(ck, place);
        if (expr.kind == ExprKind::VarRef) {
            return expr.ref;
        }
        if (expr.kind == ExprKind::Load || expr.kind == ExprKind::LoadField) {
            through_load = true;
            return -1;
        }
        if (expr.kind != ExprKind::Index && expr.kind != ExprKind::Field) {
            return -1;
        }
        place = expr.a;
    }
}

ExprId FieldAccess(Checker& ck, const Node& node, ExprId base, const std::string& field) {
    TypeId type = ExprType(ck, base);
    if (type == kErrorType) {
        return base;
    }
    if (Kind(ck, type) != TypeKind::Struct) {
        return Fail(ck, node,
                    ShowType(ck, type) + " has no fields, so ." + field + " means nothing");
    }
    const StructDecl& sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    for (std::size_t i = 0; i < sd.fields.size(); i++) {
        if (sd.fields[i].name != field) {
            continue;
        }
        ExprId id = AddExpr(ck, ExprKind::Field, sd.fields[i].type, node);
        Expr& expr = ExprAt(ck, id);
        expr.a = base;
        expr.ref = static_cast<std::int32_t>(i);
        expr.value = sd.fields[i].offset;
        return id;
    }
    return Fail(ck, node, sd.name + " has no field " + field + "; its fields are " + FieldList(sd));
}

ExprId CheckAt(Checker& ck, const Node& node) {
    if (node.items.size() != 3) {
        return Fail(ck, node, "at takes an array and an index; write (at arr i)");
    }
    const Node& array_node = node.items[1];
    ExprId array = CheckExpr(ck, array_node, -1);
    TypeId type = ExprType(ck, array);
    if (type == kErrorType) {
        return ErrorExpr(ck, node);
    }
    if (Kind(ck, type) == TypeKind::Ptr) {
        return Fail(ck, node,
                    "at indexes arrays; through a pointer write (load (+ " + Show(array_node) +
                        " " + Show(node.items[2]) + "))");
    }
    if (Kind(ck, type) != TypeKind::Array) {
        return Fail(ck, array_node,
                    Show(array_node) + " is " + ShowType(ck, type) + ", not an array");
    }
    if (!IsPlaceLike(ExprAt(ck, array).kind)) {
        return Fail(ck, array_node, "at needs an array variable, field or element");
    }
    const Type& array_type = TypeOf(*ck.out, type);
    std::uint32_t count = array_type.count;
    TypeId elem = array_type.elem;
    TypeId index_type = count <= 256 ? kU8Type : kU16Type;
    ExprId index = CheckExpr(ck, node.items[2], index_type);
    std::string what =
        "the index into " + Show(array_node) + " (" + std::to_string(count) + " elements)";
    if (!RequireType(ck, node.items[2], index, index_type, what)) {
        return ErrorExpr(ck, node);
    }
    const Expr& index_expr = ExprAt(ck, index);
    if (index_expr.kind == ExprKind::Literal && index_expr.type != kErrorType &&
        index_expr.value >= count) {
        return Fail(ck, node.items[2],
                    "index " + std::to_string(index_expr.value) + " is past the end of " +
                        Show(array_node) + " (" + std::to_string(count) + " elements)");
    }
    ExprId id = AddExpr(ck, ExprKind::Index, elem, node);
    ExprAt(ck, id).a = array;
    ExprAt(ck, id).b = index;
    return id;
}

ExprId CheckDot(Checker& ck, const Node& node) {
    if (node.items.size() < 3) {
        return Fail(ck, node, "(. expr field ...) needs a value and field names");
    }
    std::string fields;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        const Node& field = node.items[i];
        if (field.kind != NodeKind::Symbol || field.text.find('.') != std::string::npos) {
            return Fail(ck, field, "(. expr field ...) takes plain field names, like x");
        }
        fields += "." + field.text;
    }
    const Node& base_node = node.items[1];
    // a template cannot glue ,s and .x into one symbol, so expanded code may write (. s x)
    if (base_node.kind == NodeKind::Symbol && node.expansion == 0) {
        return Fail(ck, node,
                    Show(node) + " reads a field of a plain name; write " + base_node.text +
                        fields);
    }
    if (HeadName(base_node) == "load") {
        std::string fix = "(load";
        for (std::size_t i = 1; i < base_node.items.size(); i++) {
            fix += " " + Show(base_node.items[i]);
        }
        for (std::size_t i = 2; i < node.items.size(); i++) {
            fix += " " + node.items[i].text;
        }
        return Fail(ck, node, Show(node) + " reads a field through a pointer; write " + fix + ")");
    }
    ExprId base = CheckExpr(ck, base_node, -1);
    TypeId type = ExprType(ck, base);
    if (type == kErrorType) {
        return base;
    }
    if (Kind(ck, type) == TypeKind::Struct && !IsPlaceLike(ExprAt(ck, base).kind)) {
        return Fail(ck, base_node,
                    "take fields of a variable, (at ...) or a field, not " + Show(base_node));
    }
    ExprId value = base;
    for (std::size_t i = 2; i < node.items.size(); i++) {
        value = FieldAccess(ck, node, value, node.items[i].text);
        if (ExprType(ck, value) == kErrorType) {
            return value;
        }
    }
    return value;
}

namespace {

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
        CheckExpr(ck, value_node, -1);
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
        return Fail(ck, place_node, "constants have no address; use a top-level let");
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
    Var& var = ck.out->vars[static_cast<std::size_t>(root)];
    if (var.kind == VarKind::ConstArray) {
        return Fail(ck, place_node, "constants have no address; use a top-level let");
    }
    if (var.loop_var) {
        return Fail(ck, place_node,
                    "the loop variable " + var.name + " has no address; copy it into a let first");
    }
    var.address_taken = true;
    ExprId id = AddExpr(ck, ExprKind::AddrOf, PtrType(ck, expr.type), node);
    ExprAt(ck, id).a = place;
    return id;
}

ExprId CheckAs(Checker& ck, const Node& node) {
    const char* help = "as converts between u16 and pointers: (as (ptr u8) n) or (as u16 p); for "
                       "integers write (u8 x)";
    if (node.items.size() != 3) {
        return Fail(ck, node, help);
    }
    TypeId to = ResolveType(ck, node.items[1]);
    if (to == kErrorType) {
        return ErrorExpr(ck, node);
    }
    const Node& value_node = node.items[2];
    ExprId value = -1;
    if (Kind(ck, to) == TypeKind::Ptr) {
        value = CheckExpr(ck, value_node, kU16Type);
        if (!RequireType(ck, value_node, value, kU16Type, "(as " + ShowType(ck, to) + " ...)")) {
            return ErrorExpr(ck, node);
        }
    } else if (to == kU16Type) {
        value = CheckExpr(ck, value_node, -1);
        TypeId from = ExprType(ck, value);
        if (from == kErrorType) {
            return ErrorExpr(ck, node);
        }
        if (Kind(ck, from) != TypeKind::Ptr) {
            return Fail(ck, node,
                        "(as u16 x) takes a pointer; for an integer write (u16 " +
                            Show(value_node) + ")");
        }
    } else {
        return Fail(ck, node, help);
    }
    ExprId id = AddExpr(ck, ExprKind::Reinterpret, to, node);
    ExprAt(ck, id).a = value;
    return id;
}

namespace {

TypeId IoType(Checker& ck, const Node& node, const char* form) {
    if (node.kind == NodeKind::Symbol && node.text == "u8") {
        return kU8Type;
    }
    if (node.kind == NodeKind::Symbol && node.text == "u16") {
        return kU16Type;
    }
    Err(ck, node,
        std::string(form) + " moves u8 or u16 (low byte first); write (" + form + " u8 addr" +
            (std::string(form) == "io_write" ? " v)" : ")"));
    return kErrorType;
}

} // namespace

ExprId CheckIoRead(Checker& ck, const Node& node) {
    if (node.items.size() != 3) {
        return Fail(ck, node, "io_read takes a width and an address; write (io_read u8 a2.KBD)");
    }
    TypeId type = IoType(ck, node.items[1], "io_read");
    ExprId address = CheckExpr(ck, node.items[2], kU16Type);
    bool ok = RequireType(ck, node.items[2], address, kU16Type, "the io_read address");
    if (type == kErrorType || !ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::IoRead, type, node);
    ExprAt(ck, id).a = address;
    return id;
}

ExprId CheckIoWrite(Checker& ck, const Node& node) {
    if (node.items.size() != 4) {
        return Fail(ck, node,
                    "io_write takes a width, an address and a value; write (io_write u8 addr v)");
    }
    TypeId type = IoType(ck, node.items[1], "io_write");
    ExprId address = CheckExpr(ck, node.items[2], kU16Type);
    bool ok = RequireType(ck, node.items[2], address, kU16Type, "the io_write address");
    ExprId value = CheckExpr(ck, node.items[3], type);
    ok = RequireType(ck, node.items[3], value, type, "the io_write value") && ok;
    if (type == kErrorType || !ok) {
        return ErrorExpr(ck, node);
    }
    ExprId id = AddExpr(ck, ExprKind::IoWrite, kVoidType, node);
    ExprAt(ck, id).a = address;
    ExprAt(ck, id).b = value;
    ExprAt(ck, id).value = TypeOf(*ck.out, type).size;
    return id;
}

ExprId CheckCall(Checker& ck, const Node& node, std::int32_t function) {
    const Function& fn = ck.out->functions[static_cast<std::size_t>(function)];
    const std::string& head = node.items[0].text;
    std::size_t given = node.items.size() - 1;
    if (given != fn.params.size()) {
        std::string params = ParamText(ck, fn);
        return Fail(ck, node,
                    head + " takes " + std::to_string(fn.params.size()) + " argument" +
                        (fn.params.size() == 1 ? "" : "s") + ", got " + std::to_string(given) +
                        "; write (" + head + (params.empty() ? "" : " " + params) + ")");
    }
    std::vector<ExprId> args;
    bool ok = true;
    for (std::size_t i = 0; i < given; i++) {
        const Var& param = ck.out->vars[static_cast<std::size_t>(fn.params[i])];
        TypeId type = param.type;
        std::string param_name = param.name;
        ExprId arg = CheckExpr(ck, node.items[i + 1], type);
        ok = RequireType(ck, node.items[i + 1], arg, type,
                         "argument " + param_name + " of " + head) &&
             ok;
        args.push_back(arg);
    }
    AddCallee(ck, function);
    TypeId ret = ck.out->functions[static_cast<std::size_t>(function)].ret;
    ExprId id = AddExpr(ck, ExprKind::Call, ok ? ret : kErrorType, node);
    Expr& expr = ExprAt(ck, id);
    expr.ref = function;
    expr.first = AddList(ck, args);
    expr.count = static_cast<std::uint32_t>(args.size());
    return id;
}

ExprId CheckStructLit(Checker& ck, const Node& node, TypeId type) {
    const StructDecl sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    const std::string& head = node.items[0].text;
    if (sd.fields.empty()) {
        return ErrorExpr(ck, node);
    }
    std::vector<std::string> names;
    std::vector<const Node*> values;
    for (std::size_t i = 1; i < node.items.size(); i += 2) {
        const Node& key = node.items[i];
        if (key.kind != NodeKind::Keyword) {
            return Fail(ck, key,
                        "struct literals name every field: write (" + head + " :" +
                            sd.fields[0].name + " value ...)");
        }
        if (i + 1 >= node.items.size()) {
            return Fail(ck, key, key.text + " has no value; write " + key.text + " value");
        }
        std::string name = key.text.substr(1);
        if (FindFieldIndex(*ck.out, type, name) < 0) {
            return Fail(ck, key,
                        sd.name + " has no field " + name + "; its fields are " + FieldList(sd));
        }
        for (const std::string& other : names) {
            if (other == name) {
                return Fail(ck, key, key.text + " is given twice; give each field once");
            }
        }
        names.push_back(name);
        values.push_back(&node.items[i + 1]);
    }

    std::vector<const Node*> ordered(sd.fields.size(), nullptr);
    for (std::size_t i = 0; i < names.size(); i++) {
        ordered[static_cast<std::size_t>(FindFieldIndex(*ck.out, type, names[i]))] = values[i];
    }
    std::string fix = "(" + head;
    std::string missing;
    for (std::size_t i = 0; i < sd.fields.size(); i++) {
        fix += " :" + sd.fields[i].name + " " + (ordered[i] ? Show(*ordered[i]) : "...");
        if (!ordered[i]) {
            missing += (missing.empty() ? "" : ", ") + sd.fields[i].name;
        }
    }
    fix += ")";
    if (!missing.empty()) {
        return Fail(ck, node,
                    Show(node) + " is missing " + missing + "; name every field in order: " + fix);
    }
    bool in_order = true;
    for (std::size_t i = 0; i < names.size(); i++) {
        in_order = in_order && names[i] == sd.fields[i].name;
    }
    if (!in_order) {
        return Fail(ck, node, Show(node) + " fields out of order; write " + fix);
    }

    std::vector<ExprId> fields;
    for (std::size_t i = 0; i < sd.fields.size(); i++) {
        TypeId field_type = sd.fields[i].type;
        ExprId value = CheckExpr(ck, *ordered[i], field_type);
        RequireType(ck, *ordered[i], value, field_type,
                    "field " + sd.fields[i].name + " of " + sd.name);
        if (Kind(ck, field_type) == TypeKind::Struct) {
            RequireStructSource(ck, *ordered[i], value);
        }
        if (Kind(ck, field_type) == TypeKind::Array) {
            Err(ck, *ordered[i],
                "array fields start zeroed; leave the struct literal out and fill " +
                    sd.fields[i].name + " with a for loop");
        }
        fields.push_back(value);
    }
    ExprId id = AddExpr(ck, ExprKind::StructLit, type, node);
    ExprAt(ck, id).first = AddList(ck, fields);
    ExprAt(ck, id).count = static_cast<std::uint32_t>(fields.size());
    return id;
}

} // namespace tisple
