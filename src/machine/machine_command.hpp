#pragma once

// `tisple machine`: run a raw binary (or a rom) in the built-in apple iie

#include "link/segment.hpp"
#include "machine/window.hpp"

#include <cstdint>
#include <vector>

namespace tisple {

// window is null when the build has no SDL3. --disk a.dsk boots a disk (disk/machine_disks)
int RunMachineCommand(int argc, char** argv, MachineWindowFn window);

// for `tisple run`: the binary is already in memory, with the segments of a program that
// did not fit one file; argv holds only machine flags
// (--cycles, --frames, --keys, --screenshot, --wav, --mono, --window, --scale, --rom, ...)
int RunMachineOnBinary(const std::vector<uint8_t>& binary, uint16_t origin,
                       const std::vector<MemorySegment>& segments, int argc, char** argv,
                       MachineWindowFn window);

// for `tisple run --disk`: a disk image in memory goes in drive 1 and boots
int RunMachineOnDisk(const std::vector<uint8_t>& image, DiskFormat format, int argc, char** argv,
                     MachineWindowFn window);

} // namespace tisple
