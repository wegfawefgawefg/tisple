#pragma once

#include "check/load.hpp"
#include "check/program.hpp"

namespace tisple {

// resolves and type-checks an expanded program; the loaded program is moved into out
bool CheckProgram(LoadedProgram& loaded, Program& out, std::vector<Diagnostic>& errors);

// load, expand and check in one go; errors hold every problem found
bool BuildProgram(const std::string& entry_path, const LoadOptions& options, Program& out,
                  std::vector<Diagnostic>& errors);

} // namespace tisple
