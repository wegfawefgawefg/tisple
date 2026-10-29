#pragma once

// Facts about expressions the code generator needs: effects, constants, sameness.

#include "check/program.hpp"

#include <string>
#include <vector>

namespace tisple {

const Expr& ExprOf(const Program& program, ExprId id);
ExprId ListItem(const Program& program, const Expr& e, std::uint32_t i);
TypeKind ExprKindOf(const Program& program, ExprId id);
std::uint32_t ExprSize(const Program& program, ExprId id); // bytes of its type

// may write memory: calls, stores, sets, jsr (a let only writes a fresh local)
bool HasWrites(const Program& program, ExprId id);
bool HasCalls(const Program& program, ExprId id);
// a while or for loop inside
bool HasLoops(const Program& program, ExprId id);
// anything that must run even when the value is unused: writes, I/O, control flow
bool HasEffects(const Program& program, ExprId id);
// may this code change var: a set rooted at it, or for globals and address-taken vars any
// call, store or jsr
bool MayWriteVar(const Program& program, ExprId id, std::int32_t var);

// reads var anywhere inside (any VarRef to it)
bool ReadsVar(const Program& program, ExprId id, std::int32_t var);
// reads memory through a pointer
bool HasLoads(const Program& program, ExprId id);

// the same pure place or value, written the same way
bool SameExpr(const Program& program, ExprId a, ExprId b);

// how many expressions make up id (a size measure for inlining)
std::uint32_t CountExprs(const Program& program, ExprId id);

// constant parts of arithmetic combined, in the types' wrapping arithmetic: (+ (+ 5 t) 2) is
// (+ t 7), (* x 1) is x, (+ (+ p 0) 2) is (+ p 2), a constant tree a literal (fold.cpp)
void FoldConstants(Program& program);

// by var: the const tables that functions reachable from main (all functions, with no main)
// name. Run on the program as written, these must keep their memory: the interpreter reads
// them even where the mid-level passes folded every read away
std::vector<bool> FindNamedTables(const Program& program);

// functions this small (in expressions, CountExprs) are inlined at every call (gen_expr.cpp)
constexpr std::uint32_t kMaxInlineExprs = 24;

enum class InlineKind : std::uint8_t {
    None,
    Always, // at every call: at most kMaxInlineExprs expressions
    Once,   // the program calls it in one place: there, the first time that call is made
};

// by function: how its calls are inlined. never names functions that stay functions
// (qualified names; the race's t.init and t.checksum, whose entries time the work)
std::vector<InlineKind> FindInlines(const Program& program, const std::vector<std::string>& never);
// calls a function that is not inlined at every call (a JSR, or the one inlined copy)
bool HasBigCalls(const Program& program, ExprId id, const std::vector<InlineKind>& inlines);

// pointer variables some load or store goes through directly, or that an inlined function's
// parameter some load or store goes through reads in place once the call is inlined: these
// live in zero page
std::vector<bool> FindDerefPointers(const Program& program, const std::vector<InlineKind>& inlines);

// how many times a loop's body may run each time the loop is reached: a for with constant
// bounds its trip count (2 to 64), a while 8, any other loop 4 (run_counts.cpp)
std::uint32_t LoopTrips(const Program& program, ExprId loop);
// by function: how many times it may run in one run of the program, from the calls to it and
// the loops around them (at most 2^32; 0 for a function main never reaches)
std::vector<std::uint64_t> FindRunCounts(const Program& program);

// by var: locals every let and set gives 1 or -1 (unit_steps.cpp)
std::vector<bool> FindUnitSteps(const Program& program);

} // namespace tisple
