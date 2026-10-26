#pragma once

// What each function may write, callees included, so code generation only keeps a copy of
// a value when the code that runs before it is used can really change it.

#include "check/program.hpp"

#include <vector>

namespace tisple {

struct Effects {
    std::vector<std::vector<bool>> writes; // by function, by var: sets or lets it
    std::vector<bool> stores;              // by function: writes through some pointer
};

Effects ComputeEffects(const Program& program);

// may running `code` change var?
//
// Pointer provenance (proposed; needs the owner's word): a pointer reaches only what it was
// made from. addr_of gives a pointer into that variable; (as (ptr t) n) gives one into
// memory the program does not declare (screens, I/O, ROM). Getting from one into another
// is undefined, so a store through a pointer can only change address-taken variables.
bool CodeMayWrite(const Program& program, const Effects& effects, ExprId code, std::int32_t var);

} // namespace tisple
