#pragma once

// Loop collapse: collapse_scan.cpp finds loops whose every pass does the same, collapse.cpp
// runs their body once in their place.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

// what a pointer may reach (collapse_memory.cpp)
struct Reach {
    bool any = false;        // any address-taken variable
    bool undeclared = false; // memory no variable uses
    std::vector<std::int32_t> vars;
};

// what pointers reach, worked out as loops need it during one round of the passes
struct MemoryFacts {
    std::unordered_map<std::int32_t, Reach> locals;  // by pointer local or parameter
    std::unordered_map<std::int32_t, Reach> returns; // by function returning a pointer
    std::vector<Reach> loads;                        // by function, its callees included
    std::vector<Reach> stores;
    std::vector<bool> done;                 // by function: loads and stores worked out
    std::vector<std::vector<ExprId>> calls; // by function: the calls of it
};

// what a loop body does through pointers, and the variables (not its own) it reads and writes
struct MemoryUse {
    std::vector<ExprId> load_ptrs; // pointers loaded through
    std::vector<ExprId> store_ptrs;
    std::vector<std::int32_t> calls; // functions called that load or store
    std::vector<std::int32_t> read;
    std::vector<std::int32_t> written;
};

// may a store reach what the body reads or loads, or a load what it writes; memory no
// variable uses (hardware may be behind it) loaded or stored counts as a clash
bool MemoryClash(const Pass& p, MemoryFacts& m, const MemoryUse& use);
// may a store through a pointer in function (its callees included) reach var
bool StoresMayReach(const Pass& p, MemoryFacts& m, std::int32_t function, std::int32_t var);

// ---- what a loop body reads and writes (collapse_scan.cpp)

// bytes [lo, hi) of a variable, in its packed layout; with a stride, bytes [lo, hi) of each
// stride-byte element (a field of an element whose index is not known, slice 6)
struct Cell {
    std::int32_t var = -1;
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    std::uint32_t stride = 0;
    bool steady = true; // a write every pass makes to the same place (see Access)
};

// a loop body bigger than this many nodes is not looked at (the compile stays fast)
constexpr int kMaxScan = 3000;

struct Access {
    int budget = kMaxScan; // nodes left to scan
    std::vector<Cell> reads;
    std::vector<Cell> writes;
    std::vector<bool> own; // by var: let or looped over inside the body
    MemoryUse memory;      // through pointers
    bool all = false;      // I/O, jsr, wait_cycles, hardware variables, nodes not known here
    // for a body that reads its loop's counter: what may differ from pass to pass is the
    // counter and the body's own variables but the steady ones (a let of what does not differ,
    // set nowhere else; the counter of a for inside whose range does not differ); varying is
    // set under a branch on what differs and in loops inside that may not make the same passes,
    // where a write may not happen on every pass; unsteady: a store or a call's write that may
    // not land in the same place on every pass
    ExprId body = -1;
    std::int32_t counter = -1;
    std::vector<bool> steady; // by var: an own variable that is the same on every pass
    // globals a pass sets before reading them (own, see collapse_plan.cpp), which a call may
    // read; an own variable whose address is taken, which a load may read
    std::vector<std::int32_t> own_globals;
    bool own_pointed = false;
    bool varying = false;
    bool unsteady = false;
};

// :address, aux or :reserve: hardware may be behind it
bool Hardware(const Var& v);
// a variable a let or for inside the body declares
bool Own(const Access& a, std::int32_t var);
// does id read the counter or an own variable that is not steady: directly, in a call (the
// globals its summary reads) or through a pointer (a load, when an own variable's address
// is taken)
bool Varies(const Pass& p, const Access& a, ExprId id);
// what id reads and writes, into a
void Scan(const Pass& p, Access& a, ExprId id);

// ---- what collapsing a loop body takes (collapse_plan.cpp)

// a statement (set x (op x k)) among a loop body's items, x read and written nowhere else in
// the loop and k the same on every pass: after n passes x is (+ x (* k n)), (- x (* k n)),
// (^ x k) for odd n, or (| x k) and (& x k) for any n of at least one
struct Accumulator {
    ExprId stmt = -1;
    std::int32_t var = -1;
    Op op = Op::Add;
    ExprId step = -1; // k
};

// what a loop body does on each pass, as far as collapsing it goes
struct BodyPlan {
    bool same = false; // every pass reads what no pass writes: one pass does what all do
    // the body reads the loop's counter, and every pass writes the same places: the last pass
    // does what all do
    bool last = false;
    std::vector<Accumulator> accumulators;
};

// the statements a loop body runs on every pass, in order: its items, and the items of dos
// among them
void PassStatements(const Pass& p, ExprId body, std::vector<ExprId>& out);

// the plan for a loop body (a do or one form). counter: the loop's own variable, which the
// rest of the body may neither read nor write (a for's variable, a while's counter), or -1;
// skip: a statement among the items left out (the while counter's step), or -1
BodyPlan PlanBody(const Pass& p, MemoryFacts& m, ExprId body, std::int32_t counter, ExprId skip);

// ---- around collapse: continues, counts at run time, unrolling

// a loop body's (if c (continue)) items: the rest of the pass becomes their else
// (collapse_continue.cpp); returns the changes made
std::uint64_t ContinuesToElse(Pass& p, ExprId loop);

// a copy of the tree at id, named by nobody yet (unroll.cpp); with a function, the variables
// its lets and fors declare get new locals of it (names maps the old to the new, and may
// come with some already)
ExprId CloneExpr(Pass& p, ExprId id);
ExprId CloneExpr(Pass& p, ExprId id, std::int32_t function,
                 std::unordered_map<std::int32_t, std::int32_t>& names);
// a new local of function like var (its type and flags), for a copy's own let or for
std::int32_t CopyLocal(Pass& p, std::int32_t function, std::int32_t var);
// accumulators that must know how many passes there are (not | and &), and whether a count
// worked out at run time does for them all (not ^, which needs to know it is odd)
bool NeedsCount(const BodyPlan& plan);
bool CountWillDo(const BodyPlan& plan);
// a for whose pass count b - a may be worked out from copies of its bounds before it, and
// the let that does, of a new u16 local var of function (collapse_count.cpp)
bool CountableBounds(const Pass& p, ExprId loop);
ExprId CountLet(Pass& p, std::int32_t function, ExprId loop, std::int32_t& var);

// a while that only steps its counter by 1 while (< c k) and what the loop never changes hold,
// as (if test (set c k)) (collapse_exit.cpp); false when it stays
bool ScanToEnd(Pass& p, ExprId loop, ExprId before, std::vector<ExprId>& out);
// a short loop as a copy of its body per pass (unroll.cpp): what stands for it, named by
// nobody yet; false when it stays. before: the item before it, for a counting while
bool UnrollLoop(Pass& p, std::int32_t function, ExprId loop, ExprId before,
                std::vector<ExprId>& out);
// a for whose body branches on what the loop never changes, as that branch with a copy of the
// loop in each arm (unswitch.cpp); false when it stays
bool UnswitchLoop(Pass& p, MemoryFacts& m, std::int32_t function, ExprId loop,
                  std::vector<ExprId>& out);

} // namespace tisple::opt
