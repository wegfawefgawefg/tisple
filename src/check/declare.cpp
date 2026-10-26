#include "check/checker.hpp"
#include "check/names.hpp"

namespace tisple {

// ---- types

TypeId InternType(Checker& ck, const Type& type) {
    auto key = std::make_tuple(static_cast<int>(type.kind), static_cast<int>(type.elem), type.count,
                               static_cast<int>(type.decl));
    auto it = ck.interned.find(key);
    if (it != ck.interned.end()) {
        return it->second;
    }
    TypeId id = static_cast<TypeId>(ck.out->types.size());
    ck.out->types.push_back(type);
    ck.interned.emplace(key, id);
    return id;
}

TypeId PtrType(Checker& ck, TypeId elem) {
    if (elem == kErrorType) {
        return kErrorType;
    }
    Type type;
    type.kind = TypeKind::Ptr;
    type.elem = elem;
    type.size = 2;
    return InternType(ck, type);
}

TypeId StructType(Checker& ck, std::int32_t decl) {
    Type type;
    type.kind = TypeKind::Struct;
    type.decl = decl;
    type.size = ck.out->structs[static_cast<std::size_t>(decl)].size;
    return InternType(ck, type);
}

TypeId EnumType(Checker& ck, std::int32_t decl) {
    Type type;
    type.kind = TypeKind::Enum;
    type.decl = decl;
    type.size = 1;
    return InternType(ck, type);
}

TypeId ArrayOf(Checker& ck, TypeId elem, std::uint32_t count, const Node& at) {
    if (elem == kErrorType) {
        return kErrorType;
    }
    const Type& elem_type = TypeOf(*ck.out, elem);
    if (elem_type.kind == TypeKind::Struct && !EnsureStruct(ck, elem_type.decl, at)) {
        return kErrorType;
    }
    std::uint64_t size = static_cast<std::uint64_t>(TypeOf(*ck.out, elem).size) * count;
    if (size > 0x10000) {
        Err(ck, at, Show(at) + " is " + std::to_string(size) + " bytes; it does not fit in 64 KB");
        return kErrorType;
    }
    Type type;
    type.kind = TypeKind::Array;
    type.elem = elem;
    type.count = count;
    type.size = static_cast<std::uint32_t>(size);
    return InternType(ck, type);
}

namespace {

TypeId PrimitiveType(const std::string& text) {
    if (text == "u8") {
        return kU8Type;
    }
    if (text == "i8") {
        return kI8Type;
    }
    if (text == "u16") {
        return kU16Type;
    }
    if (text == "i16") {
        return kI16Type;
    }
    if (text == "bool") {
        return kBoolType;
    }
    return -1;
}

const char* kTypeHelp =
    "types are u8 i8 u16 i16 bool, (ptr t), (array t n), or a struct or enum name";

} // namespace

TypeId ResolveType(Checker& ck, const Node& node) {
    if (node.kind == NodeKind::Symbol) {
        TypeId primitive = PrimitiveType(node.text);
        if (primitive >= 0) {
            return primitive;
        }
        if (node.text == "ptr" || node.text == "array") {
            Err(ck, node, node.text + " needs parentheses; write (ptr u8) or (array u8 16)");
            return kErrorType;
        }
        std::vector<std::string> segments = SplitDotted(node.text);
        NameRef ref = segments.empty() ? NameRef() : LookupName(ck, segments, node);
        if (ref.kind == NameKind::Struct && ref.used == segments.size()) {
            return StructType(ck, ref.index);
        }
        if (ref.kind == NameKind::Enum && ref.used == segments.size()) {
            return EnumType(ck, ref.index);
        }
        Err(ck, node, "unknown type " + node.text + "; " + kTypeHelp);
        return kErrorType;
    }
    const std::string& head = HeadName(node);
    if (head == "ptr") {
        if (node.items.size() != 2) {
            Err(ck, node, "(ptr t) takes one type; write (ptr u8)");
            return kErrorType;
        }
        return PtrType(ck, ResolveType(ck, node.items[1]));
    }
    if (head == "array") {
        if (node.items.size() != 3) {
            Err(ck, node, "(array t n) takes a type and a length; write (array u8 16)");
            return kErrorType;
        }
        TypeId elem = ResolveType(ck, node.items[1]);
        std::uint32_t count = 0;
        if (!ConstCount(ck, node.items[2], count)) {
            return kErrorType;
        }
        return ArrayOf(ck, elem, count, node);
    }
    Err(ck, node, Show(node) + " is not a type; " + kTypeHelp);
    return kErrorType;
}

TypeId TryResolveType(Checker& ck, const Node& node) {
    if (node.kind == NodeKind::Symbol) {
        if (PrimitiveType(node.text) >= 0) {
            return ResolveType(ck, node);
        }
        std::vector<std::string> segments = SplitDotted(node.text);
        if (segments.empty()) {
            return -1;
        }
        NameRef ref = LookupName(ck, segments, node);
        bool whole = ref.used == segments.size();
        if (whole && (ref.kind == NameKind::Struct || ref.kind == NameKind::Enum)) {
            return ResolveType(ck, node);
        }
        return -1;
    }
    const std::string& head = HeadName(node);
    if (head == "ptr" || head == "array") {
        return ResolveType(ck, node);
    }
    return -1;
}

bool EnsureStruct(Checker& ck, std::int32_t decl, const Node& at) {
    std::size_t index = static_cast<std::size_t>(decl);
    Progress state = ck.struct_states[index];
    if (state == Progress::Done) {
        return true;
    }
    if (state == Progress::Failed) {
        return false;
    }
    StructDecl& sd = ck.out->structs[index];
    if (state == Progress::Busy) {
        Err(ck, at, "struct " + sd.name + " contains itself; use a (ptr " + sd.name + ") field");
        ck.struct_states[index] = Progress::Failed;
        return false;
    }
    ck.struct_states[index] = Progress::Busy;
    Context saved = SwitchContext(ck, sd.module, -1);
    const Node& form = *ck.struct_forms[index];
    std::vector<Field> fields;
    std::uint32_t offset = 0;
    bool ok = true;
    if (form.items.size() < 3) {
        Err(ck, form, "a struct needs fields; write (struct " + sd.name + " (x u8) (y u8))");
        ok = false;
    }
    for (std::size_t i = 2; i < form.items.size(); i++) {
        const Node& item = form.items[i];
        if (item.kind != NodeKind::List || item.items.size() != 2 ||
            item.items[0].kind != NodeKind::Symbol) {
            Err(ck, item, "each field is (name type); write (x u8)");
            ok = false;
            continue;
        }
        const Node& name = item.items[0];
        bool repeated = false;
        for (const Field& field : fields) {
            repeated = repeated || field.name == name.text;
        }
        if (repeated) {
            Err(ck, name, "field " + name.text + " appears twice in " + sd.name + "; rename one");
            ok = false;
            continue;
        }
        ok = CheckNewName(ck, name, NameRule::Snake, "field") && ok;
        TypeId type = ResolveType(ck, item.items[1]);
        const Type& field_type = TypeOf(*ck.out, type);
        if (field_type.kind == TypeKind::Struct && !EnsureStruct(ck, field_type.decl, item)) {
            type = kErrorType;
        }
        ok = ok && type != kErrorType;
        fields.push_back(Field{name.text, type, offset});
        offset += TypeOf(*ck.out, type).size;
    }
    RestoreContext(ck, std::move(saved));
    StructDecl& done = ck.out->structs[index];
    done.fields = fields;
    done.size = offset;
    TypeId self = StructType(ck, decl);
    ck.out->types[static_cast<std::size_t>(self)].size = offset;
    ck.struct_states[index] = ok ? Progress::Done : Progress::Failed;
    return ok;
}

// ---- names

std::vector<std::string> SplitDotted(const std::string& text) {
    std::vector<std::string> segments;
    std::size_t start = 0;
    while (true) {
        std::size_t dot = text.find('.', start);
        std::string segment = text.substr(start, dot == std::string::npos ? dot : dot - start);
        if (segment.empty()) {
            return {};
        }
        segments.push_back(segment);
        if (dot == std::string::npos) {
            return segments;
        }
        start = dot + 1;
    }
}

namespace {

void FromDecl(Checker& ck, const Decl& decl, const std::vector<std::string>& segments,
              std::size_t used, NameRef& ref) {
    ref.index = decl.index;
    ref.used = used;
    switch (decl.kind) {
    case DeclKind::Struct:
        ref.kind = NameKind::Struct;
        return;
    case DeclKind::Const:
        ref.kind = NameKind::Const;
        return;
    case DeclKind::Var:
        ref.kind = NameKind::Var;
        return;
    case DeclKind::Function:
        ref.kind = NameKind::Function;
        return;
    case DeclKind::Macro:
        ref.kind = NameKind::None;
        ref.missing_in = "";
        return;
    case DeclKind::Enum:
        break;
    }
    ref.kind = NameKind::Enum;
    if (segments.size() <= used) {
        return;
    }
    const EnumDecl& ed = ck.out->enums[static_cast<std::size_t>(decl.index)];
    for (const EnumMember& member : ed.members) {
        if (member.name == segments[used]) {
            ref.kind = NameKind::EnumMember;
            ref.value = member.value;
            ref.used = used + 1;
            return;
        }
    }
    ref.kind = NameKind::None;
    ref.missing_in = ed.name;
}

} // namespace

std::int32_t NameModule(const Checker& ck, const Node& at) {
    if (at.expansion == 0) {
        return ck.at.module;
    }
    std::int32_t module = ck.in->sources.expansions[at.expansion - 1].module;
    return module >= 0 ? module : ck.at.module;
}

NameRef LookupName(Checker& ck, const std::vector<std::string>& segments, const Node& at) {
    NameRef ref;
    const std::string& first = segments[0];
    for (auto scope = ck.at.scopes.rbegin(); scope != ck.at.scopes.rend(); ++scope) {
        for (auto entry = scope->entries.rbegin(); entry != scope->entries.rend(); ++entry) {
            if (entry->name == first) {
                ref.kind = NameKind::Var;
                ref.index = entry->var;
                ref.used = 1;
                return ref;
            }
        }
    }
    const ModuleScope& scope = ck.modules[static_cast<std::size_t>(NameModule(ck, at))];
    auto decl = scope.names.find(first);
    if (decl != scope.names.end()) {
        FromDecl(ck, decl->second, segments, 1, ref);
        return ref;
    }
    auto import = scope.imports.find(first);
    if (import == scope.imports.end()) {
        return ref;
    }
    if (segments.size() == 1) {
        ref.kind = NameKind::Module;
        ref.index = import->second;
        ref.used = 1;
        return ref;
    }
    const ModuleScope& target = ck.modules[static_cast<std::size_t>(import->second)];
    auto member = target.names.find(segments[1]);
    if (member == target.names.end()) {
        ref.used = 1;
        ref.missing_in = first;
        return ref;
    }
    FromDecl(ck, member->second, segments, 2, ref);
    return ref;
}

bool CheckNewName(Checker& ck, const Node& name, NameRule rule, const char* what) {
    if (name.kind != NodeKind::Symbol) {
        Err(ck, name, std::string(what) + " needs a name here, not " + Show(name));
        return false;
    }
    const std::string& text = name.text;
    if (IsBuiltinName(text)) {
        Err(ck, name, text + " is a built-in name; rename this " + what);
        return false;
    }
    if (text.find('.') != std::string::npos) {
        std::string fixed = text;
        for (char& c : fixed) {
            c = c == '.' ? '_' : c;
        }
        Err(ck, name, text + ": a new name has no dots; write " + fixed);
        return false;
    }
    bool fresh = name.expansion != 0 && IsFreshName(text);
    if (rule == NameRule::Snake && !IsSnakeName(text) && !fresh) {
        Err(ck, name,
            std::string(what) + " " + text + ": names are snake_case; write " + ToSnakeName(text));
        return false;
    }
    if (rule == NameRule::Type && !IsTypeName(text)) {
        Err(ck, name,
            std::string(what) + " " + text + ": type names are PascalCase; write " +
                ToTypeName(text));
        return false;
    }
    if (rule == NameRule::Constant && !IsConstantName(text)) {
        Err(ck, name,
            std::string(what) + " " + text + ": constant names are SCREAMING_SNAKE_CASE; write " +
                ToConstantName(text));
        return false;
    }
    const ModuleScope& scope = ck.modules[static_cast<std::size_t>(ck.at.module)];
    if (scope.imports.count(text) != 0) {
        Err(ck, name,
            text + " is an imported module here, so " + text +
                ".x would be ambiguous; rename this " + what);
        return false;
    }
    return true;
}

bool CheckLocalName(Checker& ck, const Node& name, const char* what) {
    if (!CheckNewName(ck, name, NameRule::Snake, what)) {
        return false;
    }
    for (const ScopeEntry& entry : ck.at.scopes.back().entries) {
        if (entry.name == name.text) {
            const Var& var = ck.out->vars[static_cast<std::size_t>(entry.var)];
            Err(ck, name,
                name.text + " is already declared in this body (" +
                    FormatLoc(ck.in->sources, var.loc) + "); pick another name");
            return false;
        }
    }
    return true;
}

} // namespace tisple
