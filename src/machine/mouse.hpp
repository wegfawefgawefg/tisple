#pragma once

// the applemouse ii interface card, as programs see it through its firmware.
//
// the real card is a 6821 pia, a 6805 that tracks the mouse, and a 2 KB rom paged into $Cn00
// 256 bytes at a time by port B bits 1-3. its firmware entry points (the offset of each is
// the byte at $Cn12-$Cn19, from $Cn00) take X = $Cn and Y = $n0, return C clear when fine:
// - SETMOUSE ($Cn12): A = mode, $00-$0F (bit 0 mouse on; bits 1-3 interrupts, which are not
//   emulated); C set for a bigger one. The mode also goes to $07F8 + n.
// - SERVEMOUSE ($Cn13): C clear when the mouse caused the interrupt; never here.
// - READMOUSE ($Cn14): X to $0478 + n (low) and $0578 + n (high), Y to $04F8 + n and
//   $05F8 + n, and the status to $0778 + n: bit 7 the button is down, bit 6 it was down at
//   the last READMOUSE, bit 5 the mouse moved since then (bits 1-3 interrupt causes, 0 here).
// - CLEARMOUSE ($Cn15): the position to 0, 0, in the card and in the slot's position holes.
// - POSMOUSE ($Cn16): the position from the slot's screen holes, laid out as READMOUSE's.
// - CLAMPMOUSE ($Cn17): A = 0 for X, 1 for Y; the window from the slot 0 holes: $0478 and
//   $0578 the low bound (low, high byte), $04F8 and $05F8 the high bound.
// - HOMEMOUSE ($Cn18): the position to the window's top left.
// - INITMOUSE ($Cn19): window 0-1023 both ways, position 0, 0.
// - identification: $Cn05 = $38, $Cn07 = $18, $Cn0B = $01, $Cn0C = $20 (an X-Y pointing
//   device), $CnFB = $D6.
//
// the 6805 is modelled by its behaviour, the way AppleWin's MouseInterface.cpp (GPL-2.0 or
// later, after Kyle Kim's Apple in PC) does: commands $00 | mode SET, $10 READ, $20 SERVE,
// $30 CLEAR, $40 POS, $50 INIT, $60 | axis CLAMP, $70 HOME, answered at once. positions are
// signed 16-bit; motion is tracked whatever the mode, and only counts as "moved" while the
// mouse is on. no interrupts, so polled mode (SETMOUSE 1, READMOUSE) is what works.
//
// the card's 16 registers ($C080 + n * 16):
// - 0-7: the 6821 (register = address & 3), as on the real card. the real firmware uses
//   them: port A carries bytes, port B bits 4/5 strobe them and 6/7 are the 6805's replies.
// - 8-13: a port only this emulator has, for our firmware: write the arguments to 9-12 (as
//   CLAMPMOUSE's or POSMOUSE's holes, in address order), then a command to 8; it runs at once
//   and READMOUSE's answer is in 9-13 (x low, x high, y low, y high, status).
//
// our firmware (mouse_rom.cpp, original) is the default; `--mouse-rom MouseInterface.rom`
// puts in the owner's real rom (never committed), which talks to the 6821. it finds its slot
// through the monitor's IORTS at $FF58, so it also needs an Apple rom (--rom).

#include "machine/apple2.hpp"

#include <cstdint>

namespace tisple {

// where `tisple machine` and `tisple run` put the card, as on a IIe
constexpr int kMouseSlot = 4;

// the slot hooks; slots.cpp calls these for a kMouse card
uint8_t MouseIoAccess(Apple2& a, int reg, bool is_write, uint8_t value, bool& drives);
uint8_t MouseRomByte(const Apple2& a, int offset); // $Cn00 + offset, in the page selected
void ResetMouse(Apple2& a);                        // the reset line

// our own firmware for a card in this slot (mouse_rom.cpp)
void BuildMouseRom(int slot, uint8_t out[256]);

// plugs the card into a slot with the owner's 2 KB MouseInterface.rom, or null for ours
void InsertMouseCard(Apple2& a, int slot, const uint8_t* rom);
bool HasMouseCard(const Apple2& a);

// the host's mouse: a move by dx, dy (then clamped), and the button
void MoveMouse(Apple2& a, int32_t dx, int32_t dy);
void SetMouseButton(Apple2& a, bool down);
// scripted input: from `cycle` the mouse is at x, y (clamped) with the button so. applied
// when the program next touches the card, which is the first moment it can tell
bool ScheduleMouse(Apple2& a, uint64_t cycle, int32_t x, int32_t y, bool button);

// a status line, "mouse: slot 4, x 10 y 20, button up, mode $01", if the card was used
std::string DescribeMouse(const Apple2& a);

} // namespace tisple
