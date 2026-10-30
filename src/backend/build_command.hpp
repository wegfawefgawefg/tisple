#pragma once

#include "backend/backend.hpp"
#include "machine/window.hpp"

#include <string>

namespace tisple {

// the code flags every build takes (build, run, check, and their --disk forms): -O0/-O1,
// --opt-bisect N, --no-pass NAME, --no-inline module.fn. True when argv[i] is one (i then on
// its last word); error says what was wrong with its value
bool ParseCodeFlag(int argc, char** argv, int& i, BuildOptions& options, std::string& error);

// tisple build main.tsp [-o out.bin] [--origin A] [--asm]
int RunBuildCommand(int argc, char** argv);
// tisple run main.tsp [--origin A] [machine flags...]: build, then run on the built-in
// apple iie, in a window unless headless flags (--cycles, --frames, --screenshot, --wav,
// --headless) say otherwise. --sim [--max-cycles N] runs main in the bare simulator instead.
// window is null when the build has no SDL3.
int RunRunCommand(int argc, char** argv, MachineWindowFn window);

} // namespace tisple
