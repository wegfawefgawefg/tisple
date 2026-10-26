#include "link/image.hpp"

namespace tisple {

namespace {

void WriteValue(const Program& program, const Layout& layout, Cursor dst, std::int32_t root,
                TypeId type, const std::vector<std::uint8_t>& bytes, std::uint32_t offset,
                std::vector<std::uint8_t>& memory) {
    const Type& t = TypeOf(program, type);
    if (IsScalarKind(t.kind)) {
        for (std::uint32_t k = 0; k < t.size; k++) {
            memory[CursorByte(dst, k)] = bytes[offset + k];
        }
        return;
    }
    if (t.kind == TypeKind::Struct) {
        for (const Field& field : program.structs[static_cast<std::size_t>(t.decl)].fields) {
            WriteValue(program, layout, FieldCursor(dst, field.offset), root, field.type, bytes,
                       offset + field.offset, memory);
        }
        return;
    }
    std::uint32_t elem_size = TypeOf(program, t.elem).size;
    for (std::uint32_t i = 0; i < t.count; i++) {
        Cursor element = ElementCursor(program, layout, root, dst, type, i);
        WriteValue(program, layout, element, root, t.elem, bytes, offset + i * elem_size, memory);
    }
}

} // namespace

void WriteVarInit(const Program& program, const Layout& layout, std::int32_t var,
                  std::vector<std::uint8_t>& memory) {
    const Var& v = program.vars[static_cast<std::size_t>(var)];
    if (v.init.empty() || v.type == kErrorType) {
        return;
    }
    WriteValue(program, layout, VarCursor(layout, var), var, v.type, v.init, 0, memory);
}

} // namespace tisple
