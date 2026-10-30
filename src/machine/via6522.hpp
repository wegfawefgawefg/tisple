#pragma once

// the 6522 versatile interface adapter, as far as the Mockingboard uses one: ports A and B,
// both timers, the interrupt flags and enables. written from the MOS / Rockwell R6522 data
// sheets; the shift register only stores its byte, the handshake lines CA1-CB2 and T2's
// pulse counting (on PB6, which the card leaves open) are not modelled.
//
// registers (low 4 address bits):
//   0 ORB/IRB   1 ORA/IRA   2 DDRB   3 DDRA
//   4 T1C-L: read the counter's low byte (clears IFR bit 6), write the low latch
//   5 T1C-H: read the counter's high byte; write the high latch, then the latches go into the
//            counter, IFR bit 6 clears and the timer starts
//   6 T1L-L, 7 T1L-H: the latches (writing T1L-H clears IFR bit 6 too)
//   8 T2C-L: read the low byte (clears IFR bit 5), write the low latch
//   9 T2C-H: read the high byte; write: counter = this and the low latch, IFR bit 5 clears
//   10 SR   11 ACR (bit 6: T1 free-run)   12 PCR
//   13 IFR: read the flags, bit 7 set when any enabled one is; write ones to clear them
//   14 IER: write bit 7 set to enable the bits given, clear to disable them; reads bit 7 set
//   15 ORA without handshake
//
// timing, with w the cycle of the write to T1C-H or T2C-H that loads N: the counter reads N
// at w + 1 and one less each cycle after, 0 at w + 1 + N, then $FFFF at w + N + 2, the cycle
// the timer's IFR bit sets and the IRQ output goes low (the data sheets' N + 1.5). in
// free-run T1 then reloads from its latches: N, N - 1, ... again, every N + 2 cycles, setting
// the flag at every $FFFF. in one-shot mode (and always for T2) the counter goes on down
// through $FFFF, $FFFE, ... and the flag sets only the first time after the load.
//
// the IRQ output is low while (IFR & IER) has any bit set, and stays low until the program
// clears the bit: a level.

#include "machine/mockingboard_state.hpp"

#include <cstdint>

namespace tisple {

constexpr uint8_t kViaIrqT1 = 0x40;
constexpr uint8_t kViaIrqT2 = 0x20;

// cycle is the bus cycle of the access. port_a_in and port_b_in are what the outside drives on
// pins set as inputs
uint8_t ViaRead(Via6522& v, int reg, uint64_t cycle, uint8_t port_a_in, uint8_t port_b_in);
void ViaWrite(Via6522& v, int reg, uint8_t value, uint64_t cycle);

// the RES pin: every register to 0 but the timers' counters and latches, which run on
void ResetVia(Via6522& v, uint64_t cycle);

// the first cycle the IRQ output is low, given no more accesses: 0 when it is low already,
// kViaNever when nothing enabled will set
uint64_t ViaIrqCycle(const Via6522& v);

// the port pins: outputs drive the output register, inputs float high
uint8_t ViaPortA(const Via6522& v);
uint8_t ViaPortB(const Via6522& v);

// the counters at a cycle, without side effects
uint16_t ViaTimer1(const Via6522& v, uint64_t cycle);
uint16_t ViaTimer2(const Via6522& v, uint64_t cycle);

} // namespace tisple
