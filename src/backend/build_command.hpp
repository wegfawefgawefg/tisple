#pragma once

namespace tisple {

// tisple build main.tsp [-o out.bin] [--origin A] [--asm]
int RunBuildCommand(int argc, char** argv);
// tisple run main.tsp [--max-cycles N] [--origin A]
int RunRunCommand(int argc, char** argv);

} // namespace tisple
