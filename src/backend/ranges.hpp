#pragma once

// Value ranges for code generation: the largest value an expression can have, so an add of
// two small values needs no carry out (Life's neighbour sum of cells that are 0 or 1).
//
// A variable has a range when every write to it is known: a global, constant table or
// local of a byte type (u8, i8, bool, an enum) or an array of them, not at an :address, and
// for a global or address-taken local every store through a pointer that may reach it. Which
// pointers may reach what follows the provenance rule (docs/language.md): a pointer made by
// addr_of reaches only that variable, one made with `as` from a number reaches only memory
// no variable uses, and arithmetic stays inside. Pointer values are followed through lets,
// sets, arguments and returns; pointers kept in arrays, struct fields or memory are lumped
// together. Indices stay inside their arrays (the backend assumes this everywhere).

#include "check/program.hpp"

#include <vector>

namespace tisple {

// what a pointer value may point into
struct Targets {
    bool any = false;        // any variable whose address is taken
    bool undeclared = false; // memory no variable uses
    std::vector<bool> vars;  // by var
};

struct ValueRanges {
    std::vector<std::uint8_t> var_max; // by var: no byte of it holds more (255: anything)
    std::vector<std::uint8_t> var_min; // by var: no byte of it holds less (0: anything)
    std::vector<bool> candidate;       // by var: var_max is worked out from its writes
    std::vector<Targets> var_targets;  // by var, for pointer variables
    std::vector<Targets> ret_targets;  // by function returning a pointer
    Targets heap;                      // pointers kept in memory
    // by expr: may run with RAMRD on (check/aux_mode.hpp), where a load reads aux memory's
    // byte, whatever the pointer's targets hold; empty: none
    std::vector<bool> aux_reads;
};

ValueRanges FindValueRanges(const Program& program);

// what each pointer may point into (targets.cpp): var_targets, ret_targets and heap
void FindPointerTargets(const Program& program, ValueRanges& r);
// what a pointer-valued expression may point into
Targets PointsTo(const Program& program, const ValueRanges& r, ExprId id);
// any variable whose address is taken, and memory no variable uses
Targets AnyTargets(const Program& program);

// the largest value id can have, as raw bits: up to 255 for 8-bit types, 65535 for 16-bit
std::uint32_t ExprMax(const Program& program, const ValueRanges& ranges, ExprId id);
// the smallest, as raw bits (0 when nothing better is known): a shift by a count never 0
// needs no test for 0
std::uint32_t ExprMin(const Program& program, const ValueRanges& ranges, ExprId id);

} // namespace tisple
