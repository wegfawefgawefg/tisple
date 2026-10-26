#pragma once

// peripheral slots 1-7. a card answers three windows:
// - i/o: $C080 + slot * 16, 16 registers ($C090-$C0FF for slots 1-7)
// - slot rom: $Cn00-$CnFF; touching it also selects the card's expansion rom
// - expansion rom: $C800-$CFFF for the selected card, released by touching $CFFF
// each call sets drives when the card puts a byte on the bus; otherwise the bus floats.
// cards read the clock from a.cpu.cycles when they need time (a disk's spinning).

#include "machine/apple2.hpp"

namespace tisple {

uint8_t CardIoAccess(Apple2& a, int slot, int reg, bool is_write, uint8_t value, bool& drives);
uint8_t CardRomAccess(Apple2& a, int slot, int offset, bool is_write, uint8_t value, bool& drives);
uint8_t CardExpansionAccess(Apple2& a, int slot, int offset, bool is_write, uint8_t value,
                            bool& drives);

// without side effects, for the run loop and PeekMemory
bool CardHasRom(const Apple2& a, int slot, bool expansion);
uint8_t CardRomPeek(const Apple2& a, int slot, uint16_t addr);

void InsertCard(Apple2& a, int slot, CardKind kind);
// the reset line reaches every card
void ResetCards(Apple2& a);

} // namespace tisple
