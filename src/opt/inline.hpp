#pragma once

// Inlining at the mid level: inline.cpp picks the calls and walks the functions,
// inline_copy.cpp makes each copy.

#include "opt/opt_internal.hpp"

namespace tisple::opt {

enum class CopyMode : std::uint8_t {
    Value,     // the copy's value is the call's
    Statement, // nobody uses it
    SetVar,    // it goes into a variable
};

// may the function's body become a copy with no returns
bool Lowerable(Pass& p, std::int32_t callee);
// the copy of call, made in caller: the lets of its parameters, then the body's items, into
// out, each named once; dest is the variable a SetVar copy sets
void CopyCall(Pass& p, std::int32_t caller, ExprId call, CopyMode mode, std::int32_t dest,
              std::vector<ExprId>& out);

} // namespace tisple::opt
