#pragma once

#include "check/load.hpp"

namespace tisple {

// Common Lisp-style template macros, expanded tree to tree before checking.
// Removes every (macro ...) form and replaces every call with its expansion.
// Template forms get the call's span and an expansion id (Node::expansion), so
// errors point at the call and name the macro; argument forms keep their own spans.
bool ExpandProgram(LoadedProgram& program, std::vector<Diagnostic>& errors);

constexpr int kMaxExpansionDepth = 64;
constexpr std::uint32_t kMaxExpansions = 100000;

} // namespace tisple
