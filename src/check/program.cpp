#include "check/program.hpp"

#include "check/arith.hpp"

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
           kind == TypeKind::Ptr || kind == TypeKind::Fn;
}

std::uint32_t KindBits(TypeKind kind) {
    if (kind == TypeKind::U16 || kind == TypeKind::I16 || kind == TypeKind::Ptr ||
        kind == TypeKind::Fn) {
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
    case TypeKind::Fn: {
        const Signature& sig = program.signatures[static_cast<std::size_t>(type.decl)];
        std::string params;
        for (TypeId param : sig.params) {
            params += (params.empty() ? "" : " ") + TypeName(program, param);
        }
        std::string ret = sig.ret == kVoidType ? "void" : TypeName(program, sig.ret);
        return "(fn (" + params + ") " + ret + ")";
    }
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

bool ConstValue(const Program& program, ExprId id, std::uint16_t& value) {
    const Expr& e = program.exprs[static_cast<std::size_t>(id)];
    std::uint16_t a = 0;
    std::uint16_t b = 0;
    switch (e.kind) {
    case ExprKind::Literal:
        value = static_cast<std::uint16_t>(e.value);
        return true;
    case ExprKind::Unary:
        if (!ConstValue(program, e.a, a)) {
            return false;
        }
        value = EvalUnaryOp(e.op, KindOf(program, e.type), a);
        return true;
    case ExprKind::Binary: {
        if (!ConstValue(program, e.a, a) || !ConstValue(program, e.b, b)) {
            return false;
        }
        bool div_by_zero = false;
        value =
            EvalBinaryOp(e.op, KindOf(program, program.exprs[static_cast<std::size_t>(e.a)].type),
                         a, b, div_by_zero);
        return !div_by_zero;
    }
    case ExprKind::Convert:
        if (!ConstValue(program, e.a, a)) {
            return false;
        }
        value = ConvertBits(KindOf(program, program.exprs[static_cast<std::size_t>(e.a)].type),
                            KindOf(program, e.type), a);
        return true;
    case ExprKind::Reinterpret:
        return ConstValue(program, e.a, value);
    case ExprKind::And:
    case ExprKind::Or: {
        if (!ConstValue(program, e.a, a)) {
            return false;
        }
        // the short circuit decides without b
        if ((e.kind == ExprKind::And && a == 0) || (e.kind == ExprKind::Or && a != 0)) {
            value = e.kind == ExprKind::Or ? 1 : 0;
            return true;
        }
        if (!ConstValue(program, e.b, b)) {
            return false;
        }
        value = b != 0 ? 1 : 0;
        return true;
    }
    case ExprKind::PtrAdd:
    case ExprKind::PtrSub: {
        if (!ConstValue(program, e.a, a) || !ConstValue(program, e.b, b)) {
            return false;
        }
        std::uint32_t bytes = static_cast<std::uint32_t>(b) * e.value;
        std::uint32_t sum = e.kind == ExprKind::PtrAdd ? a + bytes : a - bytes;
        value = static_cast<std::uint16_t>(sum & 0xFFFF);
        return true;
    }
    case ExprKind::Index:
    case ExprKind::Field: {
        std::int32_t var = -1;
        std::uint32_t offset = 0;
        const Type& type = TypeOf(program, e.type);
        if (!IsScalarKind(type.kind) || !ConstPlace(program, id, var, offset)) {
            return false;
        }
        return InitBits(program.vars[static_cast<std::size_t>(var)], offset, type.size, value);
    }
    default:
        return false;
    }
}

bool ConstPlace(const Program& program, ExprId place, std::int32_t& var, std::uint32_t& offset) {
    const Expr& e = program.exprs[static_cast<std::size_t>(place)];
    std::uint16_t index = 0;
    switch (e.kind) {
    case ExprKind::VarRef:
        var = e.ref;
        offset = 0;
        return program.vars[static_cast<std::size_t>(e.ref)].kind == VarKind::Const;
    case ExprKind::Field:
        if (!ConstPlace(program, e.a, var, offset)) {
            return false;
        }
        offset += e.value;
        return true;
    case ExprKind::Index: {
        const Type& array = TypeOf(program, program.exprs[static_cast<std::size_t>(e.a)].type);
        if (array.kind != TypeKind::Array || !ConstPlace(program, e.a, var, offset) ||
            !ConstValue(program, e.b, index) || index >= array.count) {
            return false;
        }
        offset += static_cast<std::uint32_t>(index) * TypeOf(program, array.elem).size;
        return true;
    }
    default:
        return false;
    }
}

const AddrSlot* SlotOverlapping(const Var& var, std::uint32_t offset, std::uint32_t size) {
    for (const AddrSlot& slot : var.addr_slots) {
        if (slot.offset < offset + size && offset < slot.offset + 2) {
            return &slot;
        }
    }
    return nullptr;
}

bool InitBits(const Var& var, std::uint32_t offset, std::uint32_t size, std::uint16_t& bits) {
    if (SlotOverlapping(var, offset, size)) {
        return false;
    }
    bits = 0;
    for (std::uint32_t i = 0; i < size; i++) {
        std::size_t at = static_cast<std::size_t>(offset + i);
        std::uint16_t byte = at < var.init.size() ? var.init[at] : 0;
        bits = static_cast<std::uint16_t>(bits | (byte << (8 * i)));
    }
    return true;
}

std::vector<std::int32_t> CallTargets(const Program& program, TypeId type) {
    std::vector<std::int32_t> out;
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        const Function& fn = program.functions[f];
        if (fn.address_taken && fn.type == type) {
            out.push_back(static_cast<std::int32_t>(f));
        }
    }
    return out;
}

} // namespace tisple
