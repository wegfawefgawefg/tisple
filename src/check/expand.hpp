#pragma once

#include "check/load.hpp"
#include "check/name_index.hpp"

namespace tisple {

// Macros, expanded tree to tree before checking: a macro body is code that runs in the
// compiler (src/macro/) and gives the forms to paste in.
// Removes every (macro ...) form and replaces every call with its expansion.
// Forms a macro makes get the call's span and an expansion id (Node::expansion), so
// errors point at the call and name the macro; argument forms keep their own spans.
// names, when given, gets every macro, its parameters and every call's head (editor tools).
bool ExpandProgram(LoadedProgram& program, std::vector<Diagnostic>& errors,
                   NameIndex* names = nullptr);

constexpr int kMaxExpansionDepth = 64;
constexpr std::uint32_t kMaxExpansions = 100000;

} // namespace tisple
