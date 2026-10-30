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

std::uint16_t FunctionValue(const Layout& layout, std::int32_t function) {
    std::size_t f = static_cast<std::size_t>(function);
    if (f < layout.functions.size() && layout.functions[f] != 0) {
        return layout.functions[f];
    }
    return static_cast<std::uint16_t>(kFunctionValueBase + function);
}

std::uint16_t SlotValue(const Layout& layout, const AddrSlot& slot) {
    if (slot.function >= 0) {
        return FunctionValue(layout, slot.function);
    }
    const VarPlace& place = layout.vars[static_cast<std::size_t>(slot.var)];
    return static_cast<std::uint16_t>((place.address + slot.at) & 0xFFFF);
}

std::int32_t FunctionOfValue(const Program& program, const Layout& layout, std::uint16_t value) {
    if (value == 0) {
        return -1;
    }
    for (std::size_t f = 0; f < program.functions.size(); f++) {
        std::int32_t function = static_cast<std::int32_t>(f);
        if (program.functions[f].address_taken && FunctionValue(layout, function) == value) {
            return function;
        }
    }
    return -1;
}

std::vector<std::uint8_t> InitBytes(const Program& program, const Layout& layout,
                                    std::int32_t var) {
    const Var& v = program.vars[static_cast<std::size_t>(var)];
    std::vector<std::uint8_t> bytes = v.init;
    for (const AddrSlot& slot : v.addr_slots) {
        std::uint16_t value = SlotValue(layout, slot);
        bytes[slot.offset] = static_cast<std::uint8_t>(value & 0xFF);
        bytes[slot.offset + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    return bytes;
}

void WriteVarInit(const Program& program, const Layout& layout, std::int32_t var,
                  std::vector<std::uint8_t>& memory) {
    const Var& v = program.vars[static_cast<std::size_t>(var)];
    if (v.init.empty() || v.type == kErrorType ||
        layout.vars[static_cast<std::size_t>(var)].dropped) {
        return;
    }
    if (v.addr_slots.empty()) {
        WriteValue(program, layout, VarCursor(layout, var), var, v.type, v.init, 0, memory);
        return;
    }
    std::vector<std::uint8_t> bytes = InitBytes(program, layout, var);
    WriteValue(program, layout, VarCursor(layout, var), var, v.type, bytes, 0, memory);
}

} // namespace tisple
