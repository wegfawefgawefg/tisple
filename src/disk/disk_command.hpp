#pragma once

#include "disk/boot_loader.hpp"
#include "machine/window.hpp"

#include <string>
#include <vector>

namespace tisple {

// tisple disk new|list|put|get|delete|boot ...
int RunDiskCommand(int argc, char** argv);

// `tisple build main.tsp --disk out.dsk` and `tisple run main.tsp --disk`: main.cpp sends
// build and run here when --disk is among their arguments
int RunBuildDiskCommand(int argc, char** argv);
int RunRunDiskCommand(int argc, char** argv, MachineWindowFn window);
bool HasDiskFlag(int argc, char** argv);

// a self-booting disk image file for a program, with a line saying what is on it
bool WriteBootDisk(const std::string& path, const std::vector<uint8_t>& program, uint16_t origin,
                   BootDiskInfo& info, std::string& error);

} // namespace tisple
