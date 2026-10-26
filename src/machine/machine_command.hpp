#pragma once

// `tisple machine`: run a raw binary (or a rom) in the built-in apple iie

#include "machine/window.hpp"

#include <cstdint>
#include <vector>

namespace tisple {

// window is null when the build has no SDL3
int RunMachineCommand(int argc, char** argv, MachineWindowFn window);

// for `tisple run`: the binary is already in memory; argv holds only machine flags
// (--cycles, --frames, --keys, --screenshot, --wav, --mono, --window, --scale, --rom, ...)
int RunMachineOnBinary(const std::vector<uint8_t>& binary, uint16_t origin, int argc, char** argv,
                       MachineWindowFn window);

} // namespace tisple
