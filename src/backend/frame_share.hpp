#pragma once

// Bytes for the locals the mid-level passes make (Var::made: inlined copies' parameters and
// locals, the lets cse and licm add, unrolled copies of lets). The reference interpreter never
// sees them, so unlike the program's own variables they need bytes only while the code still
// reads what they hold. Liveness over the finished code of every function (after the peephole
// pass, so after every move the backend makes) says where each holds a value, and:
// - made locals whose values never meet share a slot
// - a copy's local takes its slot in the frame of the function it was copied from (frames
//   overlap for functions that never run at the same time, frames.cpp, so copies of functions
//   called one after the other share bytes the way the calls' frames did), when no code that
//   names it is that function's or below it, it holds no value across a call of anything but
//   that function's callees (their frames come after), and it never meets a byte of a frame
//   that may overlap that one; else its slot is in its own function's frame
// - a made local no code names gets no bytes, and a variable of the program no code names
//   (in a function whose calls were all copied) gets bytes only the interpreter uses, over
//   its function's temporaries and slots (frames.cpp).
// A made local a pointer may reach, with a fixed address, or read in a way the liveness does
// not follow (its address as a number, (zp,X), a data byte), or holding a value where the
// code starts, keeps bytes of its own in its function's frame, as every local did before.

#include "backend/gen.hpp"

namespace tisple {

struct FrameSlot {
    std::int32_t home = -1; // the function whose frame holds it
    std::uint32_t size = 0;
    bool forced = false; // a member is a dereferenced pointer: zero page
    std::uint64_t weight = 0;
    std::vector<std::int32_t> vars;
};

struct FrameShares {
    std::vector<FrameSlot> slots;
    std::vector<std::int32_t> slot_of; // by var: its slot, or -1 (an item of its own)
    // by var: a variable of a frame no code names (a made one needs no bytes; one of the
    // program's needs them only for the interpreter)
    std::vector<bool> unnamed;
};

// callers: by function, those whose frames come before its own (frames.cpp's stack, the
// interrupt handler's included)
void ShareMadeLocals(const Backend& be, const std::vector<std::vector<std::int32_t>>& callers,
                     FrameShares& out);

} // namespace tisple
