#pragma once

#include "backend/backend.hpp"
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
// the memory a boot disk's program may fill (no DOS): up to $BFFF, and from $0B00 below the
// origin; tisple check --disk lays a program out this way too
void UseBootDiskLayout(BuildOptions& options);

// a self-booting disk image file for a program's segments (the first holds entry), with a
// line saying what is on it
bool WriteBootDisk(const std::string& path, const std::vector<MemorySegment>& segments,
                   uint16_t entry, BootDiskInfo& info, std::string& error,
                   const BootExtras& extras = BootExtras());

} // namespace tisple
