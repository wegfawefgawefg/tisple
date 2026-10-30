#pragma once

// the Sweet Micro Systems Mockingboard (the "A" and "C" sound cards): two 6522s
// (via6522.hpp) and two AY-3-8910s (ay8910.hpp), the six voices of stereo sound, no speech
// chip. written from the chips' data sheets and the card's documented wiring:
// - the card has no rom. its whole $Cn00-$CnFF page is the 6522s: address bit 7 picks one,
//   the low 4 bits the register; bits 4-6 are not decoded, so each repeats 8 times.
//   $Cn00 is the first 6522, wired to the first AY (left), $Cn80 the second (right).
// - each 6522's port A is its AY's data bus and port B bits 0-2 its BC1, BDIR and /RESET.
// - both 6522s' IRQ outputs pull the 6502's IRQ line (no NMI).
// - the Apple's reset line resets both 6522s and both AYs.
//
// the 6522s answer every read in the page, so detection by reading the timer works; the
// IRQ line goes to the cpu as a level (Machine::irq_level_at), worked out at each access
// from when the timers will next set an enabled flag. the sound is not made here: every AY
// register write goes into a log with its cycle, which mockingboard_audio.hpp renders.

#include "machine/apple2.hpp"

#include <cstdint>
#include <string>

namespace tisple {

// where `--mockingboard` puts the card when no slot is given, as in examples/ticks.tsp
constexpr int kMockingboardSlot = 4;

// the slot hooks; slots.cpp calls these for a kMockingboard card. offset is the address in
// the $Cn00 page
uint8_t MockingboardRomAccess(Apple2& a, int offset, bool is_write, uint8_t value, bool& drives);
void ResetMockingboard(Apple2& a); // the reset line

void InsertMockingboard(Apple2& a, int slot);
bool HasMockingboard(const Apple2& a);

// the register write n for n in [log_count - kPsgLogSize, log_count)
const PsgWrite& MockingboardWrite(const Apple2& a, uint64_t n);

// "mockingboard: slot 4, 120 register writes, 600 interrupts", if the card was used
std::string DescribeMockingboard(const Apple2& a);

} // namespace tisple
