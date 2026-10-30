#pragma once

// Loop-invariant code motion: licm_scan.cpp finds what may move, licm.cpp moves it.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

// what may change while a loop runs
struct LoopWrites {
    std::vector<bool> vars; // by var: set, let or looped over in it, or set by a call in it
    bool stores = false;    // stores through pointers: address-taken variables may change
    bool all = false;       // jsr, io_write, a call doing I/O, a node not known here
};

LoopWrites FindLoopWrites(const Pass& p, ExprId loop);

// the largest invariant expressions worth moving out of a loop, outermost first: in a
// while's condition (worked out whenever the loop is reached), and in its body where they
// run whenever the body does (not in an arm, nor in a loop inside that may run zero times)
struct Invariants {
    std::vector<ExprId> head;
    std::vector<ExprId> body;
};

// trips: the loop's passes when known (KnownTrips), else -1
void FindInvariants(const Pass& p, ExprId loop, const LoopWrites& w, std::int64_t trips,
                    Invariants& out);

// may running id end the pass early: a return, a break or continue not inside a loop of its
// own (depth counts the loops entered), or a node not known here
bool MayLeave(const Pass& p, ExprId id, int depth);

// ---- what moving wins (licm_cost.cpp)

// its estimated cycles beat reading a local enough to move it, out of a loop of trips passes
// (-1: not known)
bool Worth(const Pass& p, ExprId id, std::int64_t trips);
// a rough count of the cycles the 6502 spends working id out (common subexpressions use it
// too)
std::int32_t EstimatedCycles(const Pass& p, ExprId id);
// how many passes a loop makes, when its literal range (or a let of its counter just before,
// as before, and literal bounds and step) says; -1 when it does not
std::int64_t KnownTrips(const Pass& p, ExprId loop, ExprId before);
// the before KnownTrips takes for the item at index of a do: a counting while's counter's let
// when only lets of other variables stand between them, else the item just before; -1 for
// the first item
ExprId CounterBefore(const Pass& p, ExprId list, std::uint32_t index);
// a loop whose body surely runs once each time it is reached: a for with a literal range
// that is not empty or that runs to the type's end, a (while true)
bool BodyRuns(const Pass& p, ExprId loop);
// sets, lets and fors of var under id
std::uint32_t CountSets(const Pass& p, ExprId id, std::int32_t var);

} // namespace tisple::opt
