#pragma once

// the boot loader's code in pieces (boot_loader.cpp, loader_read.cpp): where it keeps
// things, and the writer its routines share. License: MIT as well as GPL-3.0-or-later, like
// the loader (boot_loader.hpp).

#include "disk/code6502.hpp"

#include <cstdint>
#include <vector>

namespace tisple {

constexpr int kLoaderBase = 0x0800;
constexpr int kCodeEnd = 0x0AF0;
constexpr int kSignature = 0x0AF0;
constexpr int kTable = 0x0200; // + nibble: page-aligned, so the lookups never cross a page
constexpr int kLowBits = 0x0300;
constexpr int kDone = 0x0356;
constexpr int kBounce = 0x0400; // an aux sector, before its copy
constexpr std::uint8_t kLoaderVersion = 2;
constexpr char kSignatureText[] = "TISPLE BOOT";

// zero page
constexpr int kSlot = 0xE0;
constexpr int kHead = 0xE1;   // the head's half-track
constexpr int kTarget = 0xE2; // seek target
constexpr int kNext = 0xE3;   // the half-track being stepped to
constexpr int kFields = 0xE4; // address field, last first: checksum, sector, track, volume
constexpr int kTrack = 0xE8;
constexpr int kPage = 0xE9;  // the page that sector 0 of this track goes to
constexpr int kLeft = 0xEA;  // pages still to read
constexpr int kCount = 0xEB; // sectors wanted on this track
constexpr int kNeed = 0xEC;  // of those, not read yet
constexpr int kTemp = 0xED;
constexpr int kPtr = 0xEE; // and $EF: where the sector goes
constexpr int kWait = 0xDF;
constexpr int kSegment = 0xDE; // the next segment's offset in the table
constexpr int kMode = 0xDD;    // this segment's bank: 0 main, 1 language card, 2 aux
constexpr int kDest = 0xDB;    // and $DC: an aux sector's page

struct Loader {
    Code6502 c;
    std::vector<std::int32_t> latch_reads; // LDX $C08C+slot instructions to patch

    // LDX latch: the address is patched at boot
    void ReadLatch(std::int32_t loop) {
        std::int32_t site = c.Label("latch");
        c.Place(site);
        c.Mem(Mn::LDX, 0xC08C);
        c.Br(Mn::BPL, loop);
        latch_reads.push_back(site);
    }
    // X = slot * 16 + phase * 2 for the half-track in A
    void PhaseIndex() {
        c.Imm(Mn::AND, 3);
        c.OpA(Mn::ASL);
        c.Mem(Mn::ORA, kSlot);
        c.Op(Mn::TAX);
    }
};

// the disk reading routines (loader_read.cpp)
void EmitWait(Loader& l, std::int32_t wait);
void EmitSeek(Loader& l, std::int32_t seek, std::int32_t wait);
void EmitNibble(Loader& l, std::int32_t nibble);
void EmitAddress(Loader& l, std::int32_t address, std::int32_t nibble);
void EmitData(Loader& l, std::int32_t data, std::int32_t nibble);
void EmitNibbleTable(Loader& l);

} // namespace tisple
