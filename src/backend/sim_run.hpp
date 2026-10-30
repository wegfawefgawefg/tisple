#pragma once

// Running a compiled program in the simulator: the whole thing from its first byte, or one
// function at a time the way the startup leaves the machine (for differential tests).

#include "backend/backend.hpp"
#include "emu/harness.hpp"

#include <memory>
#include <vector>

namespace tisple {

// every access to $C000-$CFFF, in order
struct IoAccess {
    std::uint16_t address = 0;
    std::uint8_t value = 0;
    bool write = false;
};

// what a read of the I/O page returns: a fixed function of the address and of how many
// accesses came before, so the interpreter and the simulator can be fed the same
std::uint8_t IoReadValue(std::uint16_t address, std::size_t index);

struct SimRunner {
    std::unique_ptr<Machine> machine;
    std::vector<IoAccess> io; // but the language card's switches, when the program uses it
    // the language card (Compiled::card_ram): the build's own reads of $C080-$C08F switch the
    // $D000 bank and of $C011 say which is in (Compiled::card_switches, by the instruction's
    // address). The flat memory holds that bank, other_bank the other (4 KB)
    bool card = false;
    bool bank2 = true;
    std::vector<std::uint8_t> other_bank;
    std::vector<bool> switch_at; // by address: an instruction of card_switches
};

// a fresh machine with the image loaded and the startup run up to JSR main
bool StartCompiled(SimRunner& run, const Compiled& compiled, std::string& error);

struct SimCall {
    StopReason reason = StopReason::kCycleLimit;
    bool returned = false;
    std::uint8_t a = 0;
    std::uint8_t x = 0;
    std::uint64_t cycles = 0;
};

// writes the arguments into the callee's frame and JSRs to it
SimCall CallCompiled(SimRunner& run, const Program& program, const Compiled& compiled,
                     std::int32_t function, const std::vector<std::uint16_t>& args,
                     std::uint64_t max_cycles);

struct SimMain {
    StopReason reason = StopReason::kCycleLimit;
    bool finished = false; // the final RTS came back to the caller
    bool zp_restored = false;
    std::uint8_t a = 0; // as main returned
    std::uint8_t x = 0;
    std::uint64_t main_cycles = 0;  // JSR main to its return
    std::uint64_t total_cycles = 0; // first byte to the final RTS
};

// the binary as BRUN runs it: zero page holds a pattern first, so the restore is checked
SimMain RunCompiledMain(SimRunner& run, const Compiled& compiled, std::uint64_t max_cycles);

} // namespace tisple
