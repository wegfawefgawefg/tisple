#pragma once

// the flags of `tisple machine` (and of `tisple run`, which passes its machine flags on)

#include "disk/machine_disks.hpp"
#include "machine/apple2.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tisple {

constexpr uint64_t kDefaultCycles = 100000000; // about 98 seconds of apple time

struct ScreenshotArg {
    std::string path;
    uint64_t cycle = 0;
    bool at_end = true;
};

struct MachineArgs {
    const char* program = nullptr;
    const char* rom = nullptr;
    const char* load_snapshot = nullptr;
    const char* save_snapshot = nullptr;
    const char* wav = nullptr;
    const char* keys = nullptr;
    const char* mouse = nullptr;     // scripted mouse input
    const char* mouse_rom = nullptr; // the owner's MouseInterface.rom; null for ours
    int mockingboard = 0;            // its slot; 0 for none
    uint32_t origin = kDefaultOrigin;
    uint64_t cycles = kDefaultCycles;
    bool cycles_given = false;
    VideoLook look = VideoLook::kColor;
    bool window = false;
    int scale = 2;
    std::vector<ScreenshotArg> screenshots;
    std::vector<std::pair<uint32_t, uint32_t>> peeks; // address, byte count
    MachineDisks disks;
};

void PrintMachineUsage();

// want_program: the first argument that is not a flag is the binary to run
bool ParseMachineArgs(int argc, char** argv, bool want_program, MachineArgs& args);

} // namespace tisple
