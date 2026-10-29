#pragma once

// A checked Tisple program as C, for the random-program race (races/random.py): the same
// program for cc65, LLVM-MOS and Oscar64, with exactly Tisple's results.
//
// - Types are exact: uint8_t, int8_t, uint16_t, int16_t; bool and enums are uint8_t (0/1,
//   member values); structs are C structs; pointers are pointers. Every operation casts its
//   result back to its type, and goes through unsigned where C's int would overflow, so the
//   C means Tisple's wrapping arithmetic whatever int's width is (the host's 32 bits too).
// - What Tisple defines and C does not is written out: a shift by the width or more, a
//   division by zero (check/arith.hpp's results), -32768 / -1. Signed >> relies on the
//   compilers' arithmetic shift (implementation-defined, and all of them do).
// - Tisple evaluates left to right; C does not promise that, so a value a later operand's
//   call could change goes into a temporary first. Calls inside expressions always do.
// - break inside a switch is a goto past the loop.
// - Forms with no faithful C form (I/O, jsr, u16 <-> pointer reinterprets, struct values
//   other than make) are refused with an error, never approximated.
// - Globals with :address are RACE_FIXED objects (races/include/random.h): volatile, at the
//   address. t.init and t.checksum are RACE_NOINLINE: the race's clock runs from init's
//   return to the checksum's entry.

#include "check/program.hpp"

#include <string>

namespace tisple {

// the whole program as one C file that includes "random.h"; false with the error set when
// some form has no faithful C form
bool WriteRaceC(const Program& program, std::string& out, std::string& error);

} // namespace tisple
