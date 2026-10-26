#pragma once

// Reference interpreter: runs the typed program over a 64 KB memory image with the
// target's semantics, through the same Layout the backend uses. It is the oracle the
// 6502 backend is checked against, so it favours being obviously right.
//
// - Values are raw bits: u8/i8/bool/enums in the low byte, u16/i16/pointers in 16 bits.
//   Arithmetic wraps per width (check/arith.hpp, shared with constant folding).
// - Every variable, parameters and locals included, lives in memory at its Layout address;
//   functions have static frames (there is no recursion). The image starts zeroed, then
//   top-level initial values are written; a local let always has a value. Run it on the
//   backend's Layout (Compiled::layout) so addresses agree with the compiled program.
// - Evaluation is left to right: operands, call arguments, the place of a set before its
//   value, the pointer of a store before its value. A struct value is read whole before
//   it is written, so overlapping copies through pointers are well defined.
// - io_read and io_write go only to IoHooks, never to memory; a u16 access is the low byte
//   at addr, then the high byte at addr + 1.
// - jsr stops the run with an Error ("jsr needs the machine"): machine code runs only on
//   the 6502.
// - Every evaluated expression is one step; past max_steps the run stops with StepLimit.
//   Division by zero gives what the backend's routine gives (check/arith.hpp) and goes on.
// - A global with :address and no value is never written at the start: it holds whatever
//   memory held (zero in a fresh interpreter), as on the machine.

#include "check/program.hpp"
#include "link/layout.hpp"

#include <functional>
#include <string>
#include <vector>

namespace tisple {

struct IoHooks {
    std::function<std::uint8_t(std::uint16_t)> read;        // empty: reads give 0
    std::function<void(std::uint16_t, std::uint8_t)> write; // empty: writes are dropped
};

enum class StopKind : std::uint8_t {
    Finished,
    StepLimit,
    Error,
};

struct InterpResult {
    StopKind kind = StopKind::Finished;
    bool has_value = false;
    std::uint16_t value = 0; // raw bits of the return value
    TypeId type = kVoidType;
    std::string message; // StepLimit and Error
    Loc loc;             // where it stopped
    std::uint64_t steps = 0;
};

enum class Flow : std::uint8_t {
    Normal,
    Break,
    Continue,
    Return,
    Stop,
};

struct Interp {
    const Program* program = nullptr;
    const Layout* layout = nullptr;
    std::vector<std::uint8_t> memory; // 64 KB
    IoHooks io;
    std::uint64_t max_steps = 10000000;

    // run state
    std::uint64_t steps = 0;
    Flow flow = Flow::Normal;
    std::uint16_t return_value = 0;
    StopKind stop = StopKind::Finished;
    std::string message;
    Loc stop_loc;
};

// zeroes memory and writes every top-level initial value
void InitInterp(Interp& in, const Program& program, const Layout& layout);

// calls one function with raw argument bits; memory keeps whatever it did
InterpResult CallFunction(Interp& in, std::int32_t function,
                          const std::vector<std::uint16_t>& args);

std::uint16_t ReadScalar(const Interp& in, Cursor cursor, std::uint32_t size);
void WriteScalar(Interp& in, Cursor cursor, std::uint32_t size, std::uint16_t value);

} // namespace tisple
