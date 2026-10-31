// The interpreter's memory: main memory and aux memory images, values moved through cursors,
// and the starting bytes (interp.hpp).

#include "interp/interp_internal.hpp"
#include "link/image.hpp"

namespace tisple::interp {

// ---- values as packed bytes, moved through cursors field by field

void ReadValue(const Interp& in, Cursor src, TypeId type, std::vector<std::uint8_t>& bytes,
               std::uint32_t offset) {
    const Type& t = TypeAt(in, type);
    if (IsScalarKind(t.kind)) {
        std::uint16_t value = ReadScalar(in, src, t.size);
        bytes[offset] = static_cast<std::uint8_t>(value & 0xFF);
        if (t.size == 2) {
            bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        }
        return;
    }
    if (t.kind == TypeKind::Struct) {
        for (const Field& field : in.program->structs[static_cast<std::size_t>(t.decl)].fields) {
            ReadValue(in, FieldCursor(src, field.offset), field.type, bytes, offset + field.offset);
        }
        return;
    }
    std::uint32_t elem_size = TypeAt(in, t.elem).size;
    for (std::uint32_t i = 0; i < t.count; i++) {
        Cursor element = ElementCursor(*in.program, *in.layout, -1, src, type, i);
        ReadValue(in, element, t.elem, bytes, offset + i * elem_size);
    }
}

void WriteValue(Interp& in, Cursor dst, std::int32_t root, TypeId type,
                const std::vector<std::uint8_t>& bytes, std::uint32_t offset) {
    const Type& t = TypeAt(in, type);
    if (IsScalarKind(t.kind)) {
        std::uint16_t value = bytes[offset];
        if (t.size == 2) {
            value = static_cast<std::uint16_t>(value | (bytes[offset + 1] << 8));
        }
        WriteScalar(in, dst, t.size, value);
        return;
    }
    if (t.kind == TypeKind::Struct) {
        for (const Field& field : in.program->structs[static_cast<std::size_t>(t.decl)].fields) {
            WriteValue(in, FieldCursor(dst, field.offset), root, field.type, bytes,
                       offset + field.offset);
        }
        return;
    }
    std::uint32_t elem_size = TypeAt(in, t.elem).size;
    for (std::uint32_t i = 0; i < t.count; i++) {
        Cursor element = ElementCursor(*in.program, *in.layout, root, dst, type, i);
        WriteValue(in, element, root, t.elem, bytes, offset + i * elem_size);
    }
}

} // namespace tisple::interp

namespace tisple {

using interp::WriteValue;

namespace {

// a byte's memory: aux memory's for a pointer access of $0200-$BFFF with its switch on, and
// for a const :in aux read by name (the checker allows that only where RAMRD is on, or where
// the read folds to its value)
bool InAux(const Interp& in, const Cursor& cursor, std::uint16_t address, bool write) {
    if (cursor.aux && !in.aux.empty()) {
        return true;
    }
    bool on = write ? in.aux_writes : in.aux_reads;
    return on && cursor.pointer && address >= 0x0200 && address < 0xC000 && !in.aux.empty();
}

std::uint8_t ReadByte(const Interp& in, const Cursor& cursor, std::uint16_t address) {
    return InAux(in, cursor, address, false) ? in.aux[address] : in.memory[address];
}

void WriteByte(Interp& in, const Cursor& cursor, std::uint16_t address, std::uint8_t value) {
    (InAux(in, cursor, address, true) ? in.aux : in.memory)[address] = value;
}

} // namespace

std::uint16_t ReadScalar(const Interp& in, Cursor cursor, std::uint32_t size) {
    std::uint16_t low = ReadByte(in, cursor, cursor.address);
    if (size == 1) {
        return low;
    }
    std::uint16_t high = ReadByte(in, cursor, CursorByte(cursor, 1));
    return static_cast<std::uint16_t>(low | (high << 8));
}

void WriteScalar(Interp& in, Cursor cursor, std::uint32_t size, std::uint16_t value) {
    WriteByte(in, cursor, cursor.address, static_cast<std::uint8_t>(value & 0xFF));
    if (size == 2) {
        WriteByte(in, cursor, CursorByte(cursor, 1), static_cast<std::uint8_t>(value >> 8));
    }
}

void InitInterp(Interp& in, const Program& program, const Layout& layout) {
    in.program = &program;
    in.layout = &layout;
    in.memory.assign(0x10000, 0);
    in.aux.assign(0x10000, 0);
    in.aux_reads = false;
    in.aux_writes = false;
    in.left_out.assign(program.vars.size(), {});
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        const Var& var = program.vars[i];
        std::int32_t id = static_cast<std::int32_t>(i);
        if (var.kind == VarKind::Const && layout.vars[i].dropped) {
            in.left_out[i] = InitBytes(program, layout, id);
            in.left_out[i].resize(TypeOf(program, var.type).size, 0);
            continue;
        }
        if (var.init.empty() || var.type == kErrorType || layout.vars[i].dropped) {
            continue;
        }
        // aux memory: the :in aux lets, and the const data the link copies there
        bool mirrored = i < layout.mirrored.size() && layout.mirrored[i];
        if (var.placement.in_aux || mirrored) {
            WriteVarInit(program, layout, id, in.aux);
        }
        if (var.placement.in_aux) {
            continue;
        }
        if (var.addr_slots.empty()) {
            WriteValue(in, VarCursor(layout, id), id, var.type, var.init, 0);
        } else {
            WriteValue(in, VarCursor(layout, id), id, var.type, InitBytes(program, layout, id), 0);
        }
    }
}

} // namespace tisple
