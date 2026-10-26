#pragma once

// Locals that only name a byte index: (let l u16 (as u16 (- x 1))) in a loop over x. Where
// such a local indexes a pointer or an array, code generation reads the index straight from
// x (x - 1 is LDY x / DEY, or an address one lower), and when every read of the local is
// such an index its let never runs.

#include "check/program.hpp"

#include <vector>

namespace tisple {

struct IndexAlias {
    ExprId value = -1;       // the let's value, which the local always equals; -1: none
    bool index_only = false; // every read is an index, so nothing reads its memory
    bool forward = false;    // read once, and that read is the value itself: no let
};

// by var. The value is a byte form: a u8 variable or constant, plus or minus constants,
// maybe widened with (as u16 ...); the local is never set, and no variable the value reads
// is written while the local is in scope.
//
// Also scalar lets read just once with nothing between that changes what their value
// reads (forward): (let alive u8 (load (+ here c))) read only in a later condition is that
// load, done there, and only when the condition gets that far.
std::vector<IndexAlias> FindIndexAliases(const Program& program);

} // namespace tisple
