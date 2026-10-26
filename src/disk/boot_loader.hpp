#pragma once

// self-booting disks: our own loader on track 0 reads a program to its origin and calls it.
// no DOS. it boots with Apple's disk ii rom on a real machine and with ours in the emulator.
//
// License: the loader's 6502 code is copied onto every disk `tisple disk boot` makes, so
// besides GPL-3.0-or-later it is offered under MIT (the runtime exception in COPYRIGHT). It
// was written for Tisple from the disk format and the boot interface every disk ii boot rom
// offers, and is not derived from Apple's boot code or any other project's. Keep it that way.
//
// the disk:
// - track 0, physical sectors 0 and 1: the loader. its first byte is 2, so the boot rom
//   reads both to $0800-$09FF and jumps to $0801 with X = slot * 16. that is all it takes
//   from the rom: it turns the motor on itself and steps and reads with its own code.
// - tracks 1 on: the program, from the page holding its origin, 16 pages a track in
//   physical sector order. the pages around the program are zeros.
// - the rest is an empty DOS 3.3 volume (VTOC and catalog on track 17) with the tracks
//   above marked in use, so `tisple disk put` can add files beside it.
// at run time the loader keeps to $0800-$09FF, $0296-$0365 (its nibble table and scratch,
// clear of the vectors at $03F0) and zero page $DF-$EF. then it sets the stack to
// $FF, A = X = Y = 0, interrupts off, the language card to $C082 (rom reads, as a direct
// load starts), turns the motor off and calls the origin. if the program returns, the loader
// shows the text screen and enters the monitor ($FF69); the emulator stops there instead.

#include "disk/nibble.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// the program must load in $0A00-$BFFF: the loader is at $0800-$09FF while it runs
constexpr uint32_t kBootLoadLow = 0x0A00;
constexpr uint32_t kBootLoadHigh = 0xC000;
constexpr int kBootFirstTrack = 1;

struct BootDiskInfo {
    uint16_t return_pc = 0; // where the program's final RTS lands
    uint16_t first_page = 0;
    uint32_t pages = 0;
    int last_track = 0;
};

bool MakeBootDisk(const std::vector<uint8_t>& program, uint16_t origin, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error);

// "track 1" or "tracks 1-3"
std::string BootTracksText(const BootDiskInfo& info);

// whether a disk's track 0 holds a loader of ours; its return pc if so
bool FindBootLoader(const DiskTrack& track0, uint16_t& return_pc);

} // namespace tisple
