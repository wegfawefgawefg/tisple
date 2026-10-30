#pragma once

// Shared by the mid-level passes: the pass state, effect summaries, liveness and tree edits.

#include "opt/opt.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace tisple::opt {

// ---- sets of variables, one bit each

using Bits = std::vector<std::uint64_t>;

inline bool HasBit(const Bits& bits, std::int32_t bit) {
    std::size_t b = static_cast<std::size_t>(bit);
    return (bits[b / 64] >> (b % 64)) & 1;
}
inline void SetBit(Bits& bits, std::int32_t bit) {
    std::size_t b = static_cast<std::size_t>(bit);
    bits[b / 64] |= std::uint64_t(1) << (b % 64);
}
inline void ClearBit(Bits& bits, std::int32_t bit) {
    std::size_t b = static_cast<std::size_t>(bit);
    bits[b / 64] &= ~(std::uint64_t(1) << (b % 64));
}
inline void OrBits(Bits& into, const Bits& from) {
    for (std::size_t i = 0; i < into.size(); i++) {
        into[i] |= from[i];
    }
}

// ---- effect summaries (summary.cpp)

// what running a function may do, its callees included
struct Summary {
    std::vector<bool> reads;  // by var: globals it reads
    std::vector<bool> writes; // by var: globals it sets
    bool loads = false;       // reads through a pointer: address-taken vars, undeclared memory
    bool stores = false;      // writes through a pointer
    bool io = false;          // io_read or io_write
    bool jsr = false;         // machine code: may read and write any global
    bool unknown = false;     // a node this analysis does not know: every effect
    bool terminates = true;   // every loop in it provably ends
    bool pure = false;        // none of the effects above, and it ends: see PureFunction
};

// ---- value ranges of variables (ranges_vars.cpp)

// mathematical values, lo to hi
struct Range {
    std::int64_t lo = 0;
    std::int64_t hi = 0;
};

// what one round knows of variables before its passes run: every value each variable ever
// holds, and which bytes of arrays and structs any store may write
struct VarRanges {
    bool vars = false; // natural and other below hold (--no-pass vars clears it)
    // by var: a range every value of it lies in, in its type's terms (u16 0..65535), and the
    // same in the other signedness (a u16 of 65535 or 1 is -1..1 there); the type's range
    // when nothing is known. Variables made later in the round are not in them
    std::vector<Range> natural;
    std::vector<Range> other;
    bool elements = false; // stored below holds (--no-pass elements clears it)
    // by var: the byte ranges (packed offsets) a store may write, for arrays and structs that
    // only sets reach; unstored[var] false for the others
    std::vector<std::vector<Range>> stored;
    std::vector<bool> unstored;
};

// ---- the pass state

struct Pass {
    Program* program = nullptr;
    const OptOptions* options = nullptr;
    OptReport* report = nullptr;
    std::vector<std::uint32_t> refs; // by expr: how many nodes (or function bodies) name it
    std::vector<Summary> summaries;  // by function
    // calls through function pointers, by the pointer's type: every function they may reach
    // (CallTargets), and the union of those functions' summaries (every effect with none)
    std::map<TypeId, std::vector<std::int32_t>> targets;
    std::map<TypeId, Summary> pointer_summaries;
    VarRanges ranges; // worked out at the start of each round
    // the ranges pass's memo of ValueRange by expr, good while the counters and facts around
    // the walk are those of stamp now (ranges.cpp bumps it at each change)
    struct RangeMemo {
        bool on = false;
        std::uint32_t now = 1;
        std::vector<std::uint32_t> stamp;
        std::vector<Range> range;
    };
    mutable RangeMemo memo;
    // collapse's walk only unrolls short loops and unswitches loops (once the passes settle)
    bool unroll = false;
    bool unswitch = false;
    // compile-time evaluation (eval.cpp), over every round: the interpreter steps spent, and
    // each call tried (the function, then its arguments' bits) with its value, -1 when it
    // could not be worked out
    std::uint64_t eval_steps = 0;
    // inlining (inline.cpp), over every round: the nodes copied
    std::uint32_t inline_copied = 0;
    std::map<std::vector<std::uint32_t>, std::int32_t> evaluated;
    // the last transformation made, for --opt-bisect
    const char* last_what = "";
    ExprId last_at = -1;
    std::int32_t last_var = -1;
};

const Expr& At(const Pass& p, ExprId id);
ExprId Item(const Pass& p, const Expr& e, std::uint32_t i);
const Var& VarOf(const Pass& p, std::int32_t var);

// may this transformation be made? Counts it; false once --opt-bisect's budget is spent
bool Allow(Pass& p, const char* what, ExprId at, std::int32_t var);

void ComputeSummaries(Pass& p);
// a call that can be dropped when its value is unused: writes no global, no memory through a
// pointer, no I/O or jsr, and ends
bool PureFunction(const Pass& p, std::int32_t function);
// what a Call or CallPtr may do: its callee's summary, or the union over a pointer's targets
const Summary& CallSummary(const Pass& p, const Expr& call);
// the functions a call through a pointer of this type may reach
const std::vector<std::int32_t>& PointerTargets(const Pass& p, TypeId type);
// a node whose list holds children: a call, direct or through a pointer, a struct value, a do
inline bool HasItems(ExprKind kind) {
    return kind == ExprKind::Call || kind == ExprKind::CallPtr || kind == ExprKind::StructLit ||
           kind == ExprKind::Do;
}
// no effect at all, and ends: dropped when its value is unused
bool IsPure(const Pass& p, ExprId id);
// the place's index expressions (its root var is written, not read) have no effect
bool PurePlace(const Pass& p, ExprId place);
// the var a set's place is rooted at
std::int32_t PlaceRoot(const Pass& p, ExprId place);

// ---- termination (terminate.cpp)

// every loop and call inside id provably ends: every for does, a while when a counter moves
// monotonically towards its bound each pass
bool CodeTerminates(const Pass& p, ExprId id);
// this while's condition alone ends it (loops and calls inside are not looked at)
bool WhileEnds(const Pass& p, ExprId loop);
// a continue inside id that goes to the loop id sits in (depth 0), not to a loop inside it
bool ContinuesHere(const Pass& p, ExprId id, int depth);

// ---- liveness (live.cpp)

// one function's variables whose every read and write the tree shows: its own locals and
// params and the plain globals (no address taken, no :address, not in aux)
struct Liveness {
    std::vector<std::int32_t> bit; // by var: its bit, or -1 when not tracked
    std::int32_t count = 0;
    // live right after each set and let, and after each loop
    std::unordered_map<ExprId, Bits> after;
};

// every read and write of var in function shows in the tree (see Liveness)
bool TrackedVar(const Var& var, std::int32_t function);
Liveness ComputeLiveness(const Pass& p, std::int32_t function);
// may var be read after id runs? True for untracked vars and nodes liveness never reached
bool LiveAfter(const Liveness& live, ExprId id, std::int32_t var);

// ---- tree edits (edit.cpp)

template <typename Fn> void EachChild(const Pass& p, const Expr& e, Fn fn) {
    for (ExprId child : {e.a, e.b, e.c}) {
        if (child >= 0) {
            fn(child);
        }
    }
    if (HasItems(e.kind)) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            fn(Item(p, e, i));
        }
    }
    if (e.kind == ExprKind::Match) {
        for (std::uint32_t i = 0; i < e.count; i++) {
            fn(p.program->clauses[e.first + i].body);
        }
    }
}

void CountRefs(Pass& p);
void Retain(Pass& p, ExprId id);
// one name fewer; a node nobody names gives up its children and becomes a plain literal
void Release(Pass& p, ExprId id);
// named by more than one parent (or by none): never rewritten
bool Shared(const Pass& p, ExprId id);
// a new node, named by nobody yet; its children are retained
ExprId NewExpr(Pass& p, const Expr& e);
ExprId NewEmptyDo(Pass& p, const Loc& loc);
// a new do (or other list node) of these items, named by nobody yet
ExprId NewList(Pass& p, ExprKind kind, TypeId type, const std::vector<ExprId>& items,
               const Loc& loc);
// id takes from's content (from is often one of id's children); id keeps its loc
void ReplaceWith(Pass& p, ExprId id, ExprId from);
// id becomes a literal of its own type; its children are released
void ReplaceWithLiteral(Pass& p, ExprId id, std::uint16_t bits);
// a fresh local of function, for a value a pass computes once
std::int32_t NewLocal(Pass& p, std::int32_t function, TypeId type, const Loc& loc);
// id's content becomes a read of var; its old children are let go
void BecomeRead(Pass& p, ExprId id, std::int32_t var);
// (let var value) for an expression moved out of its place, which now reads var, a new local
// of function; the let is named by nobody yet
// (loc is a copy: the new nodes may move every expression)
ExprId MoveToLet(Pass& p, std::int32_t function, ExprId id, Loc loc, std::int32_t& var);

// what a pass does with one statement (a form whose value is unused)
struct Action {
    enum Kind : std::uint8_t { Keep, Drop, Replace } kind = Keep;
    ExprId with = -1;      // Replace: the sub-expression to run instead
    const char* what = ""; // for --opt-bisect: "dead let", "dead loop", ...
    std::int32_t var = -1; // the variable it is about, or -1
};

using Decide = Action (*)(const Pass& p, const Liveness& live, ExprId stmt);

// every statement of function's body (do items whose value is unused, arms of an if or match
// whose value is unused) offered to decide, outermost first; returns the changes made
std::uint64_t EditStatements(Pass& p, std::int32_t function, Decide decide);

// ---- passes

// calls the backend would inline become copies of their function's body (inline.cpp); returns
// the changes made
std::uint64_t InlineCalls(Pass& p);

Action DecideDead(const Pass& p, const Liveness& live, ExprId stmt); // dead.cpp
Action DecideLoop(const Pass& p, const Liveness& live, ExprId stmt); // loops.cpp
// constant propagation and folding over each function's tracked scalars, branches on
// constants cut (consts.cpp, consts_walk.cpp); returns the changes made
std::uint64_t PropagateConstants(Pass& p);
// arguments for parameters their function never reads become 0 (args.cpp); returns the
// changes made
std::uint64_t DropUnreadArgs(Pass& p);
// pure return values of functions whose call values nothing uses become 0 (args.cpp);
// returns the changes made
std::uint64_t DropUnusedReturns(Pass& p);
// loops whose passes all do the same run their body once (collapse.cpp); returns the
// changes made
std::uint64_t CollapseLoops(Pass& p);
// compares and match clauses decided by value ranges (ranges.cpp); returns the changes made
std::uint64_t FoldRanges(Pass& p);
// the ranges of variables and the stored bytes of arrays (ranges_vars.cpp), into p.ranges
void ComputeVarRanges(Pass& p, bool vars, bool elements);
// the mathematical values id may hold from its shape alone (ranges.cpp): literals, masks,
// remainders, shifts, sums that cannot wrap, widenings; the type's range when it says nothing
void ShapeRange(const Pass& p, ExprId id, std::int32_t& lo, std::int32_t& hi);
// loop-invariant code motion (licm.cpp); returns the changes made
std::uint64_t HoistInvariants(Pass& p, std::int32_t function);
// common subexpressions (cse.cpp, cse_walk.cpp); returns the changes made
std::uint64_t CommonSubexpressions(Pass& p, std::int32_t function);
// calls of pure functions with constant arguments worked out at compile time (eval.cpp);
// returns the changes made
std::uint64_t EvaluateCalls(Pass& p);

} // namespace tisple::opt
