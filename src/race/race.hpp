#pragma once

// Timing one kernel inside a whole binary, the same way for every compiler in the race
// (races/README.md). The binary starts from its own entry point, so each compiler's
// startup runs as it would; when the PC first reaches the kernel's entry the inputs are
// written into memory and the clock starts. It stops when the kernel returns to whatever
// called it: the PC at the return address the call pushed, with the stack back where it
// was. Kernel cycles count from the kernel's first instruction to its RTS, inclusive; the
// JSR that called it is not counted. With an after address the clock (and the record of
// what ran) starts only once the kernel has called the function there and it has returned;
// with a stop address it stops when the PC first gets there, and the kernel then runs on to
// its return. The random-program race times from t.init's return to t.checksum's entry:
// the work of setting the globals and of the checksum is not the program's.

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
    std::uint16_t start = 0; // where the binary starts running
    std::uint16_t entry = 0; // the kernel's first instruction
    bool has_after = false;
    std::uint16_t after = 0; // the clock starts when the function here returns
    bool has_stop = false;
    std::uint16_t stop = 0;                // the clock stops here, if has_stop
    std::vector<RacePatch> patches;        // written when the kernel is entered
    std::uint64_t max_cycles = 2000000000; // kernel cycle limit
    std::uint64_t max_startup = 10000000;  // cycles allowed before the kernel starts
};

struct RaceResult {
    bool returned = false;
    std::string error; // why it did not, when it did not
    std::uint64_t startup_cycles = 0;
    std::uint64_t kernel_cycles = 0;    // to the stop address, when there is one
    std::uint64_t total_cycles = 0;     // from the entry to the return
    Regs regs;                          // at the return
    std::vector<std::uint8_t> executed; // 65536 flags: an opcode ran here while the clock ran
};

// FILE@ADDR (split at the last @, ADDR as $hex, 0xhex or decimal): the file's bytes at
// ADDR; false with the error set when it cannot be read or does not fit below $10000
bool ReadPatchArg(const char* text, RacePatch& out, std::string& error);

// runs the machine as loaded; memory is left as the kernel left it
RaceResult RunRace(Machine& m, const RaceOptions& options);

// address ranges of every byte of every instruction that ran, "first last" in hex a line
std::string ExecutedRanges(const Machine& m, const std::vector<std::uint8_t>& executed);

} // namespace tisple
