#pragma once

// Layout: an address for every Var and how its bytes are arranged. The interpreter reads
// and writes through it, and the backend will reuse it, so both agree byte for byte.
//
// Representation, per Var: split, unless some addr_of reaches into it (Var::address_taken),
// then packed. Types without arrays look the same either way.
// - packed: the type's bytes in order: fields in declared order, u16 low byte first,
//   array elements one after another. Pointers always see packed bytes.
// - split: every array is stored as byte planes. Byte k of element i lives at
//   base + k * plane_stride + i, so (array Sprite 32) is one 32-byte table per byte of
//   Sprite and (array u16 192) is a low-byte table followed by a high-byte table.
//   A Var's own array uses VarPlace::plane_stride (its length, or a whole block when a
//   :boundary keeps each plane in its own block); arrays nested in it use their length.
//
// Addresses of places, as cursors (address, stride), wrapping at 64 KB:
//   variable:            (address, 1)
//   field at offset o:   (a + o * s, s)
//   element i, packed:   (a + i * size(elem) * s, s)
//   element i, split:    (a + i * s, s * P), P from above
//   a scalar's bytes:    low at a, high at a + s

#include "check/program.hpp"

namespace tisple {

struct LayoutOptions {
    std::uint32_t data_start = 0x0C00; // first address for data
    std::uint32_t data_end = 0xC000;   // one past the last
};

struct VarPlace {
    std::uint16_t address = 0;
    std::uint32_t size = 0; // bytes spanned, padding between planes included
    bool split = false;
    std::uint32_t plane_stride = 0; // split arrays: distance between planes
};

struct Layout {
    std::vector<VarPlace> vars; // by var id
};

// never chosen for automatic placement: zero page, stack, text page 1, hi-res page 1, I/O+ROM
struct AddressRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0; // one past
};
extern const AddressRange kReservedRanges[5];

// globals and const arrays first (in declaration order), then every function's
// parameters and locals in a static frame of its own
bool LayoutProgram(const Program& program, const LayoutOptions& options, Layout& out,
                   std::vector<Diagnostic>& errors);

struct Cursor {
    std::uint16_t address = 0;
    std::uint32_t stride = 1;
    bool split = false;
    bool root = true; // still the Var itself, so its plane_stride applies
};

Cursor VarCursor(const Layout& layout, std::int32_t var);
Cursor FieldCursor(Cursor base, std::uint32_t offset);
Cursor ElementCursor(const Program& program, const Layout& layout, std::int32_t root_var,
                     Cursor base, TypeId array_type, std::uint32_t index);
// address of byte k of the value at a cursor
std::uint16_t CursorByte(Cursor cursor, std::uint32_t k);

} // namespace tisple
