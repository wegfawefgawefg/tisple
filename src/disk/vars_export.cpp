#include "disk/vars_export.hpp"

#include "link/layout.hpp"

#include <cstdio>
#include <filesystem>

namespace tisple {

namespace {

bool IsLeafKind(TypeKind kind) {
    return kind == TypeKind::Bool || kind == TypeKind::U8 || kind == TypeKind::I8 ||
           kind == TypeKind::U16 || kind == TypeKind::I16 || kind == TypeKind::Enum ||
           kind == TypeKind::Ptr || kind == TypeKind::Fn;
}

// one entry: a scalar at cursor first, count elements apart by step (count 1: a scalar)
void AddEntry(std::string& out, bool& first, const std::string& name, const Program& program,
              TypeId type, Cursor at, std::uint32_t count, std::uint32_t step, bool aux) {
    TypeKind kind = KindOf(program, type);
    std::uint32_t bytes = KindBits(kind) / 8;
    char line[256];
    std::snprintf(line, sizeof(line), "%s\n  \"%s\": {\"at\": %u, \"bytes\": %u", first ? "" : ",",
                  name.c_str(), at.address, bytes);
    out += line;
    first = false;
    if (bytes == 2) {
        out += ", \"high\": " +
               std::to_string(static_cast<std::uint16_t>(CursorByte(at, 1) - at.address));
    }
    if (count > 1) {
        out += ", \"count\": " + std::to_string(count) + ", \"step\": " + std::to_string(step);
    }
    if (kind == TypeKind::I8 || kind == TypeKind::I16) {
        out += ", \"signed\": true";
    }
    if (aux) {
        out += ", \"aux\": true";
    }
    out += "}";
}

void AddVar(std::string& out, bool& first, const Program& program, const Layout& layout,
            std::int32_t v) {
    const Var& var = program.vars[static_cast<std::size_t>(v)];
    bool aux = var.placement.in_aux;
    Cursor base = VarCursor(layout, v);
    const Type& type = TypeOf(program, var.type);
    if (IsLeafKind(type.kind)) {
        AddEntry(out, first, var.name, program, var.type, base, 1, 0, aux);
        return;
    }
    if (type.kind == TypeKind::Struct) {
        for (const Field& f : program.structs[static_cast<std::size_t>(type.decl)].fields) {
            if (IsLeafKind(KindOf(program, f.type))) {
                AddEntry(out, first, var.name + "." + f.name, program, f.type,
                         FieldCursor(base, f.offset), 1, 0, aux);
            }
        }
        return;
    }
    if (type.kind != TypeKind::Array || type.count == 0) {
        return;
    }
    Cursor e0 = ElementCursor(program, layout, v, base, var.type, 0);
    std::uint32_t step = 0;
    if (type.count > 1) {
        Cursor e1 = ElementCursor(program, layout, v, base, var.type, 1);
        step = static_cast<std::uint16_t>(e1.address - e0.address);
    }
    const Type& elem = TypeOf(program, type.elem);
    if (IsLeafKind(elem.kind)) {
        AddEntry(out, first, var.name, program, type.elem, e0, type.count, step, aux);
        return;
    }
    if (elem.kind != TypeKind::Struct) {
        return;
    }
    for (const Field& f : program.structs[static_cast<std::size_t>(elem.decl)].fields) {
        if (IsLeafKind(KindOf(program, f.type))) {
            AddEntry(out, first, var.name + "." + f.name, program, f.type,
                     FieldCursor(e0, f.offset), type.count, step, aux);
        }
    }
}

} // namespace

std::uint32_t Fnv1a32(const std::vector<std::uint8_t>& bytes) {
    std::uint32_t h = 2166136261u;
    for (std::uint8_t b : bytes) {
        h = (h ^ b) * 16777619u;
    }
    return h;
}

std::string DescribeVarsJson(const Program& program, const Compiled& compiled,
                             std::uint32_t disk_fnv) {
    std::string out = "{\"disk_fnv\": " + std::to_string(disk_fnv) +
                      ",\n\"check\": {\"at\": " + std::to_string(compiled.origin) +
                      ", \"bytes\": [";
    for (std::size_t i = 0; i < 16 && i < compiled.image.size(); i++) {
        out += (i == 0 ? "" : ", ") + std::to_string(compiled.image[i]);
    }
    out += "]},\n\"vars\": {";
    bool first = true;
    for (std::size_t v = 0; v < program.vars.size(); v++) {
        const Var& var = program.vars[v];
        if (var.kind != VarKind::Global || var.function >= 0 || var.made || var.placement.reserve ||
            var.placement.on_disk) {
            continue;
        }
        if (v >= compiled.layout.vars.size()) {
            continue;
        }
        const VarPlace& place = compiled.layout.vars[v];
        if (place.size == 0 || place.dropped || place.interp_only) {
            continue;
        }
        AddVar(out, first, program, compiled.layout, static_cast<std::int32_t>(v));
    }
    out += "\n},\n\"enums\": {";
    first = true;
    for (const EnumDecl& e : program.enums) {
        std::string prefix =
            program.modules[static_cast<std::size_t>(e.module)].name + "." + e.name;
        for (const EnumMember& m : e.members) {
            out += (first ? "\n  \"" : ",\n  \"") + prefix + "." + m.name +
                   "\": " + std::to_string(m.value);
            first = false;
        }
    }
    out += "\n}}\n";
    return out;
}

std::string VarsJsonPath(const std::string& disk_path) {
    std::filesystem::path path(disk_path);
    std::filesystem::path name = path.stem();
    name += ".vars.json";
    return (path.parent_path() / name).string();
}

} // namespace tisple
