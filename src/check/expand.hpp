#pragma once

#include "check/load.hpp"
#include "check/name_index.hpp"

namespace tisple {

// Common Lisp-style template macros, expanded tree to tree before checking.
// Removes every (macro ...) form and replaces every call with its expansion.
// Template forms get the call's span and an expansion id (Node::expansion), so
// errors point at the call and name the macro; argument forms keep their own spans.
// names, when given, gets every macro, its parameters and every call's head (editor tools).
bool ExpandProgram(LoadedProgram& program, std::vector<Diagnostic>& errors,
                   NameIndex* names = nullptr);

constexpr int kMaxExpansionDepth = 64;
constexpr std::uint32_t kMaxExpansions = 100000;

} // namespace tisple
