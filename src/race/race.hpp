#pragma once

// Timing one kernel inside a whole binary, the same way for every compiler in the race
// (races/README.md). The binary starts from its own entry point, so each compiler's
// startup runs as it would; when the PC first reaches the kernel's entry the inputs are
// written into memory and the clock starts. It stops when the kernel returns to whatever
// called it: the PC at the return address the call pushed, with the stack back where it
// was. Kernel cycles count from the kernel's first instruction to its RTS, inclusive; the
// JSR that called it is not counted.

#include "emu/harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct RacePatch {
    std::uint16_t address = 0;
    std::vector<std::uint8_t> bytes;
};

struct RaceOptions {
    std::uint16_t start = 0;               // where the binary starts running
    std::uint16_t entry = 0;               // the kernel's first instruction
    std::vector<RacePatch> patches;        // written when the kernel is entered
    std::uint64_t max_cycles = 2000000000; // kernel cycle limit
    std::uint64_t max_startup = 10000000;  // cycles allowed before the kernel starts
};

struct RaceResult {
    bool returned = false;
    std::string error; // why it did not, when it did not
    std::uint64_t startup_cycles = 0;
    std::uint64_t kernel_cycles = 0;
    Regs regs;                          // at the return
    std::vector<std::uint8_t> executed; // 65536 flags: an opcode ran at this address
};

// runs the machine as loaded; memory is left as the kernel left it
RaceResult RunRace(Machine& m, const RaceOptions& options);

// address ranges of every byte of every instruction that ran, "first last" in hex a line
std::string ExecutedRanges(const Machine& m, const std::vector<std::uint8_t>& executed);

} // namespace tisple
