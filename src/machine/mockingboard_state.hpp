#pragma once

// the Mockingboard's state as plain data, part of the Apple2 struct (apple2.hpp includes
// this). behaviour: via6522.hpp (the two 6522s), ay8910.hpp (the two AY-3-8910 sound
// chips), mockingboard.hpp (the card that wires them together).

#include <cstdint>

namespace tisple {

constexpr uint64_t kViaNever = UINT64_MAX;

// a 6522. the timers are kept lazily, as the cycle they next show $FFFF (their underflow)
// rather than counted down every cycle, so an idle card costs nothing between accesses
struct Via6522 {
    uint8_t orb = 0;
    uint8_t ora = 0;
    uint8_t ddrb = 0;
    uint8_t ddra = 0;
    uint8_t sr = 0;
    uint8_t acr = 0;
    uint8_t pcr = 0;
    uint8_t ifr = 0; // bits 0-6; bit 7 is worked out when read
    uint8_t ier = 0; // bits 0-6
    uint16_t t1_latch = 0xFFFF;
    uint8_t t2_latch_low = 0xFF;
    bool t1_armed = false; // one-shot: a write to T1C-H, not yet timed out
    // the counters at power on: T1 reads $FFFF at cycle 0, T2 something else
    uint64_t t1_underflow = 0x10000;
    uint64_t t2_underflow = 0x10000 + 0x5A5A;
    uint64_t t1_flag_at = kViaNever; // the cycle IFR bit 6 sets next
    uint64_t t2_flag_at = kViaNever; // and bit 5
};

// an AY-3-8910 as the card's bus sees it: the registers and the latched address. what it
// sounds like is worked out from the write log, outside the machine (mockingboard_audio)
struct AyChip {
    uint8_t regs[16] = {};
    uint8_t address = 0;   // the register the last latch chose
    bool selected = false; // the latched address had its top 4 bits clear
    uint8_t bus = 0xFF;    // the BDIR, BC1 and /RESET lines (port B bits 0-2) last acted on
    uint8_t data = 0xFF;   // port A then
};

// every register write and reset, in order, for whoever renders the sound
constexpr uint8_t kPsgLogReset = 0xFF; // reg: the chip was reset
constexpr uint32_t kPsgLogSize = 8192; // drain once a frame

struct PsgWrite {
    uint64_t cycle = 0;
    uint8_t chip = 0;
    uint8_t reg = 0;
    uint8_t value = 0;
};

struct MockingboardState {
    uint8_t slot = 0;
    Via6522 via[2]; // $Cn00 drives ay[0], $Cn80 ay[1]
    AyChip ay[2];
    PsgWrite log[kPsgLogSize]; // write n at [n % size]
    uint64_t log_count = 0;
    uint32_t accesses = 0;
    uint64_t writes = 0; // register writes, for the status line
};

} // namespace tisple
