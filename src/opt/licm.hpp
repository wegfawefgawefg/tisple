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

void FindInvariants(const Pass& p, ExprId loop, const LoopWrites& w, Invariants& out);

// ---- what moving wins (licm_cost.cpp)

// its estimated cycles beat reading a local enough to move it
bool Worth(const Pass& p, ExprId id);
// work the backend does faster inside a loop (a multiply by the quarter-square routines)
bool FasterInLoops(const Pass& p, ExprId id);
// how many passes a loop makes, when its literal range (or a let of its counter just before,
// as before, and literal bounds and step) says; -1 when it does not
std::int64_t KnownTrips(const Pass& p, ExprId loop, ExprId before);
// a loop whose body surely runs once each time it is reached: a for with a literal range
// that is not empty or that runs to the type's end, a (while true)
bool BodyRuns(const Pass& p, ExprId loop);
// sets, lets and fors of var under id
std::uint32_t CountSets(const Pass& p, ExprId id, std::int32_t var);

} // namespace tisple::opt
