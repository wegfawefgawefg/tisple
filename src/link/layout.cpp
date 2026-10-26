#include "link/layout.hpp"

#include <algorithm>
#include <cstdio>

namespace tisple {

const AddressRange kReservedRanges[5] = {
    {0x0000, 0x0100},  // zero page
    {0x0100, 0x0200},  // stack
    {0x0400, 0x0800},  // text page 1
    {0x2000, 0x4000},  // hi-res page 1
    {0xC000, 0x10000}, // I/O, ROM
};

namespace {

struct Used {
    std::uint32_t start = 0;
    std::uint32_t end = 0;
    std::int32_t var = -1;
};

struct Placer {
    const Program* program = nullptr;
    const LayoutOptions* options = nullptr;
    Layout* out = nullptr;
    std::vector<Diagnostic>* errors = nullptr;
    std::vector<Used> used;
};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "$%04X", value);
    return text;
}

std::string ShortName(const Var& var) {
    std::size_t dot = var.name.rfind('.');
    return dot == std::string::npos ? var.name : var.name.substr(dot + 1);
}

bool HasArray(const Program& program, TypeId type) {
    const Type& t = TypeOf(program, type);
    if (t.kind == TypeKind::Array) {
        return true;
    }
    if (t.kind != TypeKind::Struct) {
        return false;
    }
    for (const Field& field : program.structs[static_cast<std::size_t>(t.decl)].fields) {
        if (HasArray(program, field.type)) {
            return true;
        }
    }
    return false;
}

// the shape before an address is picked: representation, plane stride and size
bool Shape(Placer& pl, std::int32_t id, VarPlace& place, bool& per_plane) {
    const Var& var = pl.program->vars[static_cast<std::size_t>(id)];
    const Type& type = TypeOf(*pl.program, var.type);
    place.size = type.size;
    place.split = !var.address_taken && HasArray(*pl.program, var.type);
    per_plane = false;
    if (type.kind != TypeKind::Array) {
        return true;
    }
    std::uint32_t count = type.count;
    std::uint32_t elem_size = TypeOf(*pl.program, type.elem).size;
    place.plane_stride = count;
    std::uint32_t boundary = var.placement.boundary;
    if (!place.split || elem_size < 2 || boundary == 0 || type.size <= boundary) {
        return true;
    }
    // each plane in a block of its own, so every (at table i) stays inside one block
    if (count > boundary) {
        AddError(*pl.errors, var.loc,
                 ShortName(var) + " has " + std::to_string(count) +
                     "-byte tables, more than its :boundary " + std::to_string(boundary) +
                     "; shorten the array or drop :boundary");
        return false;
    }
    per_plane = true;
    place.plane_stride = boundary;
    place.size = (elem_size - 1) * boundary + count;
    return true;
}

// does [start, start + inside) stay in one block
bool BoundaryHolds(std::uint32_t start, std::uint32_t inside, std::uint32_t boundary) {
    return boundary == 0 || start % boundary + inside <= boundary;
}

const Used* Overlap(const Placer& pl, std::uint32_t start, std::uint32_t end) {
    for (const Used& used : pl.used) {
        if (start < used.end && used.start < end) {
            return &used;
        }
    }
    return nullptr;
}

std::uint32_t AlignUp(std::uint32_t value, std::uint32_t align) {
    if (align <= 1) {
        return value;
    }
    return (value + align - 1) / align * align;
}

void PlaceFixed(Placer& pl, std::int32_t id, VarPlace& place, bool per_plane) {
    const Var& var = pl.program->vars[static_cast<std::size_t>(id)];
    std::uint32_t start = var.placement.address;
    std::uint32_t end = start + place.size;
    std::string name = ShortName(var);
    if (end > 0x10000) {
        AddError(*pl.errors, var.loc,
                 name + " at " + Hex(start) + " is " + std::to_string(place.size) +
                     " bytes and runs past $FFFF; move it lower");
        return;
    }
    if (var.placement.align > 1 && start % var.placement.align != 0) {
        AddError(*pl.errors, var.loc,
                 ":address " + Hex(start) + " is not a multiple of :align " +
                     std::to_string(var.placement.align) + "; use " +
                     Hex(AlignUp(start, var.placement.align)) + " or drop :align");
        return;
    }
    std::uint32_t inside = per_plane ? TypeOf(*pl.program, var.type).count : place.size;
    if (!BoundaryHolds(start, inside, var.placement.boundary)) {
        AddError(*pl.errors, var.loc,
                 name + " at " + Hex(start) + " crosses a " +
                     std::to_string(var.placement.boundary) + "-byte block; start it at " +
                     Hex(AlignUp(start, var.placement.boundary)));
        return;
    }
    for (const AddressRange& range : pl.options->reserved) {
        if (start < range.end && range.start < end) {
            AddError(*pl.errors, var.loc,
                     name + " (" + Hex(start) + "-" + Hex(end - 1) +
                         ") overlaps the program's code or frames (" + Hex(range.start) + "-" +
                         Hex(range.end - 1) + "); move it");
            return;
        }
    }
    if (const Used* other = Overlap(pl, start, end)) {
        const Var& other_var = pl.program->vars[static_cast<std::size_t>(other->var)];
        AddError(*pl.errors, var.loc,
                 name + " (" + Hex(start) + "-" + Hex(end - 1) + ") overlaps " +
                     ShortName(other_var) + " (" + Hex(other->start) + "-" + Hex(other->end - 1) +
                     "); move one");
        return;
    }
    place.address = static_cast<std::uint16_t>(start);
    pl.used.push_back(Used{start, end, id});
}

// start if the object fits there, else a later address worth trying
std::uint32_t NextCandidate(const Placer& pl, std::uint32_t start, const VarPlace& place,
                            std::uint32_t inside, std::uint32_t align, std::uint32_t boundary) {
    if (!BoundaryHolds(start, inside, boundary)) {
        return AlignUp(AlignUp(start + 1, boundary), align);
    }
    std::uint32_t end = start + place.size;
    for (const AddressRange& range : kReservedRanges) {
        if (start < range.end && range.start < end) {
            return AlignUp(range.end, align);
        }
    }
    for (const AddressRange& range : pl.options->reserved) {
        if (start < range.end && range.start < end) {
            return AlignUp(range.end, align);
        }
    }
    if (const Used* taken = Overlap(pl, start, end)) {
        return AlignUp(taken->end, align);
    }
    return start;
}

void PlaceAuto(Placer& pl, std::int32_t id, VarPlace& place, bool per_plane) {
    const Var& var = pl.program->vars[static_cast<std::size_t>(id)];
    std::uint32_t align = std::max<std::uint32_t>(var.placement.align, 1);
    std::uint32_t boundary = var.placement.boundary;
    if (boundary != 0 && !per_plane && place.size > boundary) {
        AddError(*pl.errors, var.loc,
                 ShortName(var) + " is " + std::to_string(place.size) +
                     " bytes, more than its :boundary " + std::to_string(boundary) +
                     "; drop :boundary or shrink it");
        return;
    }
    // per plane, only one table has to stay inside a block
    std::uint32_t inside = per_plane ? TypeOf(*pl.program, var.type).count : place.size;
    std::uint32_t start = AlignUp(pl.options->data_start, align);
    while (start + place.size <= pl.options->data_end) {
        std::uint32_t next = NextCandidate(pl, start, place, inside, align, boundary);
        if (next == start) {
            place.address = static_cast<std::uint16_t>(start);
            pl.used.push_back(Used{start, start + place.size, id});
            return;
        }
        start = next;
    }
    AddError(*pl.errors, var.loc,
             "no room for " + ShortName(var) + " (" + std::to_string(place.size) +
                 " bytes) in the data region " + Hex(pl.options->data_start) + "-" +
                 Hex(pl.options->data_end - 1));
}

} // namespace

namespace {

bool IsPreset(const LayoutOptions& options, std::size_t var) {
    return var < options.preset.size() && options.preset[var] >= 0;
}

} // namespace

bool ShapeProgram(const Program& program, Layout& out, std::vector<Diagnostic>& errors) {
    out = Layout();
    out.vars.resize(program.vars.size());
    LayoutOptions options;
    Placer pl;
    pl.program = &program;
    pl.options = &options;
    pl.out = &out;
    pl.errors = &errors;
    std::size_t before = errors.size();
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        bool plane = false;
        Shape(pl, static_cast<std::int32_t>(i), out.vars[i], plane);
    }
    return errors.size() == before;
}

bool LayoutProgram(const Program& program, const LayoutOptions& options, Layout& out,
                   std::vector<Diagnostic>& errors) {
    out = Layout();
    out.vars.resize(program.vars.size());
    Placer pl;
    pl.program = &program;
    pl.options = &options;
    pl.out = &out;
    pl.errors = &errors;
    std::size_t before = errors.size();

    std::vector<bool> per_plane(program.vars.size(), false);
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        bool plane = false;
        Shape(pl, static_cast<std::int32_t>(i), out.vars[i], plane);
        per_plane[i] = plane;
    }
    // backend choices first; they are not Used, so frames may share bytes
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        if (IsPreset(options, i)) {
            out.vars[i].address = static_cast<std::uint16_t>(options.preset[i]);
        }
    }
    // fixed addresses next, so automatic placement flows around them
    for (std::size_t i = 0; i < program.vars.size(); i++) {
        if (program.vars[i].placement.has_address && !IsPreset(options, i)) {
            PlaceFixed(pl, static_cast<std::int32_t>(i), out.vars[i], per_plane[i]);
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        for (std::size_t i = 0; i < program.vars.size(); i++) {
            const Var& var = program.vars[i];
            bool global = var.function < 0;
            if (var.placement.has_address || IsPreset(options, i) || global != (pass == 0) ||
                out.vars[i].size == 0) {
                continue;
            }
            PlaceAuto(pl, static_cast<std::int32_t>(i), out.vars[i], per_plane[i]);
        }
    }
    return errors.size() == before;
}

Cursor VarCursor(const Layout& layout, std::int32_t var) {
    const VarPlace& place = layout.vars[static_cast<std::size_t>(var)];
    Cursor cursor;
    cursor.address = place.address;
    cursor.stride = 1;
    cursor.split = place.split;
    cursor.root = true;
    return cursor;
}

Cursor FieldCursor(Cursor base, std::uint32_t offset) {
    Cursor cursor = base;
    cursor.address = static_cast<std::uint16_t>((base.address + offset * base.stride) & 0xFFFF);
    cursor.root = false;
    return cursor;
}

Cursor ElementCursor(const Program& program, const Layout& layout, std::int32_t root_var,
                     Cursor base, TypeId array_type, std::uint32_t index) {
    const Type& type = TypeOf(program, array_type);
    Cursor cursor = base;
    cursor.root = false;
    if (!base.split) {
        std::uint32_t elem_size = TypeOf(program, type.elem).size;
        cursor.address =
            static_cast<std::uint16_t>((base.address + index * elem_size * base.stride) & 0xFFFF);
        return cursor;
    }
    std::uint32_t planes = type.count;
    if (base.root && root_var >= 0) {
        planes = layout.vars[static_cast<std::size_t>(root_var)].plane_stride;
    }
    cursor.address = static_cast<std::uint16_t>((base.address + index * base.stride) & 0xFFFF);
    cursor.stride = base.stride * planes;
    return cursor;
}

std::uint16_t CursorByte(Cursor cursor, std::uint32_t k) {
    return static_cast<std::uint16_t>((cursor.address + k * cursor.stride) & 0xFFFF);
}

} // namespace tisple
