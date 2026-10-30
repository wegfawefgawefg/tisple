#pragma once

// the General Instrument AY-3-8910 programmable sound generator, written from GI's data
// sheet. two sides:
// - the bus (AyChip, in the machine): what the Mockingboard's 6522 does to it through its
//   ports. port A is the data bus; port B bit 0 is BC1, bit 1 BDIR and bit 2 /RESET, and BC2
//   is tied high, so port B reads as a function: $04 inactive, $05 read, $06 write, $07
//   latch address, and bit 2 clear resets the chip (every register to 0). a latch takes the
//   register number from port A; a number with any of its top 4 bits set deselects the chip
//   (its mask-programmed address is 0) and it ignores writes until the next latch.
// - the sound (AySynth, outside the machine): three square-wave tones, one noise source, the
//   mixer, the volumes and the envelope, stepped at the chip's clock / 8. the card clocks the
//   chip with the Apple's own 1.02 MHz.
//
// registers: 0-5 tone periods of A, B, C (fine 8 bits, coarse 4 bits), 6 noise period
// (5 bits), 7 mixer (bits 0-2 tone off for A-C, bits 3-5 noise off), 8-10 volumes (4 bits,
// bit 4 follows the envelope), 11-12 envelope period, 13 envelope shape (4 bits; writing it
// restarts the envelope), 14-15 i/o ports (nothing is wired to them on the card).
// - tone: f = clock / (16 * period); a period of 0 acts as 1
// - noise: a 17-bit shift register stepped at clock / (16 * period), bit 0 out
// - envelope: 16 steps, one each 16 * period clocks; the shape's bits are CONT (3), ATT (2),
//   ALT (1), HOLD (0)
// - a channel sounds (tone or tone off) and (noise or noise off); its level is the volume's,
//   on a scale of about 3 dB a step, 0 silent

#include "machine/mockingboard_state.hpp"

#include <cstdint>

namespace tisple {

// the bits each register keeps
uint8_t AyRegisterMask(int reg);

// port B and port A as the 6522 drives them now. returns true when that wrote a register
// (reg, value) or reset the chip (reg kPsgLogReset)
bool AyBus(AyChip& ay, uint8_t port_b, uint8_t port_a, uint8_t& reg, uint8_t& value);

// what the chip drives on port A: in the read function, the latched register; else nothing
// (the bus floats high)
uint8_t AyDataOut(const AyChip& ay);

struct AySynth {
    uint8_t regs[16] = {};
    uint16_t tone_count[3] = {};
    bool tone_high[3] = {};
    bool odd_tick = false; // noise and envelope step every other tick
    uint8_t noise_count = 0;
    uint32_t noise_shift = 1;
    uint16_t envelope_count = 0;
    uint8_t envelope_step = 0; // 0-15 within one ramp
    bool envelope_up = false;
    bool envelope_holding = false;
    uint8_t envelope_level = 0;
};

void ResetAySynth(AySynth& s);
void AySynthWrite(AySynth& s, uint8_t reg, uint8_t value);
// one step of the clock / 8; returns the three channels' levels added, 0 to 3
double AySynthTick(AySynth& s);

} // namespace tisple
