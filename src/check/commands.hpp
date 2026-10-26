#pragma once

namespace tisple {

// tisple check file.tsp: every error, or "ok"
int RunCheckCommand(int argc, char** argv);
// tisple expand file.tsp: the program after macro expansion, through the formatter
int RunExpandCommand(int argc, char** argv);

} // namespace tisple
