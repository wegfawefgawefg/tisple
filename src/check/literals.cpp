#include "check/checker.hpp"

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

// (make (array u16 192) 0x2000 0x2400 ...), shortened for a fix
std::string ArrayLiteralFix(const std::string& type_text) {
    return "(make " + type_text + " v0 v1 ...)";
}

} // namespace

ExprId CheckStructLit(Checker& ck, const Node& node, TypeId type) {
    const StructDecl sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    if (sd.fields.empty()) {
        return ErrorExpr(ck, node);
    }
    std::vector<std::string> names;
    std::vector<const Node*> values;
    for (std::size_t i = 2; i < node.items.size(); i += 2) {
        const Node& key = node.items[i];
        if (key.kind != NodeKind::Keyword) {
            return Fail(ck, key,
                        "struct literals name every field: write (make " + sd.name + " :" +
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
    std::string fix = "(make " + sd.name;
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
        if (Kind(ck, field_type) == TypeKind::Array) {
            fields.push_back(Fail(ck, *ordered[i],
                                  "array fields start zeroed; leave the struct literal out and "
                                  "fill " +
                                      sd.fields[i].name + " with a for loop"));
            continue;
        }
        ExprId value = CheckExpr(ck, *ordered[i], field_type);
        RequireType(ck, *ordered[i], value, field_type,
                    "field " + sd.fields[i].name + " of " + sd.name);
        if (Kind(ck, field_type) == TypeKind::Struct) {
            RequireStructSource(ck, *ordered[i], value);
        }
        fields.push_back(value);
    }
    ExprId id = AddExpr(ck, ExprKind::StructLit, type, node);
    ExprAt(ck, id).first = AddList(ck, fields);
    ExprAt(ck, id).count = static_cast<std::uint32_t>(fields.size());
    return id;
}

ExprId CheckMake(Checker& ck, const Node& node) {
    if (node.items.size() < 2) {
        return Fail(ck, node,
                    "make takes a type and its values; write (make Sprite :x 1 :y 2) or "
                    "(make (array u8 3) 1 2 3)");
    }
    TypeId type = ResolveType(ck, node.items[1]);
    if (type == kErrorType) {
        return ErrorExpr(ck, node);
    }
    TypeKind kind = Kind(ck, type);
    if (kind == TypeKind::Struct) {
        return CheckStructLit(ck, node, type);
    }
    if (kind == TypeKind::Array) {
        return Fail(ck, node,
                    "an array value only starts a const or a top-level let; fill this array "
                    "element by element with a for loop");
    }
    return Fail(ck, node,
                "make builds structs and arrays; a " + ShowType(ck, type) +
                    " is written as the value itself");
}

bool FoldArrayLiteral(Checker& ck, const Node& value, TypeId type, const std::string& name,
                      std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    std::string type_text = ShowType(ck, type);
    if (HeadName(value) != "make" || value.items.size() < 2) {
        Err(ck, value,
            name + " is " + type_text + ", so its value is " + ArrayLiteralFix(type_text));
        return false;
    }
    TypeId made = ResolveType(ck, value.items[1]);
    if (made == kErrorType) {
        return false;
    }
    if (made != type) {
        Err(ck, value.items[1],
            "the value is " + ShowType(ck, made) + " but " + name + " is " + type_text +
                "; write " + ArrayLiteralFix(type_text));
        return false;
    }
    const Type array = TypeOf(*ck.out, type);
    std::size_t given = value.items.size() - 2;
    if (given != array.count) {
        Err(ck, value,
            name + " is " + type_text + " but its value has " + std::to_string(given) +
                " values; give exactly " + std::to_string(array.count));
        return false;
    }
    TypeId elem = array.elem;
    std::uint32_t elem_size = TypeOf(*ck.out, elem).size;
    bool ok = true;
    for (std::size_t i = 0; i < given; i++) {
        const Node& item = value.items[2 + i];
        std::uint32_t at = offset + static_cast<std::uint32_t>(i) * elem_size;
        std::string what = "an element of " + name;
        if (Kind(ck, elem) == TypeKind::Array) {
            ok = FoldArrayLiteral(ck, item, elem, what, bytes, at) && ok;
            continue;
        }
        ExprId element = CheckExpr(ck, item, elem);
        if (!RequireType(ck, item, element, elem, what)) {
            ok = false;
            continue;
        }
        if (Kind(ck, elem) == TypeKind::Struct && !RequireStructSource(ck, item, element)) {
            ok = false;
            continue;
        }
        std::size_t before = ck.errors->size();
        if (!FoldInit(ck, element, bytes, at)) {
            if (ck.errors->size() == before) {
                Err(ck, item,
                    Show(item) + " is not constant; " + what +
                        " is built from numbers, other constants and operators");
            }
            ok = false;
        }
    }
    return ok;
}

} // namespace tisple
