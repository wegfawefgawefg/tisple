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

// never chosen for automatic placement: zero page, stack, text page 1, I/O. The other screens
// are ordinary memory unless the program keeps them with :reserve
struct AddressRange {
    std::uint32_t start = 0;
    std::uint32_t end = 0; // one past
};
extern const AddressRange kReservedRanges[4];

// where :in aux data goes when it has no :address: auxiliary ram above its text page
constexpr std::uint32_t kAuxDataStart = 0x0800;
constexpr std::uint32_t kAuxDataEnd = 0xC000;

struct LayoutOptions {
    std::uint32_t data_start = 0x0C00; // first address for data
    std::uint32_t data_end = 0xC000;   // one past the last
    // more main memory for data once [data_start, data_end) is full, tried in order: the
    // memory below the origin, the language card
    std::vector<AddressRange> more_data;
    // addresses the backend already chose (zero page, overlapped frames), by var id; -1 or
    // missing: the layout places the var. Preset vars are not checked against each other,
    // since overlapping frames share bytes on purpose.
    std::vector<std::int32_t> preset;
    // more ranges automatic and fixed placement must stay out of (code, frames)
    std::vector<AddressRange> reserved;
    // ranges of aux memory lets :in aux stay out of: main memory's code and const data that
    // with_aux code runs and reads with RAMRD on, which the link copies there
    std::vector<AddressRange> aux_reserved;
    // by var id: left out, no address and no bytes (const tables no kept code reaches)
    std::vector<bool> dropped;
    // by var id: automatic placement keeps it out of `avoid` (the language card's
    // $D000-$DFFF, for what code in the card's bank 1 reads: backend/card_bank.hpp)
    std::vector<bool> avoiders;
    AddressRange avoid;
};

struct VarPlace {
    std::uint16_t address = 0;
    std::uint32_t size = 0; // bytes spanned, padding between planes included
    bool split = false;
    std::uint32_t plane_stride = 0; // split arrays: distance between planes
    bool dropped = false;           // LayoutOptions::dropped: not in memory at all
    // a const only the interpreter reads, at an address inside the code: no bytes in the
    // binary (backend/link_data.hpp)
    bool interp_only = false;
};

struct Layout {
    std::vector<VarPlace> vars; // by var id
    bool no_room = false;       // automatic placement ran out of memory
    // by function id: what its name is worth as a value, the address of its entry for calls
    // through a pointer (set by the backend for the functions whose address is taken). Empty
    // or 0: kFunctionValueBase + id, which only the interpreter gives meaning (image.hpp)
    std::vector<std::uint16_t> functions;
    // by var: const data code that runs with RAMRD on reads by name, which the link copies to
    // the same address in aux memory (backend/aux_link.hpp); empty: none
    std::vector<bool> mirrored;
};

// every Var's shape (size, split, plane stride) with address 0: what code generation needs
// before addresses exist. LayoutProgram gives the same shapes.
bool ShapeProgram(const Program& program, Layout& out, std::vector<Diagnostic>& errors);

// globals and const arrays first (in declaration order), then every function's
// parameters and locals in a static frame of its own; then CheckConstantPointers. Lets
// :in aux go in auxiliary ram, apart from everything else; :reserve lets hold their bytes
// like any :address let. An array
// with no :align or :boundary of its own keeps each byte table (up to 256 bytes) inside one
// page when some free start allows it, so indexed reads never pay for a page crossing
bool LayoutProgram(const Program& program, const LayoutOptions& options, Layout& out,
                   std::vector<Diagnostic>& errors);

// pointer provenance (docs/language.md): (as (ptr t) n) reaches only memory the program does
// not declare, so a constant n inside a laid-out variable is an error that says to use addr_of
bool CheckConstantPointers(const Program& program, const Layout& layout,
                           std::vector<Diagnostic>& errors);

struct Cursor {
    std::uint16_t address = 0;
    std::uint32_t stride = 1;
    bool split = false;
    bool root = true; // still the Var itself, so its plane_stride applies
    // reached through a pointer: inside a with_aux, the memory the switches pick (interp.hpp)
    bool pointer = false;
    // a place in a var :in aux by name (a const's, read where RAMRD is on): aux memory's bytes
    bool aux = false;
};

Cursor VarCursor(const Layout& layout, std::int32_t var);
Cursor FieldCursor(Cursor base, std::uint32_t offset);
Cursor ElementCursor(const Program& program, const Layout& layout, std::int32_t root_var,
                     Cursor base, TypeId array_type, std::uint32_t index);
// address of byte k of the value at a cursor
std::uint16_t CursorByte(Cursor cursor, std::uint32_t k);

} // namespace tisple
