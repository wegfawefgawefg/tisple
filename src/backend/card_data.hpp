#pragma once

// Data in the language card's bank 1, and the code that switches the card itself
// (docs/language.md, Placement; docs/overlays.md, Data in bank 1).
//
// - A let :in bank1 (no value) is room at the top of bank 1 ($D000-$DFFF), below which the
//   bank 1 code goes: the link gives the lets their addresses there in the program's order,
//   and the startup never writes them. Code reaches them only through pointers, with bank 1
//   switched in: lib/disk reads into them (disk.BANK1), lib/mockingboard plays a song there
//   (mockingboard.play_bank1).
// - A card switcher is a function whose own code reads or writes $C080-$C08F (an io_read of
//   $C08B, say, to read a byte of such a let). While its switch has bank 1 in, bank 2's
//   $D000-$DFFF is out, so: its code goes right after the startup (with the bank stubs and
//   the interrupt entry, outside $D000-$DFFF), never into bank 1 or an overlay group, and the
//   variables its code names stay out of $D000-$DFFF; so does every function it calls while
//   bank 1 is in, and what they call, and what they name (FindCardSwitching). Memory it
//   reaches through pointers while bank 1 is in is the bank-1 lets', or outside $D000-$DFFF:
//   that the compiler cannot see.
// - A program with either has the interrupt entry that saves the card's bank and puts bank 2
//   in for the handler (irq.hpp), so a tick may land between a switcher's two switches.

#include "backend/gen.hpp"

namespace tisple {

// the code that may run with bank 1 switched in: by function, a card switcher, or one called
// (directly: a JSR or JMP to its entry) from a switcher's code between its read or write of
// $C088-$C08F and the next of $C080-$C087, and everything such a function calls in turn; and
// whether that code calls the runtime's routines (they then stay out of $D000-$DFFF too)
struct CardSwitching {
    std::vector<bool> functions;
    bool runtime = false;
};

CardSwitching FindCardSwitching(const Backend& be, const std::vector<bool>& kept,
                                const std::vector<AsmItem>& runtime);

// by function: what FindCardSwitching marks
std::vector<bool> CardSwitchers(const Backend& be, const std::vector<bool>& kept,
                                const std::vector<AsmItem>& runtime);

// whether one instruction of generated code is such an access
bool SwitchesCard(const AsmItem& item);

// the bytes the lets :in bank1 take together, 0 for none
std::uint32_t Bank1DataBytes(const Program& program, const Layout& shapes);

// by var: each let :in bank1's address, from the top of bank 1 down (the first declared
// lowest), -1 for every other var
std::vector<std::int32_t> Bank1DataAddresses(const Program& program, const Layout& shapes);

} // namespace tisple
