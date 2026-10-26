#pragma once

#include "machine/window.hpp"

namespace tisple {

// tisple build main.tsp [-o out.bin] [--origin A] [--asm]
int RunBuildCommand(int argc, char** argv);
// tisple run main.tsp [--origin A] [machine flags...]: build, then run on the built-in
// apple iie, in a window unless headless flags (--cycles, --frames, --screenshot, --wav,
// --headless) say otherwise. --sim [--max-cycles N] runs main in the bare simulator instead.
// window is null when the build has no SDL3.
int RunRunCommand(int argc, char** argv, MachineWindowFn window);

} // namespace tisple
