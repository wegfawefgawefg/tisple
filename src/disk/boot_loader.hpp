#pragma once

// self-booting disks: our own loader on track 0 reads a program's segments and calls it.
// no DOS. it boots with Apple's disk ii rom on a real machine and with ours in the emulator.
//
// License: the loader's 6502 code is copied onto every disk `tisple disk boot` makes, so
// besides GPL-3.0-or-later it is offered under MIT (the runtime exception in COPYRIGHT). It
// was written for Tisple from the disk format and the boot interface every disk ii boot rom
// offers, and is not derived from Apple's boot code or any other project's. Keep it that way.
//
// the disk:
// - track 0, physical sectors 0-2: the loader. its first byte is 3, so the boot rom
//   reads them to $0800-$0AFF and jumps to $0801 with X = slot * 16. that is all it takes
//   from the rom: it turns the motor on itself and steps and reads with its own code.
// - tracks 1 on: the program's segments, each from the page holding its start and from a
//   track of its own, 16 pages a track in physical sector order, track 17 skipped. the pages
//   around a segment are zeros.
// - the rest is an empty DOS 3.3 volume (VTOC and catalog on track 17) with the tracks
//   above marked in use, so `tisple disk put` can add files beside it.
// at run time the loader keeps to $0800-$0AFF, $0296-$0365 (its nibble table and scratch,
// clear of the vectors at $03F0), $0400-$04FF for aux sectors and zero page $DB-$EF. it
// switches 80store off first, and the language card's bank 2 in to write its segments. then
// it sets the stack to $FF, A = X = Y = 0, interrupts off, the language card to $C082 (rom
// reads, as a direct load starts), turns the motor off and calls the origin. if the program
// returns, the loader shows the text screen and enters the monitor ($FF69); the emulator
// stops there instead.

#include "disk/nibble.hpp"
#include "link/segment.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// main memory segments load in $0B00-$BFFF: the loader is at $0800-$0AFF while it runs and
// uses $0200-$07FF. language card segments load in $D000-$FFFF (bank 2 at $D000), aux ones
// in $0200-$BFFF of aux memory
constexpr uint32_t kBootLoadLow = 0x0B00;
constexpr int kBootLoaderSectors = 3; // $0800-$0AFF
constexpr uint32_t kBootLoadHigh = 0xC000;
constexpr int kBootFirstTrack = 1;
constexpr int kBootCatalogTrack = 17; // DOS 3.3's VTOC and catalog: never program bytes
constexpr int kBootLastTrack = 34;
constexpr size_t kMaxBootSegments = 10;

struct BootDiskInfo {
    uint16_t return_pc = 0; // where the program's final RTS lands
    uint16_t first_page = 0;
    uint32_t pages = 0; // in every segment
    int last_track = 0;
    size_t segments = 0;
};

// one stretch of main memory from origin: the program's entry
bool MakeBootDisk(const std::vector<uint8_t>& program, uint16_t origin, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error);
// any number of segments, up to kMaxBootSegments; each starts on a track of its own, loaded
// in order, then the loader calls entry
bool MakeBootDisk(const std::vector<MemorySegment>& segments, uint16_t entry, DiskSectors& disk,
                  BootDiskInfo& info, std::string& error);

// "track 1" or "tracks 1-3"
std::string BootTracksText(const BootDiskInfo& info);

// whether a disk's track 0 holds a loader of ours; its return pc if so
bool FindBootLoader(const DiskTrack& track0, uint16_t& return_pc);

// the loader itself (boot_loader.cpp): its table names each segment's pages and bank
enum class LoaderBank : uint8_t {
    kMain = 0,
    kCard = 1, // the language card's bank 2, switched in for writes
    kAux = 2,  // read to text page 1, then copied with RAMWRT on
};

struct LoaderSegment {
    uint8_t first_page = 0;
    uint8_t pages = 0;
    LoaderBank bank = LoaderBank::kMain;
};

struct LoaderPlan {
    uint16_t entry = 0;
    std::vector<LoaderSegment> segments; // from track 1 on, each on a track of its own
};

// the loader's 512 bytes for this plan, and where its call returns
bool BuildLoader(const LoaderPlan& plan, uint8_t out[kBootLoaderSectors][kSectorBytes],
                 uint16_t& return_pc, std::string& error);

} // namespace tisple
