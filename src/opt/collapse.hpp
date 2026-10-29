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

} // namespace tisple::opt
