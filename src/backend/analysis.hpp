#pragma once

// Facts about expressions the code generator needs: effects, constants, sameness.

#include "check/program.hpp"

namespace tisple {

const Expr& ExprOf(const Program& program, ExprId id);
ExprId ListItem(const Program& program, const Expr& e, std::uint32_t i);
TypeKind ExprKindOf(const Program& program, ExprId id);
std::uint32_t ExprSize(const Program& program, ExprId id); // bytes of its type

// may write memory: calls, stores, sets (a let only writes a fresh local)
bool HasWrites(const Program& program, ExprId id);
bool HasCalls(const Program& program, ExprId id);
// anything that must run even when the value is unused: writes, I/O, control flow
bool HasEffects(const Program& program, ExprId id);
// may this code change var: a set rooted at it, or for globals and address-taken vars any
// call or store
bool MayWriteVar(const Program& program, ExprId id, std::int32_t var);

// reads var anywhere inside (any VarRef to it)
bool ReadsVar(const Program& program, ExprId id, std::int32_t var);
// reads memory through a pointer
bool HasLoads(const Program& program, ExprId id);

// the raw bits of a constant expression (folded with the target's arithmetic)
bool ConstValue(const Program& program, ExprId id, std::uint16_t& value);

// the same pure place or value, written the same way
bool SameExpr(const Program& program, ExprId a, ExprId b);

// how many expressions make up id (a size measure for inlining)
std::uint32_t CountExprs(const Program& program, ExprId id);

// pointer variables some load or store goes through directly: these live in zero page
std::vector<bool> FindDerefPointers(const Program& program);

} // namespace tisple
