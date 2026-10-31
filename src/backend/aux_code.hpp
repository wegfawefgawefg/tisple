#pragma once

// The backend's side of with_aux (check/aux_mode.hpp, docs/language.md, Aux memory code):
// what the code that runs with the aux switches on needs of frames, code generation and the
// link.
//
// - Frames: every variable that code names, and the frame of every function it runs in (the
//   region's owner and each aux-mode function: params, locals, temporaries), live in zero page,
//   which the switches never move. frames.cpp forces them there; the build fails when they do
//   not fit.
// - Code: an owner is never inlined (FindInlines), so a region stays in its owner. In those
//   functions the peephole and the dead-store pass track only zero page (a byte elsewhere may
//   be main memory's or aux memory's), and code that may run with RAMRD on is mirrored: the
//   link puts it, the runtime routines it calls and the const data it reads by name at the
//   same addresses in aux memory (aux_link.cpp), never in the language card's bank 1.

#include "check/aux_mode.hpp"

#include <vector>

namespace tisple {

struct AuxCode {
    AuxModes modes;
    // by var: lives in zero page: named by an owner or an aux-mode function, or in one's frame
    std::vector<bool> vars;
    // by var: const data read by name where RAMRD may be on, copied to aux memory by the link
    std::vector<bool> consts;
    bool any = false;   // some with_aux
    bool reads = false; // some code may run with RAMRD on: there is code to mirror
};

AuxCode FindAuxCode(const Program& program);

// the function's code runs with the switches on, or has a with_aux: frames in zero page,
// memory tracked only in zero page
inline bool IsAuxFunction(const AuxCode& aux, std::int32_t function) {
    return aux.any && AuxFunction(aux.modes, function);
}

// its code must be at the same address in both memories
inline bool IsMirroredFunction(const AuxCode& aux, std::int32_t function) {
    return aux.any && AuxMirrored(aux.modes, function);
}

} // namespace tisple
