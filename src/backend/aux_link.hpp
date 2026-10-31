#pragma once

// The link's part of with_aux (aux_code.hpp): code that may run with RAMRD on fetches its
// instructions from aux memory, and reads the const data it names there too, so the same
// bytes must be at the same addresses in both memories.
//
// - Placement: those functions' spans, then the runtime routines and their tables when that
//   code calls one, go first in the memory below the origin, from its start (lets :in aux fill
//   aux memory from $0800 on, so that splits their room the least); without that memory, or
//   room in it, after all the other code. Each span keeps its own place if they spill.
// - Aux memory: the bytes of those spans, and of each const that code reads by name, go into
//   aux memory segments at the same addresses (the loader puts them there: tisple run and the
//   boot disk load aux segments, which a DOS 3.3 BRUN file cannot hold); lets :in aux are laid
//   out around them (LayoutOptions::aux_reserved), and an :address one on them is an error.
// - Nothing at $C000 and up is copied: the language card is the same whatever RAMRD says.

#include "backend/link.hpp"

#include <vector>

namespace tisple {

// by function: its code goes in the mirrored stretch (kept, may run with RAMRD on, not in
// the card's bank 1)
std::vector<bool> MirroredFunctions(const Backend& be, const std::vector<bool>& kept,
                                    const Bank1Plan* bank1);

// some mirrored function's code calls a runtime routine: the runtime and its tables are
// mirrored too
bool MirrorsRuntime(const Backend& be, const std::vector<bool>& mirrored);

// the addresses spans [first, end) take below $C000, merged where they touch
std::vector<AddressRange> SpanRanges(const std::vector<AsmItem>& items,
                                     const std::vector<FunctionSpan>& spans, std::size_t first,
                                     std::size_t end);

// the consts with_aux code reads by name with RAMRD on that the layout put below $C000: their
// ranges, and by var which they are (Layout::mirrored)
std::vector<AddressRange> MirroredConsts(const Backend& be, const Layout& layout,
                                         std::vector<bool>& mirrored);

// a program with an interrupt handler whose kept code has a with_aux, where main's code (in
// source, the program as written: the call may be inlined since) never calls auxmem.guard:
// an interrupt inside the with_aux would run the handler from aux memory on an unenhanced
// IIe, or with the card's ram in; the warning says to call it
bool AuxWithoutGuard(const Backend& be, const std::vector<bool>& kept, const Program& source,
                     Diagnostic& warning);

// the bytes of ranges from main memory's image to aux memory's
void CopyToAux(const std::vector<AddressRange>& ranges, const std::vector<std::uint8_t>& memory,
               std::vector<std::uint8_t>& aux);

} // namespace tisple
