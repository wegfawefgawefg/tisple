#pragma once

namespace tisple {

// tisple fmt [-w | --check] file.tsp...
int RunFmtCommand(int argc, char** argv);
// tisple show [--no-color] file.tsp...
int RunShowCommand(int argc, char** argv);
// tisple gen-editors <editors dir>: rewrite the generated editor grammars
int RunGenEditorsCommand(int argc, char** argv);

} // namespace tisple
