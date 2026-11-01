#pragma once

// What each instruction reads and writes of A, X, Y and the flags, for the register liveness
// of dead_regs.cpp (the bits are peephole.hpp's kLive ones; the 65C02's instructions too).

#include "backend/peephole.hpp"

namespace tisple {

struct RegEffect {
    std::uint8_t uses = 0;
    std::uint8_t defs = 0;
    bool removable = false; // nothing but its registers and flags change
};

// a load, ALU or transfer with no other effect is removable: an operand that is a number or
// RAM the program owns (below $C000), never I/O, a pointer or the stack
RegEffect EffectOf(const Assembly& as, const AsmItem& item);

} // namespace tisple
