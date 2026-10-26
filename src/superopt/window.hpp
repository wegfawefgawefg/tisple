#pragma once

// Windows: straight-line runs of 2 to N instructions of compiled code, with no label inside
// (one may start them) and no jumps or branches, and what is live after them.
//
// Liveness is a forward scan from the window's end up to the next label, branch, jump,
// call or return (a branch's own flag read counts): a register, flag or directly addressed
// byte the window writes is dead only if the scan writes it before anything reads it.
// Everything still open when the scan stops is live, and memory reached through an index
// or a pointer is always live.

#include "backend/backend.hpp"
#include "superopt/exec.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct RawIns {
    Mn mn = Mn::NOP;
    Mode mode = Mode::Imp;
    std::uint16_t operand = 0; // address, zero-page byte or immediate
    std::uint16_t address = 0; // where it is in the program (0 for hand-made windows)
};

struct Window {
    std::uint16_t address = 0; // of the first instruction
    std::vector<SoIns> code;
    std::vector<Cell> cells;
    std::vector<std::uint8_t> imms; // immediates candidates may use
    std::uint8_t live = 0;          // kLoc mask: registers and flags read after the window
    std::uint8_t fixed = 0;         // registers, flags candidates may not write: live, kept
    bool assumes_no_alias = false;  // indexed or indirect bytes taken as distinct from the rest
    int cycles = 0;
    int bytes = 0;
};

// false with a reason when the window cannot be modelled (an instruction or operand the
// evaluator does not have, i/o, a pointer it writes, an index it moves before use)
bool MakeWindow(const std::vector<RawIns>& code, const std::vector<RawIns>& after, Window& out,
                std::string& why);

// the instructions that follow code at `item` in its span, up to the first label, branch,
// jump, call or return (that one included)
std::vector<RawIns> FollowingIns(const Compiled& compiled, std::size_t span, std::size_t item);

// a straight-line run's instructions, with the item each came from
struct RawRun {
    std::size_t span = 0;
    std::vector<RawIns> ins;
    std::vector<std::size_t> items;
};

// every maximal straight-line run of modelled instructions in the program
std::vector<RawRun> StraightRuns(const Compiled& compiled);

// the same window shape, wherever it is: code, cells and liveness as text
std::string WindowKey(const Window& w);

} // namespace tisple
