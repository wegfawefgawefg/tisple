#pragma once

#include "check/load.hpp"
#include "check/name_index.hpp"
#include "check/program.hpp"

namespace tisple {

// resolves and type-checks an expanded program; the loaded program's sources move into out.
// names, when given, gets every written name the checker resolves (editor tools)
bool CheckProgram(LoadedProgram& loaded, Program& out, std::vector<Diagnostic>& errors,
                  NameIndex* names = nullptr);

// load, expand and check in one go; errors hold every problem found
bool BuildProgram(const std::string& entry_path, const LoadOptions& options, Program& out,
                  std::vector<Diagnostic>& errors);

} // namespace tisple
