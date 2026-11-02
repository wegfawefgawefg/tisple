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
    const char* taps_at = nullptr;   // scripted taps, as a phone playtest log has them
    const char* mouse_rom = nullptr; // the owner's MouseInterface.rom; null for ours
    const char* joystick = nullptr;  // scripted paddles 0-1 and buttons
    MachineModel model = MachineModel::kIIeEnhanced; // what the rom-less machine says it is
    // the cpu, when --cpu gives it; else the rom's model's, or --model's (ModelCpu)
    Cpu cpu = Cpu::k65c02;
    bool cpu_given = false;
    bool needs_65c02 = false; // tisple run of a 65C02 build: refuse a 6502 machine
    bool aux_ram = true;      // false: --64k
    int mockingboard = 0;     // its slot; 0 for none
    uint32_t origin = kDefaultOrigin;
    uint64_t cycles = kDefaultCycles;
    bool cycles_given = false;
    VideoLook look = VideoLook::kColor;
    bool window = false;
    bool drive_sound = true; // the disk drives' sound in the window and --wav
    int scale = 2;
    const char* taps = nullptr; // a game's player config: clicks press its tap boxes' keys
    std::vector<ScreenshotArg> screenshots;
    std::vector<std::pair<uint32_t, uint32_t>> peeks; // address, byte count
    ScreenshotArg profile; // --profile F.txt[@C]: path empty for none (machine/profile.hpp)
    MachineDisks disks;
};

void PrintMachineUsage();

// the cpu a machine set up from args runs: --cpu's, else the loaded rom's model's, else
// --model's
Cpu MachineCpu(const Apple2& a, const MachineArgs& args);

// want_program: the first argument that is not a flag is the binary to run
bool ParseMachineArgs(int argc, char** argv, bool want_program, MachineArgs& args);

} // namespace tisple
