#include "check/program.hpp"

namespace tisple {

bool IsIntegerKind(TypeKind kind) {
    return kind == TypeKind::U8 || kind == TypeKind::I8 || kind == TypeKind::U16 ||
           kind == TypeKind::I16;
}

bool IsSignedKind(TypeKind kind) {
    return kind == TypeKind::I8 || kind == TypeKind::I16;
}

bool IsScalarKind(TypeKind kind) {
    return IsIntegerKind(kind) || kind == TypeKind::Bool || kind == TypeKind::Enum ||
           kind == TypeKind::Ptr;
}

std::uint32_t KindBits(TypeKind kind) {
    if (kind == TypeKind::U16 || kind == TypeKind::I16 || kind == TypeKind::Ptr) {
        return 16;
    }
    return 8;
}

const Type& TypeOf(const Program& program, TypeId id) {
    return program.types[static_cast<std::size_t>(id)];
}

TypeKind KindOf(const Program& program, TypeId id) {
    return TypeOf(program, id).kind;
}

std::string TypeName(const Program& program, TypeId id) {
    if (id < 0 || static_cast<std::size_t>(id) >= program.types.size()) {
        return "?";
    }
    const Type& type = TypeOf(program, id);
    switch (type.kind) {
    case TypeKind::Error:
        return "?";
    case TypeKind::Void:
        return "nothing";
    case TypeKind::Never:
        return "never";
    case TypeKind::Bool:
        return "bool";
    case TypeKind::U8:
        return "u8";
    case TypeKind::I8:
        return "i8";
    case TypeKind::U16:
        return "u16";
    case TypeKind::I16:
        return "i16";
    case TypeKind::Ptr:
        return "(ptr " + TypeName(program, type.elem) + ")";
    case TypeKind::Array:
        return "(array " + TypeName(program, type.elem) + " " + std::to_string(type.count) + ")";
    case TypeKind::Struct:
        return program.structs[static_cast<std::size_t>(type.decl)].name;
    case TypeKind::Enum:
        return program.enums[static_cast<std::size_t>(type.decl)].name;
    }
    return "?";
}

std::int32_t FindFunction(const Program& program, const std::string& qualified) {
    for (std::size_t i = 0; i < program.functions.size(); i++) {
        if (program.functions[i].name == qualified) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::int32_t FindVar(const Program& program, const std::string& qualified) {
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        const Var& var = program.vars[i];
        if (var.function < 0 && var.name == qualified) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

std::int32_t FindFieldIndex(const Program& program, TypeId struct_type, const std::string& name) {
    const Type& type = TypeOf(program, struct_type);
    if (type.kind != TypeKind::Struct) {
        return -1;
    }
    const StructDecl& decl = program.structs[static_cast<std::size_t>(type.decl)];
    for (std::size_t i = 0; i < decl.fields.size(); i++) {
        if (decl.fields[i].name == name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return -1;
}

} // namespace tisple
