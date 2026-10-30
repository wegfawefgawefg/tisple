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

} // namespace

bool OrderStructFields(Checker& ck, const Node& node, TypeId type,
                       std::vector<const Node*>& ordered) {
    const StructDecl sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    if (sd.fields.empty()) {
        return false;
    }
    std::vector<std::string> names;
    std::vector<const Node*> values;
    for (std::size_t i = 2; i < node.items.size(); i += 2) {
        const Node& key = node.items[i];
        if (key.kind != NodeKind::Keyword) {
            Err(ck, key,
                "struct literals name every field: write (make " + sd.name + " :" +
                    sd.fields[0].name + " value ...)");
            return false;
        }
        if (i + 1 >= node.items.size()) {
            Err(ck, key, key.text + " has no value; write " + key.text + " value");
            return false;
        }
        std::string name = key.text.substr(1);
        std::int32_t field = FindFieldIndex(*ck.out, type, name);
        if (field < 0) {
            Err(ck, key, sd.name + " has no field " + name + "; its fields are " + FieldList(sd));
            return false;
        }
        Note(ck, key, 0, NameUseKind::Field, TypeOf(*ck.out, type).decl, field, false);
        for (const std::string& other : names) {
            if (other == name) {
                Err(ck, key, key.text + " is given twice; give each field once");
                return false;
            }
        }
        names.push_back(name);
        values.push_back(&node.items[i + 1]);
    }

    ordered.assign(sd.fields.size(), nullptr);
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
        Err(ck, node,
            Show(node) + " is missing " + missing + "; name every field in order: " + fix);
        return false;
    }
    bool in_order = true;
    for (std::size_t i = 0; i < names.size(); i++) {
        in_order = in_order && names[i] == sd.fields[i].name;
    }
    if (!in_order) {
        Err(ck, node, Show(node) + " fields out of order; write " + fix);
        return false;
    }
    return true;
}

ExprId CheckStructLit(Checker& ck, const Node& node, TypeId type) {
    const StructDecl sd = ck.out->structs[static_cast<std::size_t>(TypeOf(*ck.out, type).decl)];
    std::vector<const Node*> ordered;
    if (!OrderStructFields(ck, node, type, ordered)) {
        return ErrorExpr(ck, node);
    }
    std::vector<ExprId> fields;
    for (std::size_t i = 0; i < sd.fields.size(); i++) {
        TypeId field_type = sd.fields[i].type;
        if (Kind(ck, field_type) == TypeKind::Array) {
            fields.push_back(Fail(ck, *ordered[i],
                                  "an array value only starts a const or a top-level let; leave "
                                  "the struct literal out and fill " +
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

} // namespace tisple
