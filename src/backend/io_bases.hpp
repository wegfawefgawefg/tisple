#pragma once

// I/O through a base address in a global, at the cost of a fixed address (docs/design.md, "I/O
// bases"). A card found at run time is reached as (io_write u8 (+ page 1) v), page a u16
// global init sets once. Each such access would store the address into its own instruction
// first (20 cycles for a 6-cycle write); instead:
//
// - An I/O base is a u16 global, no addr_of of it, no :address, never set by the interrupt
//   handler's code, that some io_read or io_write uses as its address, alone or plus or minus
//   a constant K.
// - Such an access is one LDA or STA absolute whose operand bytes hold the base's value plus K
//   (the symbol `<base>.io`, plus K). It is I/O to the passes (a symbol at $C000 until the
//   link), so they never track or drop it.
// - Every (set base v) is followed by a call of `<base>.patch`, a function whose code (made
//   once all code is final) stores base + K into each such operand, with interrupts off, so
//   the operands always hold what the base holds. The startup calls it once too, before main.
// - The interpreter runs the accesses as written; the bytes they reach are the same.

#include "backend/gen.hpp"

namespace tisple {

// finds the bases, adds their patch functions and a call of one after each set of a base
IoBases LowerIoBases(Program& program);

// an access at addr through a base: its var and K (mod 65536). Not when later, worked out
// after the address and before the access (an io_write's value), may set the base
bool IoBaseAddress(const Program& program, const IoBases& bases, const Effects& effects,
                   ExprId addr, ExprId later, std::int32_t& var, std::int32_t& offset);

// once the code is final (functions dropped): each kept patch function's code, a label before
// each access through its base in the kept code, and every base symbol at its var's initial
// value
void EmitIoPatches(Backend& be, const std::vector<bool>& kept);

} // namespace tisple
