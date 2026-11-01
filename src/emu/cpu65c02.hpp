#pragma once

// Our 65C02 core: the CMOS chip of Apple's enhanced IIe and IIc, the parts without Rockwell's
// bit instructions (RMB, SMB, BBR, BBS) and WDC's WAI and STP. Apple documents only the 65C02's
// 27 new instructions, and code that used the Rockwell ones would break on a IIgs, whose 65816
// gives $x7 and $xF other meanings.
//
// An instruction at a time, each bus cycle performed in order through the machine's pins
// (emu/bus.hpp), so the I/O hook, the cycle count and the bus log see what the chip does:
// - the 65C02's dummy cycles: a read of the last operand byte where the NMOS chip read a
//   half-formed address (indexed page crossings, stores), two reads and a write for a
//   read-modify-write (the NMOS chip writes twice), JMP (abs) reading the NMOS chip's wrong
//   address once before the right one (the page bug fixed, 6 cycles);
// - decimal mode: N, V and Z valid, the 65C02's own SBC result (Bruce Clark's sequences), one
//   cycle more, which reads the operand's address again;
// - BRK, IRQ and reset clear D;
// - shifts and rotates abs,X take 6 cycles without a page crossing (INC and DEC abs,X 7);
// - the undefined opcodes are NOPs of the documented lengths and cycles (6502.org, Bruce
//   Clark's "65C02 Opcodes"; Klaus Dormann's 65C02 test checks the lengths): $x2 2 bytes 2
//   cycles; $x3, $x7, $xB, $xF 1 byte 1 cycle (no interrupt starts after one); $44 2 bytes 3
//   cycles (a zero-page read); $54, $D4, $F4 2 bytes 4 cycles (zp,X); $DC, $FC 3 bytes 4
//   cycles; $5C 3 bytes 8 cycles.
// Interrupts are polled like the NMOS core polls them (machine.cpp): the line and the I flag
// as they are at the second-to-last cycle, a taken branch that stays in its page polling at
// its first. NMI is not wired (nothing in the machine raises it).
//
// Checked against SingleStepTests' synertek65c02 set (every bus cycle; tests/test_singlestep.cpp
// says where that set's chip differs from the documented NOPs) and Klaus Dormann's functional,
// 65C02 and decimal tests (tests/test_dormann.cpp).

#include "emu/machine.hpp"

namespace tisple {

// one instruction, or the interrupt sequence when the last one polled an interrupt
void Step65c02(Machine& m);
// the reset sequence: 7 cycles, then the vector at $FFFC; I set, D clear
void Reset65c02(Machine& m);

} // namespace tisple
