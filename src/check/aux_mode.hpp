#pragma once

// Where code runs with the aux memory switches on: (with_aux reads writes body ...)
// (docs/language.md, Aux memory code). The checker makes the form into an AuxSwitch on, the
// body, and an AuxSwitch off, with an off before each return, break and continue that leaves
// it; FindAuxModes follows the switches through every function's tree, so it holds for the
// program as checked and as the mid-level passes leave it (inlined, unrolled, unswitched).
//
// - RAMRD (kAuxReads): reads of $0200-$BFFF, instruction fetches too, come from aux memory.
// - RAMWRT (kAuxWrites): writes of $0200-$BFFF go to aux memory.
// - A function called where a switch may be on runs with it on: an aux-mode function, which
//   its callees are too. Its frame and the region owner's live in zero page; code that may
//   run with RAMRD on is mirrored (the same bytes at the same address in aux memory).

#include "check/program.hpp"

#include <cstdint>
#include <vector>

namespace tisple {

constexpr std::uint8_t kAuxReads = 1;
constexpr std::uint8_t kAuxWrites = 2;

// the switches an AuxSwitch on may turn on: RAMRD unless its reads is false, RAMWRT unless
// its writes is
std::uint8_t AuxOnBits(const Program& program, const Expr& on);

struct AuxModes {
    // by function: the switches that may be on while it runs, from the calls inside a
    // with_aux (and from the functions those run)
    std::vector<std::uint8_t> entry;
    // by function: the first call that runs it with a switch on (for messages), or -1
    std::vector<ExprId> entry_site;
    // by function: the switches its own with_aux forms may turn on
    std::vector<std::uint8_t> owns;
    // by expr: the switches that may be on when it runs (an AuxSwitch off: before it runs)
    std::vector<std::uint8_t> state;
    bool any = false; // some with_aux in the program
};

AuxModes FindAuxModes(const Program& program);

// runs with a switch on, or has a with_aux of its own: its frame goes in zero page
inline bool AuxFunction(const AuxModes& modes, std::int32_t function) {
    std::size_t f = static_cast<std::size_t>(function);
    return modes.entry[f] != 0 || modes.owns[f] != 0;
}

// may run with RAMRD on: its code must be at the same address in both memories
inline bool AuxMirrored(const AuxModes& modes, std::int32_t function) {
    std::size_t f = static_cast<std::size_t>(function);
    return ((modes.entry[f] | modes.owns[f]) & kAuxReads) != 0;
}

// the switches that may be on when expr id runs
inline std::uint8_t AuxStateAt(const AuxModes& modes, ExprId id) {
    return id >= 0 && static_cast<std::size_t>(id) < modes.state.size()
               ? modes.state[static_cast<std::size_t>(id)]
               : 0;
}

// a function with an AuxSwitch on in its tree
bool HasAuxSwitch(const Program& program, std::int32_t function);

} // namespace tisple
